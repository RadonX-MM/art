/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ART_RUNTIME_CLASS_TABLE_H_
#define ART_RUNTIME_CLASS_TABLE_H_

#include <string>
#include <utility>
#include <vector>

#include "base/allocator.h"
#include "base/hash_set.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "dex_file.h"
#include "gc_root.h"
#include "object_callbacks.h"
#include "runtime.h"

namespace art {

namespace mirror {
  class ClassLoader;
}  // namespace mirror

class ClassVisitor {
 public:
  virtual ~ClassVisitor() {}
  // Return true to continue visiting.
  virtual bool Visit(mirror::Class* klass) = 0;
};

// Each loader has a ClassTable
class ClassTable {
 public:
  ClassTable();

  // Used by image writer for checking.
  bool Contains(mirror::Class* klass)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Freeze the current class tables by allocating a new table and never updating or modifying the
  // existing table. This helps prevents dirty pages after caused by inserting after zygote fork.
  void FreezeSnapshot()
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Returns the number of classes in previous snapshots.
  size_t NumZygoteClasses() const SHARED_REQUIRES(Locks::classlinker_classes_lock_);

  // Returns all off the classes in the lastest snapshot.
  size_t NumNonZygoteClasses() const SHARED_REQUIRES(Locks::classlinker_classes_lock_);

  // Update a class in the table with the new class. Returns the existing class which was replaced.
  mirror::Class* UpdateClass(const char* descriptor, mirror::Class* new_klass, size_t hash)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // NO_THREAD_SAFETY_ANALYSIS for object marking requiring heap bitmap lock.
  template<class Visitor>
  void VisitRoots(Visitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS
      SHARED_REQUIRES(Locks::classlinker_classes_lock_, Locks::mutator_lock_);
  template<class Visitor>
  void VisitRoots(const Visitor& visitor)
      NO_THREAD_SAFETY_ANALYSIS
      SHARED_REQUIRES(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  // Return false if the callback told us to exit.
  bool Visit(ClassVisitor* visitor)
      SHARED_REQUIRES(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  mirror::Class* Lookup(const char* descriptor, size_t hash)
      SHARED_REQUIRES(Locks::classlinker_classes_lock_, Locks::mutator_lock_);

  void Insert(mirror::Class* klass)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void InsertWithHash(mirror::Class* klass, size_t hash)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Returns true if the class was found and removed, false otherwise.
  bool Remove(const char* descriptor)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if we inserted the dex file, false if it already exists.
  bool InsertDexFile(mirror::Object* dex_file)
      REQUIRES(Locks::classlinker_classes_lock_)
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  class ClassDescriptorHashEquals {
   public:
    // Same class loader and descriptor.
    std::size_t operator()(const GcRoot<mirror::Class>& root) const NO_THREAD_SAFETY_ANALYSIS;
    bool operator()(const GcRoot<mirror::Class>& a, const GcRoot<mirror::Class>& b) const
        NO_THREAD_SAFETY_ANALYSIS;;
    // Same descriptor.
    bool operator()(const GcRoot<mirror::Class>& a, const char* descriptor) const
        NO_THREAD_SAFETY_ANALYSIS;
    std::size_t operator()(const char* descriptor) const NO_THREAD_SAFETY_ANALYSIS;
  };
  class GcRootEmptyFn {
   public:
    void MakeEmpty(GcRoot<mirror::Class>& item) const {
      item = GcRoot<mirror::Class>();
    }
    bool IsEmpty(const GcRoot<mirror::Class>& item) const {
      return item.IsNull();
    }
  };
  // hash set which hashes class descriptor, and compares descriptors and class loaders. Results
  // should be compared for a matching Class descriptor and class loader.
  typedef HashSet<GcRoot<mirror::Class>, GcRootEmptyFn, ClassDescriptorHashEquals,
      ClassDescriptorHashEquals, TrackingAllocator<GcRoot<mirror::Class>, kAllocatorTagClassTable>>
      ClassSet;

  // TODO: shard lock to have one per class loader.
  // We have a vector to help prevent dirty pages after the zygote forks by calling FreezeSnapshot.
  std::vector<ClassSet> classes_ GUARDED_BY(Locks::classlinker_classes_lock_);
  // Dex files used by the class loader which may not be owned by the class loader. We keep these
  // live so that we do not have issues closing any of the dex files.
  std::vector<GcRoot<mirror::Object>> dex_files_ GUARDED_BY(Locks::classlinker_classes_lock_);
};

}  // namespace art

#endif  // ART_RUNTIME_CLASS_TABLE_H_
