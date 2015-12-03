/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jit_code_cache.h"

#include <sstream>

#include "art_method-inl.h"
#include "base/time_utils.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "gc/accounting/bitmap-inl.h"
#include "jit/profiling_info.h"
#include "linear_alloc.h"
#include "mem_map.h"
#include "oat_file-inl.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"

namespace art {
namespace jit {

static constexpr int kProtAll = PROT_READ | PROT_WRITE | PROT_EXEC;
static constexpr int kProtData = PROT_READ | PROT_WRITE;
static constexpr int kProtCode = PROT_READ | PROT_EXEC;

#define CHECKED_MPROTECT(memory, size, prot)                \
  do {                                                      \
    int rc = mprotect(memory, size, prot);                  \
    if (UNLIKELY(rc != 0)) {                                \
      errno = rc;                                           \
      PLOG(FATAL) << "Failed to mprotect jit code cache";   \
    }                                                       \
  } while (false)                                           \

JitCodeCache* JitCodeCache::Create(size_t initial_capacity,
                                   size_t max_capacity,
                                   std::string* error_msg) {
  CHECK_GE(max_capacity, initial_capacity);
  // We need to have 32 bit offsets from method headers in code cache which point to things
  // in the data cache. If the maps are more than 4G apart, having multiple maps wouldn't work.
  // Ensure we're below 1 GB to be safe.
  if (max_capacity > 1 * GB) {
    std::ostringstream oss;
    oss << "Maxium code cache capacity is limited to 1 GB, "
        << PrettySize(max_capacity) << " is too big";
    *error_msg = oss.str();
    return nullptr;
  }

  std::string error_str;
  // Map name specific for android_os_Debug.cpp accounting.
  MemMap* data_map = MemMap::MapAnonymous(
    "data-code-cache", nullptr, max_capacity, kProtAll, false, false, &error_str);
  if (data_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << max_capacity;
    *error_msg = oss.str();
    return nullptr;
  }

  // Align both capacities to page size, as that's the unit mspaces use.
  initial_capacity = RoundDown(initial_capacity, 2 * kPageSize);
  max_capacity = RoundDown(max_capacity, 2 * kPageSize);

  // Data cache is 1 / 2 of the map.
  // TODO: Make this variable?
  size_t data_size = max_capacity / 2;
  size_t code_size = max_capacity - data_size;
  DCHECK_EQ(code_size + data_size, max_capacity);
  uint8_t* divider = data_map->Begin() + data_size;

  MemMap* code_map = data_map->RemapAtEnd(divider, "jit-code-cache", kProtAll, &error_str);
  if (code_map == nullptr) {
    std::ostringstream oss;
    oss << "Failed to create read write execute cache: " << error_str << " size=" << max_capacity;
    *error_msg = oss.str();
    return nullptr;
  }
  DCHECK_EQ(code_map->Begin(), divider);
  data_size = initial_capacity / 2;
  code_size = initial_capacity - data_size;
  DCHECK_EQ(code_size + data_size, initial_capacity);
  return new JitCodeCache(code_map, data_map, code_size, data_size, max_capacity);
}

JitCodeCache::JitCodeCache(MemMap* code_map,
                           MemMap* data_map,
                           size_t initial_code_capacity,
                           size_t initial_data_capacity,
                           size_t max_capacity)
    : lock_("Jit code cache", kJitCodeCacheLock),
      lock_cond_("Jit code cache variable", lock_),
      collection_in_progress_(false),
      code_map_(code_map),
      data_map_(data_map),
      max_capacity_(max_capacity),
      current_capacity_(initial_code_capacity + initial_data_capacity),
      code_end_(initial_code_capacity),
      data_end_(initial_data_capacity),
      has_done_one_collection_(false),
      last_update_time_ns_(0) {

  code_mspace_ = create_mspace_with_base(code_map_->Begin(), code_end_, false /*locked*/);
  data_mspace_ = create_mspace_with_base(data_map_->Begin(), data_end_, false /*locked*/);

  if (code_mspace_ == nullptr || data_mspace_ == nullptr) {
    PLOG(FATAL) << "create_mspace_with_base failed";
  }

  SetFootprintLimit(current_capacity_);

  CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  CHECKED_MPROTECT(data_map_->Begin(), data_map_->Size(), kProtData);

  VLOG(jit) << "Created jit code cache: initial data size="
            << PrettySize(initial_data_capacity)
            << ", initial code size="
            << PrettySize(initial_code_capacity);
}

bool JitCodeCache::ContainsPc(const void* ptr) const {
  return code_map_->Begin() <= ptr && ptr < code_map_->End();
}

bool JitCodeCache::ContainsMethod(ArtMethod* method) {
  MutexLock mu(Thread::Current(), lock_);
  for (auto& it : method_code_map_) {
    if (it.second == method) {
      return true;
    }
  }
  return false;
}

class ScopedCodeCacheWrite {
 public:
  explicit ScopedCodeCacheWrite(MemMap* code_map) : code_map_(code_map) {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtAll);
  }
  ~ScopedCodeCacheWrite() {
    CHECKED_MPROTECT(code_map_->Begin(), code_map_->Size(), kProtCode);
  }
 private:
  MemMap* const code_map_;

  DISALLOW_COPY_AND_ASSIGN(ScopedCodeCacheWrite);
};

uint8_t* JitCodeCache::CommitCode(Thread* self,
                                  ArtMethod* method,
                                  const uint8_t* mapping_table,
                                  const uint8_t* vmap_table,
                                  const uint8_t* gc_map,
                                  size_t frame_size_in_bytes,
                                  size_t core_spill_mask,
                                  size_t fp_spill_mask,
                                  const uint8_t* code,
                                  size_t code_size) {
  uint8_t* result = CommitCodeInternal(self,
                                       method,
                                       mapping_table,
                                       vmap_table,
                                       gc_map,
                                       frame_size_in_bytes,
                                       core_spill_mask,
                                       fp_spill_mask,
                                       code,
                                       code_size);
  if (result == nullptr) {
    // Retry.
    GarbageCollectCache(self);
    result = CommitCodeInternal(self,
                                method,
                                mapping_table,
                                vmap_table,
                                gc_map,
                                frame_size_in_bytes,
                                core_spill_mask,
                                fp_spill_mask,
                                code,
                                code_size);
  }
  return result;
}

bool JitCodeCache::WaitForPotentialCollectionToComplete(Thread* self) {
  bool in_collection = false;
  while (collection_in_progress_) {
    in_collection = true;
    lock_cond_.Wait(self);
  }
  return in_collection;
}

static uintptr_t FromCodeToAllocation(const void* code) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  return reinterpret_cast<uintptr_t>(code) - RoundUp(sizeof(OatQuickMethodHeader), alignment);
}

void JitCodeCache::FreeCode(const void* code_ptr, ArtMethod* method ATTRIBUTE_UNUSED) {
  uintptr_t allocation = FromCodeToAllocation(code_ptr);
  const OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
  const uint8_t* data = method_header->GetNativeGcMap();
  if (data != nullptr) {
    mspace_free(data_mspace_, const_cast<uint8_t*>(data));
  }
  data = method_header->GetMappingTable();
  if (data != nullptr) {
    mspace_free(data_mspace_, const_cast<uint8_t*>(data));
  }
  // Use the offset directly to prevent sanity check that the method is
  // compiled with optimizing.
  // TODO(ngeoffray): Clean up.
  if (method_header->vmap_table_offset_ != 0) {
    data = method_header->code_ - method_header->vmap_table_offset_;
    mspace_free(data_mspace_, const_cast<uint8_t*>(data));
  }
  mspace_free(code_mspace_, reinterpret_cast<uint8_t*>(allocation));
}

void JitCodeCache::RemoveMethodsIn(Thread* self, const LinearAlloc& alloc) {
  MutexLock mu(self, lock_);
  // We do not check if a code cache GC is in progress, as this method comes
  // with the classlinker_classes_lock_ held, and suspending ourselves could
  // lead to a deadlock.
  {
    ScopedCodeCacheWrite scc(code_map_.get());
    for (auto it = method_code_map_.begin(); it != method_code_map_.end();) {
      if (alloc.ContainsUnsafe(it->second)) {
        FreeCode(it->first, it->second);
        it = method_code_map_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto it = profiling_infos_.begin(); it != profiling_infos_.end();) {
    ProfilingInfo* info = *it;
    if (alloc.ContainsUnsafe(info->GetMethod())) {
      info->GetMethod()->SetProfilingInfo(nullptr);
      mspace_free(data_mspace_, reinterpret_cast<uint8_t*>(info));
      it = profiling_infos_.erase(it);
    } else {
      ++it;
    }
  }
}

uint8_t* JitCodeCache::CommitCodeInternal(Thread* self,
                                          ArtMethod* method,
                                          const uint8_t* mapping_table,
                                          const uint8_t* vmap_table,
                                          const uint8_t* gc_map,
                                          size_t frame_size_in_bytes,
                                          size_t core_spill_mask,
                                          size_t fp_spill_mask,
                                          const uint8_t* code,
                                          size_t code_size) {
  size_t alignment = GetInstructionSetAlignment(kRuntimeISA);
  // Ensure the header ends up at expected instruction alignment.
  size_t header_size = RoundUp(sizeof(OatQuickMethodHeader), alignment);
  size_t total_size = header_size + code_size;

  OatQuickMethodHeader* method_header = nullptr;
  uint8_t* code_ptr = nullptr;
  {
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    WaitForPotentialCollectionToComplete(self);
    {
      ScopedCodeCacheWrite scc(code_map_.get());
      uint8_t* result = reinterpret_cast<uint8_t*>(
          mspace_memalign(code_mspace_, alignment, total_size));
      if (result == nullptr) {
        return nullptr;
      }
      code_ptr = result + header_size;
      DCHECK_ALIGNED_PARAM(reinterpret_cast<uintptr_t>(code_ptr), alignment);

      std::copy(code, code + code_size, code_ptr);
      method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
      new (method_header) OatQuickMethodHeader(
          (mapping_table == nullptr) ? 0 : code_ptr - mapping_table,
          (vmap_table == nullptr) ? 0 : code_ptr - vmap_table,
          (gc_map == nullptr) ? 0 : code_ptr - gc_map,
          frame_size_in_bytes,
          core_spill_mask,
          fp_spill_mask,
          code_size);
    }

    __builtin___clear_cache(reinterpret_cast<char*>(code_ptr),
                            reinterpret_cast<char*>(code_ptr + code_size));
  }
  // We need to update the entry point in the runnable state for the instrumentation.
  {
    MutexLock mu(self, lock_);
    method_code_map_.Put(code_ptr, method);
    Runtime::Current()->GetInstrumentation()->UpdateMethodsCode(
        method, method_header->GetEntryPoint());
    if (collection_in_progress_) {
      // We need to update the live bitmap if there is a GC to ensure it sees this new
      // code.
      GetLiveBitmap()->AtomicTestAndSet(FromCodeToAllocation(code_ptr));
    }
    last_update_time_ns_ = NanoTime();
    VLOG(jit)
        << "JIT added "
        << PrettyMethod(method) << "@" << method
        << " ccache_size=" << PrettySize(CodeCacheSizeLocked()) << ": "
        << " dcache_size=" << PrettySize(DataCacheSizeLocked()) << ": "
        << reinterpret_cast<const void*>(method_header->GetEntryPoint()) << ","
        << reinterpret_cast<const void*>(method_header->GetEntryPoint() + method_header->code_size_);
  }

  return reinterpret_cast<uint8_t*>(method_header);
}

size_t JitCodeCache::CodeCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  return CodeCacheSizeLocked();
}

size_t JitCodeCache::CodeCacheSizeLocked() {
  size_t bytes_allocated = 0;
  mspace_inspect_all(code_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

size_t JitCodeCache::DataCacheSize() {
  MutexLock mu(Thread::Current(), lock_);
  return DataCacheSizeLocked();
}

size_t JitCodeCache::DataCacheSizeLocked() {
  size_t bytes_allocated = 0;
  mspace_inspect_all(data_mspace_, DlmallocBytesAllocatedCallback, &bytes_allocated);
  return bytes_allocated;
}

size_t JitCodeCache::NumberOfCompiledCode() {
  MutexLock mu(Thread::Current(), lock_);
  return method_code_map_.size();
}

void JitCodeCache::ClearData(Thread* self, void* data) {
  MutexLock mu(self, lock_);
  mspace_free(data_mspace_, data);
}

uint8_t* JitCodeCache::ReserveData(Thread* self, size_t size) {
  size = RoundUp(size, sizeof(void*));
  uint8_t* result = nullptr;

  {
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    WaitForPotentialCollectionToComplete(self);
    result = reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, size));
  }

  if (result == nullptr) {
    // Retry.
    GarbageCollectCache(self);
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    WaitForPotentialCollectionToComplete(self);
    result = reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, size));
  }

  return result;
}

uint8_t* JitCodeCache::AddDataArray(Thread* self, const uint8_t* begin, const uint8_t* end) {
  uint8_t* result = ReserveData(self, end - begin);
  if (result == nullptr) {
    return nullptr;  // Out of space in the data cache.
  }
  std::copy(begin, end, result);
  return result;
}

class MarkCodeVisitor FINAL : public StackVisitor {
 public:
  MarkCodeVisitor(Thread* thread_in, JitCodeCache* code_cache_in)
      : StackVisitor(thread_in, nullptr, StackVisitor::StackWalkKind::kSkipInlinedFrames),
        code_cache_(code_cache_in),
        bitmap_(code_cache_->GetLiveBitmap()) {}

  bool VisitFrame() OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    const OatQuickMethodHeader* method_header = GetCurrentOatQuickMethodHeader();
    if (method_header == nullptr) {
      return true;
    }
    const void* code = method_header->GetCode();
    if (code_cache_->ContainsPc(code)) {
      // Use the atomic set version, as multiple threads are executing this code.
      bitmap_->AtomicTestAndSet(FromCodeToAllocation(code));
    }
    return true;
  }

 private:
  JitCodeCache* const code_cache_;
  CodeCacheBitmap* const bitmap_;
};

class MarkCodeClosure FINAL : public Closure {
 public:
  MarkCodeClosure(JitCodeCache* code_cache, Barrier* barrier)
      : code_cache_(code_cache), barrier_(barrier) {}

  void Run(Thread* thread) OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    DCHECK(thread == Thread::Current() || thread->IsSuspended());
    MarkCodeVisitor visitor(thread, code_cache_);
    visitor.WalkStack();
    if (kIsDebugBuild) {
      // The stack walking code queries the side instrumentation stack if it
      // sees an instrumentation exit pc, so the JIT code of methods in that stack
      // must have been seen. We sanity check this below.
      for (const instrumentation::InstrumentationStackFrame& frame
              : *thread->GetInstrumentationStack()) {
        // The 'method_' in InstrumentationStackFrame is the one that has return_pc_ in
        // its stack frame, it is not the method owning return_pc_. We just pass null to
        // LookupMethodHeader: the method is only checked against in debug builds.
        OatQuickMethodHeader* method_header =
            code_cache_->LookupMethodHeader(frame.return_pc_, nullptr);
        if (method_header != nullptr) {
          const void* code = method_header->GetCode();
          CHECK(code_cache_->GetLiveBitmap()->Test(FromCodeToAllocation(code)));
        }
      }
    }
    barrier_->Pass(Thread::Current());
  }

 private:
  JitCodeCache* const code_cache_;
  Barrier* const barrier_;
};

void JitCodeCache::NotifyCollectionDone(Thread* self) {
  collection_in_progress_ = false;
  lock_cond_.Broadcast(self);
}

void JitCodeCache::SetFootprintLimit(size_t new_footprint) {
  size_t per_space_footprint = new_footprint / 2;
  DCHECK(IsAlignedParam(per_space_footprint, kPageSize));
  DCHECK_EQ(per_space_footprint * 2, new_footprint);
  mspace_set_footprint_limit(data_mspace_, per_space_footprint);
  {
    ScopedCodeCacheWrite scc(code_map_.get());
    mspace_set_footprint_limit(code_mspace_, per_space_footprint);
  }
}

bool JitCodeCache::IncreaseCodeCacheCapacity() {
  if (current_capacity_ == max_capacity_) {
    return false;
  }

  // Double the capacity if we're below 1MB, or increase it by 1MB if
  // we're above.
  if (current_capacity_ < 1 * MB) {
    current_capacity_ *= 2;
  } else {
    current_capacity_ += 1 * MB;
  }
  if (current_capacity_ > max_capacity_) {
    current_capacity_ = max_capacity_;
  }

  if (!kIsDebugBuild || VLOG_IS_ON(jit)) {
    LOG(INFO) << "Increasing code cache capacity to " << PrettySize(current_capacity_);
  }

  SetFootprintLimit(current_capacity_);

  return true;
}

void JitCodeCache::GarbageCollectCache(Thread* self) {
  instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();

  // Wait for an existing collection, or let everyone know we are starting one.
  {
    ScopedThreadSuspension sts(self, kSuspended);
    MutexLock mu(self, lock_);
    if (WaitForPotentialCollectionToComplete(self)) {
      return;
    } else {
      collection_in_progress_ = true;
    }
  }

  // Check if we just need to grow the capacity. If we don't, allocate the bitmap while
  // we hold the lock.
  {
    MutexLock mu(self, lock_);
    if (has_done_one_collection_ && IncreaseCodeCacheCapacity()) {
      has_done_one_collection_ = false;
      NotifyCollectionDone(self);
      return;
    } else {
      live_bitmap_.reset(CodeCacheBitmap::Create(
          "code-cache-bitmap",
          reinterpret_cast<uintptr_t>(code_map_->Begin()),
          reinterpret_cast<uintptr_t>(code_map_->Begin() + current_capacity_ / 2)));
    }
  }

  if (!kIsDebugBuild || VLOG_IS_ON(jit)) {
    LOG(INFO) << "Clearing code cache, code="
              << PrettySize(CodeCacheSize())
              << ", data=" << PrettySize(DataCacheSize());
  }
  // Walk over all compiled methods and set the entry points of these
  // methods to interpreter.
  {
    MutexLock mu(self, lock_);
    for (auto& it : method_code_map_) {
      instrumentation->UpdateMethodsCode(it.second, GetQuickToInterpreterBridge());
    }
    for (ProfilingInfo* info : profiling_infos_) {
      info->GetMethod()->SetProfilingInfo(nullptr);
    }
  }

  // Run a checkpoint on all threads to mark the JIT compiled code they are running.
  {
    Barrier barrier(0);
    size_t threads_running_checkpoint = 0;
    MarkCodeClosure closure(this, &barrier);
    threads_running_checkpoint =
        Runtime::Current()->GetThreadList()->RunCheckpoint(&closure);
    // Now that we have run our checkpoint, move to a suspended state and wait
    // for other threads to run the checkpoint.
    ScopedThreadSuspension sts(self, kSuspended);
    if (threads_running_checkpoint != 0) {
      barrier.Increment(self, threads_running_checkpoint);
    }
  }

  {
    MutexLock mu(self, lock_);
    // Free unused compiled code, and restore the entry point of used compiled code.
    {
      ScopedCodeCacheWrite scc(code_map_.get());
      for (auto it = method_code_map_.begin(); it != method_code_map_.end();) {
        const void* code_ptr = it->first;
        ArtMethod* method = it->second;
        uintptr_t allocation = FromCodeToAllocation(code_ptr);
        const OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
        if (GetLiveBitmap()->Test(allocation)) {
          instrumentation->UpdateMethodsCode(method, method_header->GetEntryPoint());
          ++it;
        } else {
          method->ClearCounter();
          DCHECK_NE(method->GetEntryPointFromQuickCompiledCode(), method_header->GetEntryPoint());
          FreeCode(code_ptr, method);
          it = method_code_map_.erase(it);
        }
      }
    }

    // Free all profiling info.
    for (ProfilingInfo* info : profiling_infos_) {
      DCHECK(info->GetMethod()->GetProfilingInfo(sizeof(void*)) == nullptr);
      mspace_free(data_mspace_, reinterpret_cast<uint8_t*>(info));
    }
    profiling_infos_.clear();

    live_bitmap_.reset(nullptr);
    has_done_one_collection_ = true;
    NotifyCollectionDone(self);
  }

  if (!kIsDebugBuild || VLOG_IS_ON(jit)) {
    LOG(INFO) << "After clearing code cache, code="
              << PrettySize(CodeCacheSize())
              << ", data=" << PrettySize(DataCacheSize());
  }
}


OatQuickMethodHeader* JitCodeCache::LookupMethodHeader(uintptr_t pc, ArtMethod* method) {
  static_assert(kRuntimeISA != kThumb2, "kThumb2 cannot be a runtime ISA");
  if (kRuntimeISA == kArm) {
    // On Thumb-2, the pc is offset by one.
    --pc;
  }
  if (!ContainsPc(reinterpret_cast<const void*>(pc))) {
    return nullptr;
  }

  MutexLock mu(Thread::Current(), lock_);
  if (method_code_map_.empty()) {
    return nullptr;
  }
  auto it = method_code_map_.lower_bound(reinterpret_cast<const void*>(pc));
  --it;

  const void* code_ptr = it->first;
  OatQuickMethodHeader* method_header = OatQuickMethodHeader::FromCodePointer(code_ptr);
  if (!method_header->Contains(pc)) {
    return nullptr;
  }
  if (kIsDebugBuild && method != nullptr) {
    DCHECK_EQ(it->second, method)
        << PrettyMethod(method) << " " << PrettyMethod(it->second) << " " << std::hex << pc;
  }
  return method_header;
}

ProfilingInfo* JitCodeCache::AddProfilingInfo(Thread* self,
                                              ArtMethod* method,
                                              const std::vector<uint32_t>& entries,
                                              bool retry_allocation) {
  ProfilingInfo* info = AddProfilingInfoInternal(self, method, entries);

  if (info == nullptr && retry_allocation) {
    GarbageCollectCache(self);
    info = AddProfilingInfoInternal(self, method, entries);
  }
  return info;
}

ProfilingInfo* JitCodeCache::AddProfilingInfoInternal(Thread* self,
                                                      ArtMethod* method,
                                                      const std::vector<uint32_t>& entries) {
  size_t profile_info_size = RoundUp(
      sizeof(ProfilingInfo) + sizeof(ProfilingInfo::InlineCache) * entries.size(),
      sizeof(void*));
  ScopedThreadSuspension sts(self, kSuspended);
  MutexLock mu(self, lock_);
  WaitForPotentialCollectionToComplete(self);

  // Check whether some other thread has concurrently created it.
  ProfilingInfo* info = method->GetProfilingInfo(sizeof(void*));
  if (info != nullptr) {
    return info;
  }

  uint8_t* data = reinterpret_cast<uint8_t*>(mspace_malloc(data_mspace_, profile_info_size));
  if (data == nullptr) {
    return nullptr;
  }
  info = new (data) ProfilingInfo(method, entries);
  method->SetProfilingInfo(info);
  profiling_infos_.push_back(info);
  return info;
}

// NO_THREAD_SAFETY_ANALYSIS as this is called from mspace code, at which point the lock
// is already held.
void* JitCodeCache::MoreCore(const void* mspace, intptr_t increment) NO_THREAD_SAFETY_ANALYSIS {
  if (code_mspace_ == mspace) {
    size_t result = code_end_;
    code_end_ += increment;
    return reinterpret_cast<void*>(result + code_map_->Begin());
  } else {
    DCHECK_EQ(data_mspace_, mspace);
    size_t result = data_end_;
    data_end_ += increment;
    return reinterpret_cast<void*>(result + data_map_->Begin());
  }
}

void JitCodeCache::GetCompiledArtMethods(const OatFile* oat_file,
                                         std::set<ArtMethod*>& methods) {
  MutexLock mu(Thread::Current(), lock_);
  for (auto it : method_code_map_) {
    if (it.second->GetDexFile()->GetOatDexFile()->GetOatFile() == oat_file) {
      methods.insert(it.second);
    }
  }
}

uint64_t JitCodeCache::GetLastUpdateTimeNs() {
  MutexLock mu(Thread::Current(), lock_);
  return last_update_time_ns_;
}
}  // namespace jit
}  // namespace art
