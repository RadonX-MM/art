/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "optimizing_compiler.h"

#include <fstream>
#include <stdint.h>

#ifdef ART_ENABLE_CODEGEN_arm64
#include "dex_cache_array_fixups_arm.h"
#endif

#ifdef ART_ENABLE_CODEGEN_arm64
#include "instruction_simplifier_arm64.h"
#endif

#ifdef ART_ENABLE_CODEGEN_x86
#include "pc_relative_fixups_x86.h"
#endif

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/arena_containers.h"
#include "base/dumpable.h"
#include "base/macros.h"
#include "base/timing_logger.h"
#include "boolean_simplifier.h"
#include "bounds_check_elimination.h"
#include "builder.h"
#include "code_generator.h"
#include "compiled_method.h"
#include "compiler.h"
#include "constant_folding.h"
#include "dead_code_elimination.h"
#include "dex/quick/dex_file_to_method_inliner_map.h"
#include "dex/verified_method.h"
#include "dex/verification_results.h"
#include "driver/compiler_driver.h"
#include "driver/compiler_driver-inl.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "elf_writer_quick.h"
#include "graph_checker.h"
#include "graph_visualizer.h"
#include "gvn.h"
#include "induction_var_analysis.h"
#include "inliner.h"
#include "instruction_simplifier.h"
#include "intrinsics.h"
#include "jit/jit_code_cache.h"
#include "licm.h"
#include "jni/quick/jni_compiler.h"
#include "load_store_elimination.h"
#include "nodes.h"
#include "prepare_for_register_allocation.h"
#include "reference_type_propagation.h"
#include "register_allocator.h"
#include "sharpening.h"
#include "side_effects_analysis.h"
#include "ssa_builder.h"
#include "ssa_phi_elimination.h"
#include "ssa_liveness_analysis.h"
#include "utils/assembler.h"
#include "verifier/method_verifier.h"

namespace art {

class OptimizingCompiler;

// fast compile path
CompiledMethod* TryFastCompile(CompilerDriver* driver,
                               Compiler* compiler,
                               const DexFile::CodeItem* code_item,
                               uint32_t access_flags,
                               InvokeType invoke_type,
                               uint16_t class_def_idx,
                               uint32_t method_idx,
                               jobject jclass_loader,
                               const DexFile& dex_file) __attribute__((weak));


/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator FINAL : public CodeAllocator {
 public:
  explicit CodeVectorAllocator(ArenaAllocator* arena)
      : memory_(arena->Adapter(kArenaAllocCodeBuffer)),
        size_(0) {}

  virtual uint8_t* Allocate(size_t size) {
    size_ = size;
    memory_.resize(size);
    return &memory_[0];
  }

  size_t GetSize() const { return size_; }
  const ArenaVector<uint8_t>& GetMemory() const { return memory_; }

 private:
  ArenaVector<uint8_t> memory_;
  size_t size_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};

/**
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be dumped.
 */
static constexpr const char kStringFilter[] = "";

class PassScope;

class PassObserver : public ValueObject {
 public:
  PassObserver(HGraph* graph,
               CodeGenerator* codegen,
               std::ostream* visualizer_output,
               CompilerDriver* compiler_driver)
      : graph_(graph),
        cached_method_name_(),
        timing_logger_enabled_(compiler_driver->GetDumpPasses()),
        timing_logger_(timing_logger_enabled_ ? GetMethodName() : "", true, true),
        disasm_info_(graph->GetArena()),
        visualizer_enabled_(!compiler_driver->GetDumpCfgFileName().empty()),
        visualizer_(visualizer_output, graph, *codegen),
        graph_in_bad_state_(false) {
    if (timing_logger_enabled_ || visualizer_enabled_) {
      if (!IsVerboseMethod(compiler_driver, GetMethodName())) {
        timing_logger_enabled_ = visualizer_enabled_ = false;
      }
      if (visualizer_enabled_) {
        visualizer_.PrintHeader(GetMethodName());
        codegen->SetDisassemblyInformation(&disasm_info_);
      }
    }
  }

  ~PassObserver() {
    if (timing_logger_enabled_) {
      LOG(INFO) << "TIMINGS " << GetMethodName();
      LOG(INFO) << Dumpable<TimingLogger>(timing_logger_);
    }
  }

  void DumpDisassembly() const {
    if (visualizer_enabled_) {
      visualizer_.DumpGraphWithDisassembly();
    }
  }

  void SetGraphInBadState() { graph_in_bad_state_ = true; }

  const char* GetMethodName() {
    // PrettyMethod() is expensive, so we delay calling it until we actually have to.
    if (cached_method_name_.empty()) {
      cached_method_name_ = PrettyMethod(graph_->GetMethodIdx(), graph_->GetDexFile());
    }
    return cached_method_name_.c_str();
  }

 private:
  void StartPass(const char* pass_name) {
    // Dump graph first, then start timer.
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ false, graph_in_bad_state_);
    }
    if (timing_logger_enabled_) {
      timing_logger_.StartTiming(pass_name);
    }
  }

  void EndPass(const char* pass_name) {
    // Pause timer first, then dump graph.
    if (timing_logger_enabled_) {
      timing_logger_.EndTiming();
    }
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass */ true, graph_in_bad_state_);
    }

    // Validate the HGraph if running in debug mode.
    if (kIsDebugBuild) {
      if (!graph_in_bad_state_) {
        if (graph_->IsInSsaForm()) {
          SSAChecker checker(graph_);
          checker.Run();
          if (!checker.IsValid()) {
            LOG(FATAL) << "Error after " << pass_name << ": " << Dumpable<SSAChecker>(checker);
          }
        } else {
          GraphChecker checker(graph_);
          checker.Run();
          if (!checker.IsValid()) {
            LOG(FATAL) << "Error after " << pass_name << ": " << Dumpable<GraphChecker>(checker);
          }
        }
      }
    }
  }

  static bool IsVerboseMethod(CompilerDriver* compiler_driver, const char* method_name) {
    // Test an exact match to --verbose-methods. If verbose-methods is set, this overrides an
    // empty kStringFilter matching all methods.
    if (compiler_driver->GetCompilerOptions().HasVerboseMethods()) {
      return compiler_driver->GetCompilerOptions().IsVerboseMethod(method_name);
    }

    // Test the kStringFilter sub-string. constexpr helper variable to silence unreachable-code
    // warning when the string is empty.
    constexpr bool kStringFilterEmpty = arraysize(kStringFilter) <= 1;
    if (kStringFilterEmpty || strstr(method_name, kStringFilter) != nullptr) {
      return true;
    }

    return false;
  }

  HGraph* const graph_;

  std::string cached_method_name_;

  bool timing_logger_enabled_;
  TimingLogger timing_logger_;

  DisassemblyInformation disasm_info_;

  bool visualizer_enabled_;
  HGraphVisualizer visualizer_;

  // Flag to be set by the compiler if the pass failed and the graph is not
  // expected to validate.
  bool graph_in_bad_state_;

  friend PassScope;

  DISALLOW_COPY_AND_ASSIGN(PassObserver);
};

class PassScope : public ValueObject {
 public:
  PassScope(const char *pass_name, PassObserver* pass_observer)
      : pass_name_(pass_name),
        pass_observer_(pass_observer) {
    pass_observer_->StartPass(pass_name_);
  }

  ~PassScope() {
    pass_observer_->EndPass(pass_name_);
  }

 private:
  const char* const pass_name_;
  PassObserver* const pass_observer_;
};

class OptimizingCompiler FINAL : public Compiler {
 public:
  explicit OptimizingCompiler(CompilerDriver* driver);
  ~OptimizingCompiler();

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file, CompilationUnit* cu) const
      OVERRIDE;

  CompiledMethod* Compile(const DexFile::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          jobject class_loader,
                          const DexFile& dex_file,
                          Handle<mirror::DexCache> dex_cache) const OVERRIDE;

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file) const OVERRIDE {
    return ArtQuickJniCompileMethod(GetCompilerDriver(), access_flags, method_idx, dex_file);
  }

  uintptr_t GetEntryPointOf(ArtMethod* method) const OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_) {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
        InstructionSetPointerSize(GetCompilerDriver()->GetInstructionSet())));
  }

  void InitCompilationUnit(CompilationUnit& cu) const OVERRIDE;

  void Init() OVERRIDE;

  void UnInit() const OVERRIDE;

  void MaybeRecordStat(MethodCompilationStat compilation_stat) const {
    if (compilation_stats_.get() != nullptr) {
      compilation_stats_->RecordStat(compilation_stat);
    }
  }

  bool JitCompile(Thread* self, jit::JitCodeCache* code_cache, ArtMethod* method)
      OVERRIDE
      SHARED_REQUIRES(Locks::mutator_lock_);

 private:
  // Whether we should run any optimization or register allocation. If false, will
  // just run the code generation after the graph was built.
  const bool run_optimizations_;

  // Create a 'CompiledMethod' for an optimized graph.
  CompiledMethod* EmitOptimized(ArenaAllocator* arena,
                                CodeVectorAllocator* code_allocator,
                                CodeGenerator* codegen,
                                CompilerDriver* driver) const;

  // Create a 'CompiledMethod' for a non-optimized graph.
  CompiledMethod* EmitBaseline(ArenaAllocator* arena,
                               CodeVectorAllocator* code_allocator,
                               CodeGenerator* codegen,
                               CompilerDriver* driver) const;

  // Try compiling a method and return the code generator used for
  // compiling it.
  // This method:
  // 1) Builds the graph. Returns null if it failed to build it.
  // 2) If `run_optimizations_` is set:
  //    2.1) Transform the graph to SSA. Returns null if it failed.
  //    2.2) Run optimizations on the graph, including register allocator.
  // 3) Generate code with the `code_allocator` provided.
  CodeGenerator* TryCompile(ArenaAllocator* arena,
                            CodeVectorAllocator* code_allocator,
                            const DexFile::CodeItem* code_item,
                            uint32_t access_flags,
                            InvokeType invoke_type,
                            uint16_t class_def_idx,
                            uint32_t method_idx,
                            jobject class_loader,
                            const DexFile& dex_file,
                            Handle<mirror::DexCache> dex_cache) const;

  std::unique_ptr<OptimizingCompilerStats> compilation_stats_;

  std::unique_ptr<std::ostream> visualizer_output_;

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(CompilerDriver* driver)
    : Compiler(driver, kMaximumCompilationTimeBeforeWarning),
      run_optimizations_(
          driver->GetCompilerOptions().GetCompilerFilter() != CompilerOptions::kTime) {}

void OptimizingCompiler::Init() {
  // Enable C1visualizer output. Must be done in Init() because the compiler
  // driver is not fully initialized when passed to the compiler's constructor.
  CompilerDriver* driver = GetCompilerDriver();
  const std::string cfg_file_name = driver->GetDumpCfgFileName();
  if (!cfg_file_name.empty()) {
    CHECK_EQ(driver->GetThreadCount(), 1U)
      << "Graph visualizer requires the compiler to run single-threaded. "
      << "Invoke the compiler with '-j1'.";
    std::ios_base::openmode cfg_file_mode =
        driver->GetDumpCfgAppend() ? std::ofstream::app : std::ofstream::out;
    visualizer_output_.reset(new std::ofstream(cfg_file_name, cfg_file_mode));
  }
  if (driver->GetDumpStats()) {
    compilation_stats_.reset(new OptimizingCompilerStats());
  }
}

void OptimizingCompiler::UnInit() const {
}

OptimizingCompiler::~OptimizingCompiler() {
  if (compilation_stats_.get() != nullptr) {
    compilation_stats_->Log();
  }
}

void OptimizingCompiler::InitCompilationUnit(CompilationUnit& cu ATTRIBUTE_UNUSED) const {
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx ATTRIBUTE_UNUSED,
                                          const DexFile& dex_file ATTRIBUTE_UNUSED,
                                          CompilationUnit* cu ATTRIBUTE_UNUSED) const {
  return true;
}

static bool IsInstructionSetSupported(InstructionSet instruction_set) {
  return (instruction_set == kArm && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kArm64
      || (instruction_set == kThumb2 && !kArm32QuickCodeUseSoftFloat)
      || instruction_set == kMips
      || instruction_set == kMips64
      || instruction_set == kX86
      || instruction_set == kX86_64;
}

// Read barrier are supported on ARM, ARM64, x86 and x86-64 at the moment.
// TODO: Add support for other architectures and remove this function
static bool InstructionSetSupportsReadBarrier(InstructionSet instruction_set) {
  return instruction_set == kArm64
      || instruction_set == kThumb2
      || instruction_set == kX86
      || instruction_set == kX86_64;
}


HOptimization* GetMoreOptimizing(HGraph*,
                                 const DexCompilationUnit&,
                                 CompilerDriver*,
                                 OptimizingCompilerStats*) __attribute__((weak));
HOptimization* GetMoreOptimizing(HGraph*,
                                 const DexCompilationUnit&,
                                 CompilerDriver*,
                                 OptimizingCompilerStats*)
{
  return nullptr;
}

static void RunOptimizations(HOptimization* optimizations[],
                             size_t length,
                             PassObserver* pass_observer) {
  for (size_t i = 0; i < length; ++i) {
    PassScope scope(optimizations[i]->GetPassName(), pass_observer);
    optimizations[i]->Run();
  }
}

static void MaybeRunInliner(HGraph* graph,
                            CodeGenerator* codegen,
                            CompilerDriver* driver,
                            OptimizingCompilerStats* stats,
                            const DexCompilationUnit& dex_compilation_unit,
                            PassObserver* pass_observer,
                            StackHandleScopeCollection* handles) {
  const CompilerOptions& compiler_options = driver->GetCompilerOptions();
  bool should_inline = (compiler_options.GetInlineDepthLimit() > 0)
      && (compiler_options.GetInlineMaxCodeUnits() > 0);
  if (!should_inline) {
    return;
  }
  HInliner* inliner = new (graph->GetArena()) HInliner(
    graph, codegen, dex_compilation_unit, dex_compilation_unit, driver, handles, stats);
  HOptimization* optimizations[] = { inliner };

  RunOptimizations(optimizations, arraysize(optimizations), pass_observer);
}

static void RunArchOptimizations(InstructionSet instruction_set,
                                 HGraph* graph,
                                 OptimizingCompilerStats* stats,
                                 PassObserver* pass_observer) {
  ArenaAllocator* arena = graph->GetArena();
  switch (instruction_set) {
#ifdef ART_ENABLE_CODEGEN_arm
    case kThumb2:
    case kArm: {
      arm::DexCacheArrayFixups* fixups = new (arena) arm::DexCacheArrayFixups(graph, stats);
      HOptimization* arm_optimizations[] = {
        fixups
      };
      RunOptimizations(arm_optimizations, arraysize(arm_optimizations), pass_observer);
      break;
    }
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case kArm64: {
      arm64::InstructionSimplifierArm64* simplifier =
          new (arena) arm64::InstructionSimplifierArm64(graph, stats);
      SideEffectsAnalysis* side_effects = new (arena) SideEffectsAnalysis(graph);
      GVNOptimization* gvn = new (arena) GVNOptimization(graph, *side_effects, "GVN_after_arch");
      HOptimization* arm64_optimizations[] = {
        simplifier,
        side_effects,
        gvn
      };
      RunOptimizations(arm64_optimizations, arraysize(arm64_optimizations), pass_observer);
      break;
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case kX86: {
      x86::PcRelativeFixups* pc_relative_fixups = new (arena) x86::PcRelativeFixups(graph, stats);
      HOptimization* x86_optimizations[] = {
          pc_relative_fixups
      };
      RunOptimizations(x86_optimizations, arraysize(x86_optimizations), pass_observer);
      break;
    }
#endif
    default:
      break;
  }
}

NO_INLINE  // Avoid increasing caller's frame size by large stack-allocated objects.
static void AllocateRegisters(HGraph* graph,
                              CodeGenerator* codegen,
                              PassObserver* pass_observer) {
  PrepareForRegisterAllocation(graph).Run();
  SsaLivenessAnalysis liveness(graph, codegen);
  {
    PassScope scope(SsaLivenessAnalysis::kLivenessPassName, pass_observer);
    liveness.Analyze();
  }
  {
    PassScope scope(RegisterAllocator::kRegisterAllocatorPassName, pass_observer);
    RegisterAllocator(graph->GetArena(), codegen, liveness).AllocateRegisters();
  }
}

static void RunOptimizations(HGraph* graph,
                             CodeGenerator* codegen,
                             CompilerDriver* driver,
                             OptimizingCompilerStats* stats,
                             const DexCompilationUnit& dex_compilation_unit,
                             PassObserver* pass_observer) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScopeCollection handles(soa.Self());
  ScopedThreadSuspension sts(soa.Self(), kNative);

  ArenaAllocator* arena = graph->GetArena();
  HDeadCodeElimination* dce1 = new (arena) HDeadCodeElimination(
      graph, stats, HDeadCodeElimination::kInitialDeadCodeEliminationPassName);
  HDeadCodeElimination* dce2 = new (arena) HDeadCodeElimination(
      graph, stats, HDeadCodeElimination::kFinalDeadCodeEliminationPassName);
  HConstantFolding* fold1 = new (arena) HConstantFolding(graph);
  InstructionSimplifier* simplify1 = new (arena) InstructionSimplifier(graph, stats);
  HBooleanSimplifier* boolean_simplify = new (arena) HBooleanSimplifier(graph);
  HConstantFolding* fold2 = new (arena) HConstantFolding(graph, "constant_folding_after_inlining");
  HConstantFolding* fold3 = new (arena) HConstantFolding(graph, "constant_folding_after_bce");
  SideEffectsAnalysis* side_effects = new (arena) SideEffectsAnalysis(graph);
  GVNOptimization* gvn = new (arena) GVNOptimization(graph, *side_effects);
  LICM* licm = new (arena) LICM(graph, *side_effects);
  LoadStoreElimination* lse = new (arena) LoadStoreElimination(graph, *side_effects);
  HInductionVarAnalysis* induction = new (arena) HInductionVarAnalysis(graph);
  BoundsCheckElimination* bce = new (arena) BoundsCheckElimination(graph, *side_effects, induction);
  ReferenceTypePropagation* type_propagation =
      new (arena) ReferenceTypePropagation(graph, &handles);
  HSharpening* sharpening = new (arena) HSharpening(graph, codegen, dex_compilation_unit, driver);
  InstructionSimplifier* simplify2 = new (arena) InstructionSimplifier(
      graph, stats, "instruction_simplifier_after_types");
  InstructionSimplifier* simplify3 = new (arena) InstructionSimplifier(
      graph, stats, "instruction_simplifier_after_bce");
  InstructionSimplifier* simplify4 = new (arena) InstructionSimplifier(
      graph, stats, "instruction_simplifier_before_codegen");

  IntrinsicsRecognizer* intrinsics = new (arena) IntrinsicsRecognizer(graph, driver);

  HOptimization* optimizations1[] = {
    intrinsics,
    fold1,
    simplify1,
    type_propagation,
    sharpening,
    dce1,
    simplify2
  };

  RunOptimizations(optimizations1, arraysize(optimizations1), pass_observer);

  MaybeRunInliner(graph, codegen, driver, stats, dex_compilation_unit, pass_observer, &handles);

  // TODO: Update passes incompatible with try/catch so we have the same
  //       pipeline for all methods.
  if (graph->HasTryCatch()) {
    HOptimization* optimizations2[] = {
      boolean_simplify,
      side_effects,
      gvn,
      dce2,
      // The codegen has a few assumptions that only the instruction simplifier
      // can satisfy. For example, the code generator does not expect to see a
      // HTypeConversion from a type to the same type.
      simplify4,
    };

    RunOptimizations(optimizations2, arraysize(optimizations2), pass_observer);
  } else {
    HOptimization* optimizations2[] = {
      // BooleanSimplifier depends on the InstructionSimplifier removing
      // redundant suspend checks to recognize empty blocks.
      boolean_simplify,
      fold2,  // TODO: if we don't inline we can also skip fold2.
      side_effects,
      gvn,
      licm,
      induction,
      bce,
      fold3,  // evaluates code generated by dynamic bce
      simplify3,
      lse,
      dce2,
      // The codegen has a few assumptions that only the instruction simplifier
      // can satisfy. For example, the code generator does not expect to see a
      // HTypeConversion from a type to the same type.
      simplify4,
    };

    RunOptimizations(optimizations2, arraysize(optimizations2), pass_observer);
  }

  RunArchOptimizations(driver->GetInstructionSet(), graph, stats, pass_observer);
  AllocateRegisters(graph, codegen, pass_observer);
}

// The stack map we generate must be 4-byte aligned on ARM. Since existing
// maps are generated alongside these stack maps, we must also align them.
static ArrayRef<const uint8_t> AlignVectorSize(ArenaVector<uint8_t>& vector) {
  size_t size = vector.size();
  size_t aligned_size = RoundUp(size, 4);
  for (; size < aligned_size; ++size) {
    vector.push_back(0);
  }
  return ArrayRef<const uint8_t>(vector);
}

static ArenaVector<LinkerPatch> EmitAndSortLinkerPatches(CodeGenerator* codegen) {
  ArenaVector<LinkerPatch> linker_patches(codegen->GetGraph()->GetArena()->Adapter());
  codegen->EmitLinkerPatches(&linker_patches);

  // Sort patches by literal offset. Required for .oat_patches encoding.
  std::sort(linker_patches.begin(), linker_patches.end(),
            [](const LinkerPatch& lhs, const LinkerPatch& rhs) {
    return lhs.LiteralOffset() < rhs.LiteralOffset();
  });

  return linker_patches;
}

CompiledMethod* OptimizingCompiler::EmitOptimized(ArenaAllocator* arena,
                                                  CodeVectorAllocator* code_allocator,
                                                  CodeGenerator* codegen,
                                                  CompilerDriver* compiler_driver) const {
  ArenaVector<LinkerPatch> linker_patches = EmitAndSortLinkerPatches(codegen);
  ArenaVector<uint8_t> stack_map(arena->Adapter(kArenaAllocStackMaps));
  stack_map.resize(codegen->ComputeStackMapsSize());
  codegen->BuildStackMaps(MemoryRegion(stack_map.data(), stack_map.size()));

  CompiledMethod* compiled_method = CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(code_allocator->GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      ArrayRef<const SrcMapElem>(codegen->GetSrcMappingTable()),
      ArrayRef<const uint8_t>(),  // mapping_table.
      ArrayRef<const uint8_t>(stack_map),
      ArrayRef<const uint8_t>(),  // native_gc_map.
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const LinkerPatch>(linker_patches));

  return compiled_method;
}

CompiledMethod* OptimizingCompiler::EmitBaseline(
    ArenaAllocator* arena,
    CodeVectorAllocator* code_allocator,
    CodeGenerator* codegen,
    CompilerDriver* compiler_driver) const {
  ArenaVector<LinkerPatch> linker_patches = EmitAndSortLinkerPatches(codegen);

  ArenaVector<uint8_t> mapping_table(arena->Adapter(kArenaAllocBaselineMaps));
  codegen->BuildMappingTable(&mapping_table);
  ArenaVector<uint8_t> vmap_table(arena->Adapter(kArenaAllocBaselineMaps));
  codegen->BuildVMapTable(&vmap_table);
  ArenaVector<uint8_t> gc_map(arena->Adapter(kArenaAllocBaselineMaps));
  codegen->BuildNativeGCMap(&gc_map, *compiler_driver);

  CompiledMethod* compiled_method = CompiledMethod::SwapAllocCompiledMethod(
      compiler_driver,
      codegen->GetInstructionSet(),
      ArrayRef<const uint8_t>(code_allocator->GetMemory()),
      // Follow Quick's behavior and set the frame size to zero if it is
      // considered "empty" (see the definition of
      // art::CodeGenerator::HasEmptyFrame).
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      ArrayRef<const SrcMapElem>(codegen->GetSrcMappingTable()),
      AlignVectorSize(mapping_table),
      AlignVectorSize(vmap_table),
      AlignVectorSize(gc_map),
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const LinkerPatch>(linker_patches));
  return compiled_method;
}

CodeGenerator* OptimizingCompiler::TryCompile(ArenaAllocator* arena,
                                              CodeVectorAllocator* code_allocator,
                                              const DexFile::CodeItem* code_item,
                                              uint32_t access_flags,
                                              InvokeType invoke_type,
                                              uint16_t class_def_idx,
                                              uint32_t method_idx,
                                              jobject class_loader,
                                              const DexFile& dex_file,
                                              Handle<mirror::DexCache> dex_cache) const {
  MaybeRecordStat(MethodCompilationStat::kAttemptCompilation);
  CompilerDriver* compiler_driver = GetCompilerDriver();
  InstructionSet instruction_set = compiler_driver->GetInstructionSet();

  // Always use the Thumb-2 assembler: some runtime functionality
  // (like implicit stack overflow checks) assume Thumb-2.
  if (instruction_set == kArm) {
    instruction_set = kThumb2;
  }

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledUnsupportedIsa);
    return nullptr;
  }

  // When read barriers are enabled, do not attempt to compile for
  // instruction sets that have no read barrier support.
  if (kEmitCompilerReadBarrier && !InstructionSetSupportsReadBarrier(instruction_set)) {
    return nullptr;
  }

  if (Compiler::IsPathologicalCase(*code_item, method_idx, dex_file)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledPathological);
    return nullptr;
  }

  // Implementation of the space filter: do not compile a code item whose size in
  // code units is bigger than 128.
  static constexpr size_t kSpaceFilterOptimizingThreshold = 128;
  const CompilerOptions& compiler_options = compiler_driver->GetCompilerOptions();
  if ((compiler_options.GetCompilerFilter() == CompilerOptions::kSpace)
      && (code_item->insns_size_in_code_units_ > kSpaceFilterOptimizingThreshold)) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledSpaceFilter);
    return nullptr;
  }

  DexCompilationUnit dex_compilation_unit(
    nullptr, class_loader, Runtime::Current()->GetClassLinker(), dex_file, code_item,
    class_def_idx, method_idx, access_flags,
    compiler_driver->GetVerifiedMethod(&dex_file, method_idx), dex_cache);

  bool requires_barrier = dex_compilation_unit.IsConstructor()
      && compiler_driver->RequiresConstructorBarrier(Thread::Current(),
                                                     dex_compilation_unit.GetDexFile(),
                                                     dex_compilation_unit.GetClassDefIndex());
  HGraph* graph = new (arena) HGraph(
      arena, dex_file, method_idx, requires_barrier, compiler_driver->GetInstructionSet(),
      kInvalidInvokeType, compiler_driver->GetCompilerOptions().GetDebuggable());

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            instruction_set,
                            *compiler_driver->GetInstructionSetFeatures(),
                            compiler_driver->GetCompilerOptions()));
  if (codegen.get() == nullptr) {
    MaybeRecordStat(MethodCompilationStat::kNotCompiledNoCodegen);
    return nullptr;
  }
  codegen->GetAssembler()->cfi().SetEnabled(
      compiler_driver->GetCompilerOptions().GetGenerateDebugInfo());

  PassObserver pass_observer(graph,
                             codegen.get(),
                             visualizer_output_.get(),
                             compiler_driver);

  const uint8_t* interpreter_metadata = nullptr;
  {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::ClassLoader> loader(hs.NewHandle(
        soa.Decode<mirror::ClassLoader*>(class_loader)));
    ArtMethod* art_method = compiler_driver->ResolveMethod(
        soa, dex_cache, loader, &dex_compilation_unit, method_idx, invoke_type);
    // We may not get a method, for example if its class is erroneous.
    // TODO: Clean this up, the compiler driver should just pass the ArtMethod to compile.
    if (art_method != nullptr) {
      interpreter_metadata = art_method->GetQuickenedInfo();
    }
  }
  HGraphBuilder builder(graph,
                        &dex_compilation_unit,
                        &dex_compilation_unit,
                        &dex_file,
                        compiler_driver,
                        compilation_stats_.get(),
                        interpreter_metadata,
                        dex_cache);

  VLOG(compiler) << "Building " << pass_observer.GetMethodName();

  {
    PassScope scope(HGraphBuilder::kBuilderPassName, &pass_observer);
    if (!builder.BuildGraph(*code_item)) {
      pass_observer.SetGraphInBadState();
      return nullptr;
    }
  }

  VLOG(compiler) << "Optimizing " << pass_observer.GetMethodName();
  if (run_optimizations_) {
    {
      PassScope scope(SsaBuilder::kSsaBuilderPassName, &pass_observer);
      if (!graph->TryBuildingSsa()) {
        // We could not transform the graph to SSA, bailout.
        LOG(INFO) << "Skipping compilation of " << pass_observer.GetMethodName()
            << ": it contains a non natural loop";
        MaybeRecordStat(MethodCompilationStat::kNotCompiledCannotBuildSSA);
        pass_observer.SetGraphInBadState();
        return nullptr;
      }
    }

    RunOptimizations(graph,
                     codegen.get(),
                     compiler_driver,
                     compilation_stats_.get(),
                     dex_compilation_unit,
                     &pass_observer);
    codegen->CompileOptimized(code_allocator);
  } else {
    codegen->CompileBaseline(code_allocator);
  }
  pass_observer.DumpDisassembly();

  if (kArenaAllocatorCountAllocations) {
    if (arena->BytesAllocated() > 4 * MB) {
      MemStats mem_stats(arena->GetMemStats());
      LOG(INFO) << PrettyMethod(method_idx, dex_file) << " " << Dumpable<MemStats>(mem_stats);
    }
  }

  return codegen.release();
}

static bool CanHandleVerificationFailure(const VerifiedMethod* verified_method) {
  // For access errors the compiler will use the unresolved helpers (e.g. HInvokeUnresolved).
  uint32_t unresolved_mask = verifier::VerifyError::VERIFY_ERROR_NO_CLASS
      | verifier::VerifyError::VERIFY_ERROR_ACCESS_CLASS
      | verifier::VerifyError::VERIFY_ERROR_ACCESS_FIELD
      | verifier::VerifyError::VERIFY_ERROR_ACCESS_METHOD;
  return (verified_method->GetEncounteredVerificationFailures() & (~unresolved_mask)) == 0;
}

CompiledMethod* OptimizingCompiler::Compile(const DexFile::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            jobject jclass_loader,
                                            const DexFile& dex_file,
                                            Handle<mirror::DexCache> dex_cache) const {
  CompilerDriver* compiler_driver = GetCompilerDriver();
  CompiledMethod* method = nullptr;
  DCHECK(Runtime::Current()->IsAotCompiler());
  const VerifiedMethod* verified_method = compiler_driver->GetVerifiedMethod(&dex_file, method_idx);
  DCHECK(!verified_method->HasRuntimeThrow());
  if (compiler_driver->IsMethodVerifiedWithoutFailures(method_idx, class_def_idx, dex_file)
      || CanHandleVerificationFailure(verified_method)) {
    ArenaAllocator arena(Runtime::Current()->GetArenaPool());
    CodeVectorAllocator code_allocator(&arena);
    std::unique_ptr<CodeGenerator> codegen(
        TryCompile(&arena,
                   &code_allocator,
                   code_item,
                   access_flags,
                   invoke_type,
                   class_def_idx,
                   method_idx,
                   jclass_loader,
                   dex_file,
                   dex_cache));
    if (codegen.get() != nullptr) {
      MaybeRecordStat(MethodCompilationStat::kCompiled);
      if (run_optimizations_) {
        method = EmitOptimized(&arena, &code_allocator, codegen.get(), compiler_driver);
      } else {
        method = EmitBaseline(&arena, &code_allocator, codegen.get(), compiler_driver);
      }
    }
  } else {
    if (compiler_driver->GetCompilerOptions().VerifyAtRuntime()) {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledVerifyAtRuntime);
    } else {
      MaybeRecordStat(MethodCompilationStat::kNotCompiledVerificationError);
    }
  }

  if (kIsDebugBuild &&
      IsCompilingWithCoreImage() &&
      IsInstructionSetSupported(compiler_driver->GetInstructionSet()) &&
      (!kEmitCompilerReadBarrier ||
       InstructionSetSupportsReadBarrier(compiler_driver->GetInstructionSet()))) {
    // For testing purposes, we put a special marker on method names
    // that should be compiled with this compiler (when the the
    // instruction set is supported -- and has support for read
    // barriers, if they are enabled). This makes sure we're not
    // regressing.
    std::string method_name = PrettyMethod(method_idx, dex_file);
    bool shouldCompile = method_name.find("$opt$") != std::string::npos;
    DCHECK((method != nullptr) || !shouldCompile) << "Didn't compile " << method_name;
  }

  return method;
}

Compiler* CreateOptimizingCompiler(CompilerDriver* driver) {
  return new OptimizingCompiler(driver);
}

bool IsCompilingWithCoreImage() {
  const std::string& image = Runtime::Current()->GetImageLocation();
  return EndsWith(image, "core.art") || EndsWith(image, "core-optimizing.art");
}

bool OptimizingCompiler::JitCompile(Thread* self,
                                    jit::JitCodeCache* code_cache,
                                    ArtMethod* method) {
  StackHandleScope<2> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));

  jobject jclass_loader = class_loader.ToJObject();
  const DexFile* dex_file = method->GetDexFile();
  const uint16_t class_def_idx = method->GetClassDefIndex();
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  const uint32_t method_idx = method->GetDexMethodIndex();
  const uint32_t access_flags = method->GetAccessFlags();
  const InvokeType invoke_type = method->GetInvokeType();

  ArenaAllocator arena(Runtime::Current()->GetArenaPool());
  CodeVectorAllocator code_allocator(&arena);
  std::unique_ptr<CodeGenerator> codegen;
  {
    // Go to native so that we don't block GC during compilation.
    ScopedThreadSuspension sts(self, kNative);

    DCHECK(run_optimizations_);
    codegen.reset(
        TryCompile(&arena,
                   &code_allocator,
                   code_item,
                   access_flags,
                   invoke_type,
                   class_def_idx,
                   method_idx,
                   jclass_loader,
                   *dex_file,
                   dex_cache));
    if (codegen.get() == nullptr) {
      return false;
    }
  }

  size_t stack_map_size = codegen->ComputeStackMapsSize();
  uint8_t* stack_map_data = code_cache->ReserveData(self, stack_map_size);
  if (stack_map_data == nullptr) {
    return false;
  }
  codegen->BuildStackMaps(MemoryRegion(stack_map_data, stack_map_size));
  const void* code = code_cache->CommitCode(
      self,
      method,
      nullptr,
      stack_map_data,
      nullptr,
      codegen->HasEmptyFrame() ? 0 : codegen->GetFrameSize(),
      codegen->GetCoreSpillMask(),
      codegen->GetFpuSpillMask(),
      code_allocator.GetMemory().data(),
      code_allocator.GetSize());

  if (code == nullptr) {
    code_cache->ClearData(self, stack_map_data);
    return false;
  }

  return true;
}

}  // namespace art
