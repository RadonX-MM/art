/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_COMPILER_IMAGE_WRITER_H_
#define ART_COMPILER_IMAGE_WRITER_H_

#include <stdint.h>
#include "base/memory_tool.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <ostream>

#include "base/bit_utils.h"
#include "base/macros.h"
#include "driver/compiler_driver.h"
#include "gc/space/space.h"
#include "length_prefixed_array.h"
#include "lock_word.h"
#include "mem_map.h"
#include "oat_file.h"
#include "mirror/dex_cache.h"
#include "os.h"
#include "safe_map.h"
#include "utils.h"

namespace art {
namespace gc {
namespace space {
class ImageSpace;
}  // namespace space
}  // namespace gc

static constexpr int kInvalidImageFd = -1;

// Write a Space built during compilation for use during execution.
class ImageWriter FINAL {
 public:
  ImageWriter(const CompilerDriver& compiler_driver,
              uintptr_t image_begin,
              bool compile_pic,
              bool compile_app_image)
      : compiler_driver_(compiler_driver),
        image_begin_(reinterpret_cast<uint8_t*>(image_begin)),
        image_end_(0),
        image_objects_offset_begin_(0),
        image_roots_address_(0),
        oat_file_(nullptr),
        oat_data_begin_(nullptr),
        compile_pic_(compile_pic),
        compile_app_image_(compile_app_image),
        boot_image_space_(nullptr),
        target_ptr_size_(InstructionSetPointerSize(compiler_driver_.GetInstructionSet())),
        bin_slot_sizes_(),
        bin_slot_offsets_(),
        bin_slot_count_(),
        intern_table_bytes_(0u),
        image_method_array_(ImageHeader::kImageMethodsCount),
        dirty_methods_(0u),
        clean_methods_(0u) {
    CHECK_NE(image_begin, 0U);
    std::fill_n(image_methods_, arraysize(image_methods_), nullptr);
    std::fill_n(oat_address_offsets_, arraysize(oat_address_offsets_), 0);
  }

  ~ImageWriter() {
  }

  bool PrepareImageAddressSpace();

  bool IsImageAddressSpaceReady() const {
    return image_roots_address_ != 0u;
  }

  template <typename T>
  T* GetImageAddress(T* object) const SHARED_REQUIRES(Locks::mutator_lock_) {
    return (object == nullptr || IsInBootImage(object))
        ? object
        : reinterpret_cast<T*>(image_begin_ + GetImageOffset(object));
  }

  ArtMethod* GetImageMethodAddress(ArtMethod* method) SHARED_REQUIRES(Locks::mutator_lock_);

  template <typename PtrType>
  PtrType GetDexCacheArrayElementImageAddress(const DexFile* dex_file, uint32_t offset)
      const SHARED_REQUIRES(Locks::mutator_lock_) {
    auto it = dex_cache_array_starts_.find(dex_file);
    DCHECK(it != dex_cache_array_starts_.end());
    return reinterpret_cast<PtrType>(
        image_begin_ + bin_slot_offsets_[kBinDexCacheArray] + it->second + offset);
  }

  uint8_t* GetOatFileBegin() const;

  // If image_fd is not kInvalidImageFd, then we use that for the file. Otherwise we open
  // image_filename.
  bool Write(int image_fd,
             const std::string& image_filename,
             const std::string& oat_filename,
             const std::string& oat_location)
      REQUIRES(!Locks::mutator_lock_);

  uintptr_t GetOatDataBegin() {
    return reinterpret_cast<uintptr_t>(oat_data_begin_);
  }

 private:
  bool AllocMemory();

  // Mark the objects defined in this space in the given live bitmap.
  void RecordImageAllocations() SHARED_REQUIRES(Locks::mutator_lock_);

  // Classify different kinds of bins that objects end up getting packed into during image writing.
  enum Bin {
    // Likely-clean:
    kBinString,                        // [String] Almost always immutable (except for obj header).
    // Unknown mix of clean/dirty:
    kBinRegular,
    // Likely-dirty:
    // All classes get their own bins since their fields often dirty
    kBinClassInitializedFinalStatics,  // Class initializers have been run, no non-final statics
    kBinClassInitialized,         // Class initializers have been run
    kBinClassVerified,            // Class verified, but initializers haven't been run
    // Add more bins here if we add more segregation code.
    // Non mirror fields must be below.
    // ArtFields should be always clean.
    kBinArtField,
    // If the class is initialized, then the ArtMethods are probably clean.
    kBinArtMethodClean,
    // ArtMethods may be dirty if the class has native methods or a declaring class that isn't
    // initialized.
    kBinArtMethodDirty,
    // Dex cache arrays have a special slot for PC-relative addressing. Since they are
    // huge, and as such their dirtiness is not important for the clean/dirty separation,
    // we arbitrarily keep them at the end of the native data.
    kBinDexCacheArray,            // Arrays belonging to dex cache.
    kBinSize,
    // Number of bins which are for mirror objects.
    kBinMirrorCount = kBinArtField,
  };
  friend std::ostream& operator<<(std::ostream& stream, const Bin& bin);

  enum NativeObjectRelocationType {
    kNativeObjectRelocationTypeArtField,
    kNativeObjectRelocationTypeArtFieldArray,
    kNativeObjectRelocationTypeArtMethodClean,
    kNativeObjectRelocationTypeArtMethodArrayClean,
    kNativeObjectRelocationTypeArtMethodDirty,
    kNativeObjectRelocationTypeArtMethodArrayDirty,
    kNativeObjectRelocationTypeDexCacheArray,
  };
  friend std::ostream& operator<<(std::ostream& stream, const NativeObjectRelocationType& type);

  enum OatAddress {
    kOatAddressInterpreterToInterpreterBridge,
    kOatAddressInterpreterToCompiledCodeBridge,
    kOatAddressJNIDlsymLookup,
    kOatAddressQuickGenericJNITrampoline,
    kOatAddressQuickIMTConflictTrampoline,
    kOatAddressQuickResolutionTrampoline,
    kOatAddressQuickToInterpreterBridge,
    // Number of elements in the enum.
    kOatAddressCount,
  };
  friend std::ostream& operator<<(std::ostream& stream, const OatAddress& oat_address);

  static constexpr size_t kBinBits = MinimumBitsToStore<uint32_t>(kBinMirrorCount - 1);
  // uint32 = typeof(lockword_)
  // Subtract read barrier bits since we want these to remain 0, or else it may result in DCHECK
  // failures due to invalid read barrier bits during object field reads.
  static const size_t kBinShift = BitSizeOf<uint32_t>() - kBinBits -
      LockWord::kReadBarrierStateSize;
  // 111000.....0
  static const size_t kBinMask = ((static_cast<size_t>(1) << kBinBits) - 1) << kBinShift;

  // We use the lock word to store the bin # and bin index of the object in the image.
  //
  // The struct size must be exactly sizeof(LockWord), currently 32-bits, since this will end up
  // stored in the lock word bit-for-bit when object forwarding addresses are being calculated.
  struct BinSlot {
    explicit BinSlot(uint32_t lockword);
    BinSlot(Bin bin, uint32_t index);

    // The bin an object belongs to, i.e. regular, class/verified, class/initialized, etc.
    Bin GetBin() const;
    // The offset in bytes from the beginning of the bin. Aligned to object size.
    uint32_t GetIndex() const;
    // Pack into a single uint32_t, for storing into a lock word.
    uint32_t Uint32Value() const { return lockword_; }
    // Comparison operator for map support
    bool operator<(const BinSlot& other) const  { return lockword_ < other.lockword_; }

  private:
    // Must be the same size as LockWord, any larger and we would truncate the data.
    const uint32_t lockword_;
  };

  // We use the lock word to store the offset of the object in the image.
  void AssignImageOffset(mirror::Object* object, BinSlot bin_slot)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void SetImageOffset(mirror::Object* object, size_t offset)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool IsImageOffsetAssigned(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_);
  size_t GetImageOffset(mirror::Object* object) const SHARED_REQUIRES(Locks::mutator_lock_);
  void UpdateImageOffset(mirror::Object* obj, uintptr_t offset)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void PrepareDexCacheArraySlots() SHARED_REQUIRES(Locks::mutator_lock_);
  void AssignImageBinSlot(mirror::Object* object) SHARED_REQUIRES(Locks::mutator_lock_);
  void SetImageBinSlot(mirror::Object* object, BinSlot bin_slot)
      SHARED_REQUIRES(Locks::mutator_lock_);
  bool IsImageBinSlotAssigned(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_);
  BinSlot GetImageBinSlot(mirror::Object* object) const SHARED_REQUIRES(Locks::mutator_lock_);

  void AddDexCacheArrayRelocation(void* array, size_t offset) SHARED_REQUIRES(Locks::mutator_lock_);
  void AddMethodPointerArray(mirror::PointerArray* arr) SHARED_REQUIRES(Locks::mutator_lock_);

  static void* GetImageAddressCallback(void* writer, mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return reinterpret_cast<ImageWriter*>(writer)->GetImageAddress(obj);
  }

  mirror::Object* GetLocalAddress(mirror::Object* object) const
      SHARED_REQUIRES(Locks::mutator_lock_) {
    size_t offset = GetImageOffset(object);
    uint8_t* dst = image_->Begin() + offset;
    return reinterpret_cast<mirror::Object*>(dst);
  }

  // Returns the address in the boot image if we are compiling the app image.
  const uint8_t* GetOatAddress(OatAddress type) const;

  const uint8_t* GetOatAddressForOffset(uint32_t offset) const {
    // With Quick, code is within the OatFile, as there are all in one
    // .o ELF object.
    DCHECK_LE(offset, oat_file_->Size());
    DCHECK(oat_data_begin_ != nullptr);
    return offset == 0u ? nullptr : oat_data_begin_ + offset;
  }

  // Returns true if the class was in the original requested image classes list.
  bool KeepClass(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_);

  // Debug aid that list of requested image classes.
  void DumpImageClasses();

  // Preinitializes some otherwise lazy fields (such as Class name) to avoid runtime image dirtying.
  void ComputeLazyFieldsForImageClasses()
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Remove unwanted classes from various roots.
  void PruneNonImageClasses() SHARED_REQUIRES(Locks::mutator_lock_);

  // Verify unwanted classes removed.
  void CheckNonImageClassesRemoved() SHARED_REQUIRES(Locks::mutator_lock_);
  static void CheckNonImageClassesRemovedCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Lays out where the image objects will be at runtime.
  void CalculateNewObjectOffsets()
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CreateHeader(size_t oat_loaded_size, size_t oat_data_offset)
      SHARED_REQUIRES(Locks::mutator_lock_);
  mirror::ObjectArray<mirror::Object>* CreateImageRoots() const
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CalculateObjectBinSlots(mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void UnbinObjectsIntoOffset(mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_);

  void WalkInstanceFields(mirror::Object* obj, mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void WalkFieldsInOrder(mirror::Object* obj)
      SHARED_REQUIRES(Locks::mutator_lock_);
  static void WalkFieldsCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);
  static void UnbinObjectsIntoOffsetCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Creates the contiguous image in memory and adjusts pointers.
  void CopyAndFixupNativeData() SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupObjects() SHARED_REQUIRES(Locks::mutator_lock_);
  static void CopyAndFixupObjectsCallback(mirror::Object* obj, void* arg)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupObject(mirror::Object* obj) SHARED_REQUIRES(Locks::mutator_lock_);
  void CopyAndFixupMethod(ArtMethod* orig, ArtMethod* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupClass(mirror::Class* orig, mirror::Class* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupObject(mirror::Object* orig, mirror::Object* copy)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupDexCache(mirror::DexCache* orig_dex_cache, mirror::DexCache* copy_dex_cache)
      SHARED_REQUIRES(Locks::mutator_lock_);
  void FixupPointerArray(mirror::Object* dst,
                         mirror::PointerArray* arr,
                         mirror::Class* klass,
                         Bin array_type)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Get quick code for non-resolution/imt_conflict/abstract method.
  const uint8_t* GetQuickCode(ArtMethod* method, bool* quick_is_interpreted)
      SHARED_REQUIRES(Locks::mutator_lock_);

  const uint8_t* GetQuickEntryPoint(ArtMethod* method)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Patches references in OatFile to expect runtime addresses.
  void SetOatChecksumFromElfFile(File* elf_file);

  // Calculate the sum total of the bin slot sizes in [0, up_to). Defaults to all bins.
  size_t GetBinSizeSum(Bin up_to = kBinSize) const;

  // Return true if a method is likely to be dirtied at runtime.
  bool WillMethodBeDirty(ArtMethod* m) const SHARED_REQUIRES(Locks::mutator_lock_);

  // Assign the offset for an ArtMethod.
  void AssignMethodOffset(ArtMethod* method, NativeObjectRelocationType type)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if klass is loaded by the boot class loader but not in the boot image.
  bool IsBootClassLoaderNonImageClass(mirror::Class* klass) SHARED_REQUIRES(Locks::mutator_lock_);

  // Return true if klass depends on a boot class loader non image class live. We want to prune
  // these classes since we do not want any boot class loader classes in the image. This means that
  // we also cannot have any classes which refer to these boot class loader non image classes.
  bool ContainsBootClassLoaderNonImageClass(mirror::Class* klass)
      SHARED_REQUIRES(Locks::mutator_lock_);

  // early_exit is true if we had a cyclic dependency anywhere down the chain.
  bool ContainsBootClassLoaderNonImageClassInternal(mirror::Class* klass,
                                                    bool* early_exit,
                                                    std::unordered_set<mirror::Class*>* visited)
      SHARED_REQUIRES(Locks::mutator_lock_);

  static Bin BinTypeForNativeRelocationType(NativeObjectRelocationType type);

  uintptr_t NativeOffsetInImage(void* obj);

  // Location of where the object will be when the image is loaded at runtime.
  template <typename T>
  T* NativeLocationInImage(T* obj);

  // Location of where the temporary copy of the object currently is.
  template <typename T>
  T* NativeCopyLocation(T* obj);

  // Return true of obj is inside of the boot image space. This may only return true if we are
  // compiling an app image.
  bool IsInBootImage(const void* obj) const;

  // Return true if ptr is within the boot oat file.
  bool IsInBootOatFile(const void* ptr) const;

  const CompilerDriver& compiler_driver_;

  // Beginning target image address for the output image.
  uint8_t* image_begin_;

  // Offset to the free space in image_.
  size_t image_end_;

  // Offset from image_begin_ to where the first object is in image_.
  size_t image_objects_offset_begin_;

  // The image roots address in the image.
  uint32_t image_roots_address_;

  // oat file with code for this image
  OatFile* oat_file_;

  // Memory mapped for generating the image.
  std::unique_ptr<MemMap> image_;

  // Pointer arrays that need to be updated. Since these are only some int and long arrays, we need
  // to keep track. These include vtable arrays, iftable arrays, and dex caches.
  std::unordered_map<mirror::PointerArray*, Bin> pointer_arrays_;

  // The start offsets of the dex cache arrays.
  SafeMap<const DexFile*, size_t> dex_cache_array_starts_;

  // Saved hash codes. We use these to restore lockwords which were temporarily used to have
  // forwarding addresses as well as copying over hash codes.
  std::unordered_map<mirror::Object*, uint32_t> saved_hashcode_map_;

  // Beginning target oat address for the pointers from the output image to its oat file.
  const uint8_t* oat_data_begin_;

  // Image bitmap which lets us know where the objects inside of the image reside.
  std::unique_ptr<gc::accounting::ContinuousSpaceBitmap> image_bitmap_;

  // Offset from oat_data_begin_ to the stubs.
  uint32_t oat_address_offsets_[kOatAddressCount];

  // Boolean flags.
  const bool compile_pic_;
  const bool compile_app_image_;

  // Cache the boot image space in this class for faster lookups.
  gc::space::ImageSpace* boot_image_space_;

  // Size of pointers on the target architecture.
  size_t target_ptr_size_;

  // Bin slot tracking for dirty object packing
  size_t bin_slot_sizes_[kBinSize];  // Number of bytes in a bin
  size_t bin_slot_offsets_[kBinSize];  // Number of bytes in previous bins.
  size_t bin_slot_count_[kBinSize];  // Number of objects in a bin

  // Cached size of the intern table for when we allocate memory.
  size_t intern_table_bytes_;

  // ArtField, ArtMethod relocating map. These are allocated as array of structs but we want to
  // have one entry per art field for convenience. ArtFields are placed right after the end of the
  // image objects (aka sum of bin_slot_sizes_). ArtMethods are placed right after the ArtFields.
  struct NativeObjectRelocation {
    uintptr_t offset;
    NativeObjectRelocationType type;

    bool IsArtMethodRelocation() const {
      return type == kNativeObjectRelocationTypeArtMethodClean ||
          type == kNativeObjectRelocationTypeArtMethodDirty;
    }
  };
  std::unordered_map<void*, NativeObjectRelocation> native_object_relocations_;

  // Runtime ArtMethods which aren't reachable from any Class but need to be copied into the image.
  ArtMethod* image_methods_[ImageHeader::kImageMethodsCount];
  // Fake length prefixed array for image methods. This array does not contain the actual
  // ArtMethods. We only use it for the header and relocation addresses.
  LengthPrefixedArray<ArtMethod> image_method_array_;

  // Counters for measurements, used for logging only.
  uint64_t dirty_methods_;
  uint64_t clean_methods_;

  // Prune class memoization table to speed up ContainsBootClassLoaderNonImageClass.
  std::unordered_map<mirror::Class*, bool> prune_class_memo_;

  friend class ContainsBootClassLoaderNonImageClassVisitor;
  friend class FixupClassVisitor;
  friend class FixupRootVisitor;
  friend class FixupVisitor;
  friend class NativeLocationVisitor;
  friend class NonImageClassesVisitor;
  DISALLOW_COPY_AND_ASSIGN(ImageWriter);
};

}  // namespace art

#endif  // ART_COMPILER_IMAGE_WRITER_H_
