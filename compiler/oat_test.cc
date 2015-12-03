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

#include "arch/instruction_set_features.h"
#include "art_method-inl.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "common_compiler_test.h"
#include "compiled_method.h"
#include "compiler.h"
#include "dex/pass_manager.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/quick_compiler_callbacks.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_options.h"
#include "dwarf/method_debug_info.h"
#include "elf_writer.h"
#include "elf_writer_quick.h"
#include "entrypoints/quick/quick_entrypoints.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "oat_file-inl.h"
#include "oat_writer.h"
#include "scoped_thread_state_change.h"
#include "vector_output_stream.h"

namespace art {

NO_RETURN static void Usage(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string error;
  StringAppendV(&error, fmt, ap);
  LOG(FATAL) << error;
  va_end(ap);
  UNREACHABLE();
}

class OatTest : public CommonCompilerTest {
 protected:
  static const bool kCompile = false;  // DISABLED_ due to the time to compile libcore

  void CheckMethod(ArtMethod* method,
                   const OatFile::OatMethod& oat_method,
                   const DexFile& dex_file)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const CompiledMethod* compiled_method =
        compiler_driver_->GetCompiledMethod(MethodReference(&dex_file,
                                                            method->GetDexMethodIndex()));

    if (compiled_method == nullptr) {
      EXPECT_TRUE(oat_method.GetQuickCode() == nullptr) << PrettyMethod(method) << " "
                                                        << oat_method.GetQuickCode();
      EXPECT_EQ(oat_method.GetFrameSizeInBytes(), 0U);
      EXPECT_EQ(oat_method.GetCoreSpillMask(), 0U);
      EXPECT_EQ(oat_method.GetFpSpillMask(), 0U);
    } else {
      const void* quick_oat_code = oat_method.GetQuickCode();
      EXPECT_TRUE(quick_oat_code != nullptr) << PrettyMethod(method);
      EXPECT_EQ(oat_method.GetFrameSizeInBytes(), compiled_method->GetFrameSizeInBytes());
      EXPECT_EQ(oat_method.GetCoreSpillMask(), compiled_method->GetCoreSpillMask());
      EXPECT_EQ(oat_method.GetFpSpillMask(), compiled_method->GetFpSpillMask());
      uintptr_t oat_code_aligned = RoundDown(reinterpret_cast<uintptr_t>(quick_oat_code), 2);
      quick_oat_code = reinterpret_cast<const void*>(oat_code_aligned);
      ArrayRef<const uint8_t> quick_code = compiled_method->GetQuickCode();
      EXPECT_FALSE(quick_code.empty());
      size_t code_size = quick_code.size() * sizeof(quick_code[0]);
      EXPECT_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size))
          << PrettyMethod(method) << " " << code_size;
      CHECK_EQ(0, memcmp(quick_oat_code, &quick_code[0], code_size));
    }
  }

  void SetupCompiler(Compiler::Kind compiler_kind,
                     InstructionSet insn_set,
                     const std::vector<std::string>& compiler_options,
                     /*out*/std::string* error_msg) {
    ASSERT_TRUE(error_msg != nullptr);
    insn_features_.reset(InstructionSetFeatures::FromVariant(insn_set, "default", error_msg));
    ASSERT_TRUE(insn_features_ != nullptr) << error_msg;
    compiler_options_.reset(new CompilerOptions);
    for (const std::string& option : compiler_options) {
      compiler_options_->ParseCompilerOption(option, Usage);
    }
    verification_results_.reset(new VerificationResults(compiler_options_.get()));
    method_inliner_map_.reset(new DexFileToMethodInlinerMap);
    callbacks_.reset(new QuickCompilerCallbacks(verification_results_.get(),
                                                method_inliner_map_.get(),
                                                CompilerCallbacks::CallbackMode::kCompileApp));
    Runtime::Current()->SetCompilerCallbacks(callbacks_.get());
    timer_.reset(new CumulativeLogger("Compilation times"));
    compiler_driver_.reset(new CompilerDriver(compiler_options_.get(),
                                              verification_results_.get(),
                                              method_inliner_map_.get(),
                                              compiler_kind,
                                              insn_set,
                                              insn_features_.get(),
                                              false,
                                              nullptr,
                                              nullptr,
                                              nullptr,
                                              2,
                                              true,
                                              true,
                                              "",
                                              false,
                                              timer_.get(),
                                              -1,
                                              ""));
  }

  bool WriteElf(File* file,
                const std::vector<const DexFile*>& dex_files,
                SafeMap<std::string, std::string>& key_value_store) {
    TimingLogger timings("WriteElf", false, false);
    OatWriter oat_writer(dex_files,
                         42U,
                         4096U,
                         0,
                         compiler_driver_.get(),
                         nullptr,
                         /*compiling_boot_image*/false,
                         &timings,
                         &key_value_store);
    std::unique_ptr<ElfWriter> elf_writer = CreateElfWriterQuick(
        compiler_driver_->GetInstructionSet(),
        &compiler_driver_->GetCompilerOptions(),
        file);

    elf_writer->Start();

    OutputStream* rodata = elf_writer->StartRoData();
    if (!oat_writer.WriteRodata(rodata)) {
      return false;
    }
    elf_writer->EndRoData(rodata);

    OutputStream* text = elf_writer->StartText();
    if (!oat_writer.WriteCode(text)) {
      return false;
    }
    elf_writer->EndText(text);

    elf_writer->SetBssSize(oat_writer.GetBssSize());

    elf_writer->WriteDynamicSection();

    ArrayRef<const dwarf::MethodDebugInfo> method_infos(oat_writer.GetMethodDebugInfo());
    elf_writer->WriteDebugInfo(method_infos);

    ArrayRef<const uintptr_t> patch_locations(oat_writer.GetAbsolutePatchLocations());
    elf_writer->WritePatchLocations(patch_locations);

    return elf_writer->End();
  }

  std::unique_ptr<const InstructionSetFeatures> insn_features_;
  std::unique_ptr<QuickCompilerCallbacks> callbacks_;
};

TEST_F(OatTest, WriteRead) {
  TimingLogger timings("OatTest::WriteRead", false, false);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();

  // TODO: make selectable.
  Compiler::Kind compiler_kind = Compiler::kQuick;
  InstructionSet insn_set = kIsTargetBuild ? kThumb2 : kX86;
  std::string error_msg;
  SetupCompiler(compiler_kind, insn_set, std::vector<std::string>(), /*out*/ &error_msg);

  jobject class_loader = nullptr;
  if (kCompile) {
    TimingLogger timings2("OatTest::WriteRead", false, false);
    compiler_driver_->SetDexFilesForOatFile(class_linker->GetBootClassPath());
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings2);
  }

  ScratchFile tmp;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "lue.art");
  bool success = WriteElf(tmp.GetFile(), class_linker->GetBootClassPath(), key_value_store);
  ASSERT_TRUE(success);

  if (kCompile) {  // OatWriter strips the code, regenerate to compare
    compiler_driver_->CompileAll(class_loader, class_linker->GetBootClassPath(), &timings);
  }
  std::unique_ptr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(), tmp.GetFilename(), nullptr,
                                                  nullptr, false, nullptr, &error_msg));
  ASSERT_TRUE(oat_file.get() != nullptr) << error_msg;
  const OatHeader& oat_header = oat_file->GetOatHeader();
  ASSERT_TRUE(oat_header.IsValid());
  ASSERT_EQ(1U, oat_header.GetDexFileCount());  // core
  ASSERT_EQ(42U, oat_header.GetImageFileLocationOatChecksum());
  ASSERT_EQ(4096U, oat_header.GetImageFileLocationOatDataBegin());
  ASSERT_EQ("lue.art", std::string(oat_header.GetStoreValueByKey(OatHeader::kImageLocationKey)));

  ASSERT_TRUE(java_lang_dex_file_ != nullptr);
  const DexFile& dex_file = *java_lang_dex_file_;
  uint32_t dex_file_checksum = dex_file.GetLocationChecksum();
  const OatFile::OatDexFile* oat_dex_file = oat_file->GetOatDexFile(dex_file.GetLocation().c_str(),
                                                                    &dex_file_checksum);
  ASSERT_TRUE(oat_dex_file != nullptr);
  CHECK_EQ(dex_file.GetLocationChecksum(), oat_dex_file->GetDexFileLocationChecksum());
  ScopedObjectAccess soa(Thread::Current());
  auto pointer_size = class_linker->GetImagePointerSize();
  for (size_t i = 0; i < dex_file.NumClassDefs(); i++) {
    const DexFile::ClassDef& class_def = dex_file.GetClassDef(i);
    const uint8_t* class_data = dex_file.GetClassData(class_def);

    size_t num_virtual_methods = 0;
    if (class_data != nullptr) {
      ClassDataItemIterator it(dex_file, class_data);
      num_virtual_methods = it.NumVirtualMethods();
    }

    const char* descriptor = dex_file.GetClassDescriptor(class_def);
    mirror::Class* klass = class_linker->FindClass(soa.Self(), descriptor,
                                                   NullHandle<mirror::ClassLoader>());

    const OatFile::OatClass oat_class = oat_dex_file->GetOatClass(i);
    CHECK_EQ(mirror::Class::Status::kStatusNotReady, oat_class.GetStatus()) << descriptor;
    CHECK_EQ(kCompile ? OatClassType::kOatClassAllCompiled : OatClassType::kOatClassNoneCompiled,
             oat_class.GetType()) << descriptor;

    size_t method_index = 0;
    for (auto& m : klass->GetDirectMethods(pointer_size)) {
      CheckMethod(&m, oat_class.GetOatMethod(method_index), dex_file);
      ++method_index;
    }
    size_t visited_virtuals = 0;
    for (auto& m : klass->GetVirtualMethods(pointer_size)) {
      if (!m.IsMiranda()) {
        CheckMethod(&m, oat_class.GetOatMethod(method_index), dex_file);
        ++method_index;
        ++visited_virtuals;
      }
    }
    EXPECT_EQ(visited_virtuals, num_virtual_methods);
  }
}

TEST_F(OatTest, OatHeaderSizeCheck) {
  // If this test is failing and you have to update these constants,
  // it is time to update OatHeader::kOatVersion
  EXPECT_EQ(72U, sizeof(OatHeader));
  EXPECT_EQ(4U, sizeof(OatMethodOffsets));
  EXPECT_EQ(28U, sizeof(OatQuickMethodHeader));
  EXPECT_EQ(114 * GetInstructionSetPointerSize(kRuntimeISA), sizeof(QuickEntryPoints));
}

TEST_F(OatTest, OatHeaderIsValid) {
    InstructionSet insn_set = kX86;
    std::string error_msg;
    std::unique_ptr<const InstructionSetFeatures> insn_features(
        InstructionSetFeatures::FromVariant(insn_set, "default", &error_msg));
    ASSERT_TRUE(insn_features.get() != nullptr) << error_msg;
    std::vector<const DexFile*> dex_files;
    uint32_t image_file_location_oat_checksum = 0;
    uint32_t image_file_location_oat_begin = 0;
    std::unique_ptr<OatHeader> oat_header(OatHeader::Create(insn_set,
                                                            insn_features.get(),
                                                            &dex_files,
                                                            image_file_location_oat_checksum,
                                                            image_file_location_oat_begin,
                                                            nullptr));
    ASSERT_NE(oat_header.get(), nullptr);
    ASSERT_TRUE(oat_header->IsValid());

    char* magic = const_cast<char*>(oat_header->GetMagic());
    strcpy(magic, "");  // bad magic
    ASSERT_FALSE(oat_header->IsValid());
    strcpy(magic, "oat\n000");  // bad version
    ASSERT_FALSE(oat_header->IsValid());
}

TEST_F(OatTest, EmptyTextSection) {
  TimingLogger timings("OatTest::EmptyTextSection", false, false);

  // TODO: make selectable.
  Compiler::Kind compiler_kind = Compiler::kQuick;
  InstructionSet insn_set = kRuntimeISA;
  if (insn_set == kArm) insn_set = kThumb2;
  std::string error_msg;
  std::vector<std::string> compiler_options;
  compiler_options.push_back("--compiler-filter=verify-at-runtime");
  SetupCompiler(compiler_kind, insn_set, compiler_options, /*out*/ &error_msg);

  jobject class_loader;
  {
    ScopedObjectAccess soa(Thread::Current());
    class_loader = LoadDex("Main");
  }
  ASSERT_TRUE(class_loader != nullptr);
  std::vector<const DexFile*> dex_files = GetDexFiles(class_loader);
  ASSERT_TRUE(!dex_files.empty());

  ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
  for (const DexFile* dex_file : dex_files) {
    ScopedObjectAccess soa(Thread::Current());
    class_linker->RegisterDexFile(
        *dex_file,
        class_linker->GetOrCreateAllocatorForClassLoader(
            soa.Decode<mirror::ClassLoader*>(class_loader)));
  }
  compiler_driver_->SetDexFilesForOatFile(dex_files);
  compiler_driver_->CompileAll(class_loader, dex_files, &timings);

  ScratchFile tmp;
  SafeMap<std::string, std::string> key_value_store;
  key_value_store.Put(OatHeader::kImageLocationKey, "test.art");
  bool success = WriteElf(tmp.GetFile(), dex_files, key_value_store);
  ASSERT_TRUE(success);

  std::unique_ptr<OatFile> oat_file(OatFile::Open(tmp.GetFilename(),
                                                  tmp.GetFilename(),
                                                  nullptr,
                                                  nullptr,
                                                  false,
                                                  nullptr,
                                                  &error_msg));
  ASSERT_TRUE(oat_file != nullptr);
  EXPECT_LT(static_cast<size_t>(oat_file->Size()), static_cast<size_t>(tmp.GetFile()->GetLength()));
}

}  // namespace art
