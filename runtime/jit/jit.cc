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

#include "jit.h"

#include <dlfcn.h>

#include "art_method-inl.h"
#include "debugger.h"
#include "entrypoints/runtime_asm_entrypoints.h"
#include "interpreter/interpreter.h"
#include "jit_code_cache.h"
#include "jit_instrumentation.h"
#include "oat_file_manager.h"
#include "offline_profiling_info.h"
#include "runtime.h"
#include "runtime_options.h"
#include "utils.h"

namespace art {
namespace jit {

JitOptions* JitOptions::CreateFromRuntimeArguments(const RuntimeArgumentMap& options) {
  auto* jit_options = new JitOptions;
  jit_options->use_jit_ = options.GetOrDefault(RuntimeArgumentMap::UseJIT);
  jit_options->code_cache_initial_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheInitialCapacity);
  jit_options->code_cache_max_capacity_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCodeCacheMaxCapacity);
  jit_options->compile_threshold_ =
      options.GetOrDefault(RuntimeArgumentMap::JITCompileThreshold);
  jit_options->warmup_threshold_ =
      options.GetOrDefault(RuntimeArgumentMap::JITWarmupThreshold);
  jit_options->dump_info_on_shutdown_ =
      options.Exists(RuntimeArgumentMap::DumpJITInfoOnShutdown);
  jit_options->save_profiling_info_ =
      options.GetOrDefault(RuntimeArgumentMap::JITSaveProfilingInfo);;
  return jit_options;
}

void Jit::DumpInfo(std::ostream& os) {
  os << "Code cache size=" << PrettySize(code_cache_->CodeCacheSize())
     << " data cache size=" << PrettySize(code_cache_->DataCacheSize())
     << " number of compiled code=" << code_cache_->NumberOfCompiledCode()
     << "\n";
  cumulative_timings_.Dump(os);
}

void Jit::AddTimingLogger(const TimingLogger& logger) {
  cumulative_timings_.AddLogger(logger);
}

Jit::Jit()
    : jit_library_handle_(nullptr), jit_compiler_handle_(nullptr), jit_load_(nullptr),
      jit_compile_method_(nullptr), dump_info_on_shutdown_(false),
      cumulative_timings_("JIT timings") {
}

Jit* Jit::Create(JitOptions* options, std::string* error_msg) {
  std::unique_ptr<Jit> jit(new Jit);
  jit->dump_info_on_shutdown_ = options->DumpJitInfoOnShutdown();
  if (!jit->LoadCompiler(error_msg)) {
    return nullptr;
  }
  jit->code_cache_.reset(JitCodeCache::Create(
      options->GetCodeCacheInitialCapacity(), options->GetCodeCacheMaxCapacity(), error_msg));
  if (jit->GetCodeCache() == nullptr) {
    return nullptr;
  }
  jit->offline_profile_info_.reset(nullptr);
  if (options->GetSaveProfilingInfo()) {
    jit->offline_profile_info_.reset(new OfflineProfilingInfo());
  }
  LOG(INFO) << "JIT created with initial_capacity="
      << PrettySize(options->GetCodeCacheInitialCapacity())
      << ", max_capacity=" << PrettySize(options->GetCodeCacheMaxCapacity())
      << ", compile_threshold=" << options->GetCompileThreshold();
  return jit.release();
}

bool Jit::LoadCompiler(std::string* error_msg) {
  jit_library_handle_ = dlopen(
      kIsDebugBuild ? "libartd-compiler.so" : "libart-compiler.so", RTLD_NOW);
  if (jit_library_handle_ == nullptr) {
    std::ostringstream oss;
    oss << "JIT could not load libart-compiler.so: " << dlerror();
    *error_msg = oss.str();
    return false;
  }
  jit_load_ = reinterpret_cast<void* (*)(CompilerCallbacks**)>(
      dlsym(jit_library_handle_, "jit_load"));
  if (jit_load_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_load entry point";
    return false;
  }
  jit_unload_ = reinterpret_cast<void (*)(void*)>(
      dlsym(jit_library_handle_, "jit_unload"));
  if (jit_unload_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_unload entry point";
    return false;
  }
  jit_compile_method_ = reinterpret_cast<bool (*)(void*, ArtMethod*, Thread*)>(
      dlsym(jit_library_handle_, "jit_compile_method"));
  if (jit_compile_method_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't find jit_compile_method entry point";
    return false;
  }
  CompilerCallbacks* callbacks = nullptr;
  VLOG(jit) << "Calling JitLoad interpreter_only="
      << Runtime::Current()->GetInstrumentation()->InterpretOnly();
  jit_compiler_handle_ = (jit_load_)(&callbacks);
  if (jit_compiler_handle_ == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT couldn't load compiler";
    return false;
  }
  if (callbacks == nullptr) {
    dlclose(jit_library_handle_);
    *error_msg = "JIT compiler callbacks were not set";
    jit_compiler_handle_ = nullptr;
    return false;
  }
  compiler_callbacks_ = callbacks;
  return true;
}

bool Jit::CompileMethod(ArtMethod* method, Thread* self) {
  DCHECK(!method->IsRuntimeMethod());
  if (Dbg::IsDebuggerActive() && Dbg::MethodHasAnyBreakpoints(method)) {
    VLOG(jit) << "JIT not compiling " << PrettyMethod(method) << " due to breakpoint";
    return false;
  }
  return jit_compile_method_(jit_compiler_handle_, method, self);
}

void Jit::CreateThreadPool() {
  CHECK(instrumentation_cache_.get() != nullptr);
  instrumentation_cache_->CreateThreadPool();
}

void Jit::DeleteThreadPool() {
  if (instrumentation_cache_.get() != nullptr) {
    instrumentation_cache_->DeleteThreadPool(Thread::Current());
  }
}

void Jit::SaveProfilingInfo(const std::string& filename) {
  if (offline_profile_info_ == nullptr) {
    return;
  }
  // Note that we can't check the PrimaryOatFile when constructing the offline_profilie_info_
  // because it becomes known to the Runtime after we create and initialize the JIT.
  const OatFile* primary_oat_file = Runtime::Current()->GetOatFileManager().GetPrimaryOatFile();
  if (primary_oat_file == nullptr) {
    LOG(WARNING) << "Couldn't find a primary oat file when trying to save profile info to "
                 << filename;
    return;
  }

  uint64_t last_update_ns = code_cache_->GetLastUpdateTimeNs();
  if (offline_profile_info_->NeedsSaving(last_update_ns)) {
    VLOG(profiler) << "Iniate save profiling information to: " << filename;
    std::set<ArtMethod*> methods;
    {
      ScopedObjectAccess soa(Thread::Current());
      code_cache_->GetCompiledArtMethods(primary_oat_file, methods);
    }
    offline_profile_info_->SaveProfilingInfo(filename, last_update_ns, methods);
  } else {
    VLOG(profiler) << "No need to save profiling information to: " << filename;
  }
}

Jit::~Jit() {
  if (dump_info_on_shutdown_) {
    DumpInfo(LOG(INFO));
  }
  DeleteThreadPool();
  if (jit_compiler_handle_ != nullptr) {
    jit_unload_(jit_compiler_handle_);
  }
  if (jit_library_handle_ != nullptr) {
    dlclose(jit_library_handle_);
  }
}

void Jit::CreateInstrumentationCache(size_t compile_threshold, size_t warmup_threshold) {
  CHECK_GT(compile_threshold, 0U);
  instrumentation_cache_.reset(
      new jit::JitInstrumentationCache(compile_threshold, warmup_threshold));
}

}  // namespace jit
}  // namespace art
