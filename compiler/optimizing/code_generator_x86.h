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

#ifndef ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
#define ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_

#include "code_generator.h"
#include "dex/compiler_enums.h"
#include "driver/compiler_options.h"
#include "nodes.h"
#include "parallel_move_resolver.h"
#include "utils/x86/assembler_x86.h"

namespace art {
namespace x86 {

// Use a local definition to prevent copying mistakes.
static constexpr size_t kX86WordSize = kX86PointerSize;

class CodeGeneratorX86;

static constexpr Register kParameterCoreRegisters[] = { ECX, EDX, EBX };
static constexpr RegisterPair kParameterCorePairRegisters[] = { ECX_EDX, EDX_EBX };
static constexpr size_t kParameterCoreRegistersLength = arraysize(kParameterCoreRegisters);
static constexpr XmmRegister kParameterFpuRegisters[] = { XMM0, XMM1, XMM2, XMM3 };
static constexpr size_t kParameterFpuRegistersLength = arraysize(kParameterFpuRegisters);

static constexpr Register kRuntimeParameterCoreRegisters[] = { EAX, ECX, EDX, EBX };
static constexpr size_t kRuntimeParameterCoreRegistersLength =
    arraysize(kRuntimeParameterCoreRegisters);
static constexpr XmmRegister kRuntimeParameterFpuRegisters[] = { XMM0, XMM1, XMM2, XMM3 };
static constexpr size_t kRuntimeParameterFpuRegistersLength =
    arraysize(kRuntimeParameterFpuRegisters);

class InvokeRuntimeCallingConvention : public CallingConvention<Register, XmmRegister> {
 public:
  InvokeRuntimeCallingConvention()
      : CallingConvention(kRuntimeParameterCoreRegisters,
                          kRuntimeParameterCoreRegistersLength,
                          kRuntimeParameterFpuRegisters,
                          kRuntimeParameterFpuRegistersLength,
                          kX86PointerSize) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeRuntimeCallingConvention);
};

class InvokeDexCallingConvention : public CallingConvention<Register, XmmRegister> {
 public:
  InvokeDexCallingConvention() : CallingConvention(
      kParameterCoreRegisters,
      kParameterCoreRegistersLength,
      kParameterFpuRegisters,
      kParameterFpuRegistersLength,
      kX86PointerSize) {}

  RegisterPair GetRegisterPairAt(size_t argument_index) {
    DCHECK_LT(argument_index + 1, GetNumberOfRegisters());
    return kParameterCorePairRegisters[argument_index];
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConvention);
};

class InvokeDexCallingConventionVisitorX86 : public InvokeDexCallingConventionVisitor {
 public:
  InvokeDexCallingConventionVisitorX86() {}
  virtual ~InvokeDexCallingConventionVisitorX86() {}

  Location GetNextLocation(Primitive::Type type) OVERRIDE;
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE;
  Location GetMethodLocation() const OVERRIDE;

 private:
  InvokeDexCallingConvention calling_convention;

  DISALLOW_COPY_AND_ASSIGN(InvokeDexCallingConventionVisitorX86);
};

class FieldAccessCallingConventionX86 : public FieldAccessCallingConvention {
 public:
  FieldAccessCallingConventionX86() {}

  Location GetObjectLocation() const OVERRIDE {
    return Location::RegisterLocation(ECX);
  }
  Location GetFieldIndexLocation() const OVERRIDE {
    return Location::RegisterLocation(EAX);
  }
  Location GetReturnLocation(Primitive::Type type) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(EAX, EDX)
        : Location::RegisterLocation(EAX);
  }
  Location GetSetValueLocation(Primitive::Type type, bool is_instance) const OVERRIDE {
    return Primitive::Is64BitType(type)
        ? Location::RegisterPairLocation(EDX, EBX)
        : (is_instance
            ? Location::RegisterLocation(EDX)
            : Location::RegisterLocation(ECX));
  }
  Location GetFpuLocation(Primitive::Type type ATTRIBUTE_UNUSED) const OVERRIDE {
    return Location::FpuRegisterLocation(XMM0);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FieldAccessCallingConventionX86);
};

class ParallelMoveResolverX86 : public ParallelMoveResolverWithSwap {
 public:
  ParallelMoveResolverX86(ArenaAllocator* allocator, CodeGeneratorX86* codegen)
      : ParallelMoveResolverWithSwap(allocator), codegen_(codegen) {}

  void EmitMove(size_t index) OVERRIDE;
  void EmitSwap(size_t index) OVERRIDE;
  void SpillScratch(int reg) OVERRIDE;
  void RestoreScratch(int reg) OVERRIDE;

  X86Assembler* GetAssembler() const;

 private:
  void Exchange(Register reg, int mem);
  void Exchange(int mem1, int mem2);
  void Exchange32(XmmRegister reg, int mem);
  void MoveMemoryToMemory32(int dst, int src);
  void MoveMemoryToMemory64(int dst, int src);

  CodeGeneratorX86* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(ParallelMoveResolverX86);
};

class LocationsBuilderX86 : public HGraphVisitor {
 public:
  LocationsBuilderX86(HGraph* graph, CodeGeneratorX86* codegen)
      : HGraphVisitor(graph), codegen_(codegen) {}

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

 private:
  void HandleBitwiseOperation(HBinaryOperation* instruction);
  void HandleInvoke(HInvoke* invoke);
  void HandleShift(HBinaryOperation* instruction);
  void HandleFieldSet(HInstruction* instruction, const FieldInfo& field_info);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);

  CodeGeneratorX86* const codegen_;
  InvokeDexCallingConventionVisitorX86 parameter_visitor_;

  DISALLOW_COPY_AND_ASSIGN(LocationsBuilderX86);
};

class InstructionCodeGeneratorX86 : public HGraphVisitor {
 public:
  InstructionCodeGeneratorX86(HGraph* graph, CodeGeneratorX86* codegen);

#define DECLARE_VISIT_INSTRUCTION(name, super)     \
  void Visit##name(H##name* instr) OVERRIDE;

  FOR_EACH_CONCRETE_INSTRUCTION_COMMON(DECLARE_VISIT_INSTRUCTION)
  FOR_EACH_CONCRETE_INSTRUCTION_X86(DECLARE_VISIT_INSTRUCTION)

#undef DECLARE_VISIT_INSTRUCTION

  void VisitInstruction(HInstruction* instruction) OVERRIDE {
    LOG(FATAL) << "Unreachable instruction " << instruction->DebugName()
               << " (id " << instruction->GetId() << ")";
  }

  X86Assembler* GetAssembler() const { return assembler_; }

 private:
  // Generate code for the given suspend check. If not null, `successor`
  // is the block to branch to if the suspend check is not needed, and after
  // the suspend call.
  void GenerateSuspendCheck(HSuspendCheck* check, HBasicBlock* successor);
  void GenerateClassInitializationCheck(SlowPathCode* slow_path, Register class_reg);
  void HandleBitwiseOperation(HBinaryOperation* instruction);
  void GenerateDivRemIntegral(HBinaryOperation* instruction);
  void DivRemOneOrMinusOne(HBinaryOperation* instruction);
  void DivByPowerOfTwo(HDiv* instruction);
  void GenerateDivRemWithAnyConstant(HBinaryOperation* instruction);
  void GenerateRemFP(HRem* rem);
  void HandleShift(HBinaryOperation* instruction);
  void GenerateShlLong(const Location& loc, Register shifter);
  void GenerateShrLong(const Location& loc, Register shifter);
  void GenerateUShrLong(const Location& loc, Register shifter);
  void GenerateShlLong(const Location& loc, int shift);
  void GenerateShrLong(const Location& loc, int shift);
  void GenerateUShrLong(const Location& loc, int shift);
  void GenerateMemoryBarrier(MemBarrierKind kind);
  void HandleFieldSet(HInstruction* instruction,
                      const FieldInfo& field_info,
                      bool value_can_be_null);
  void HandleFieldGet(HInstruction* instruction, const FieldInfo& field_info);
  // Push value to FPU stack. `is_fp` specifies whether the value is floating point or not.
  // `is_wide` specifies whether it is long/double or not.
  void PushOntoFPStack(Location source, uint32_t temp_offset,
                       uint32_t stack_adjustment, bool is_fp, bool is_wide);

  void GenerateImplicitNullCheck(HNullCheck* instruction);
  void GenerateExplicitNullCheck(HNullCheck* instruction);
  void GenerateTestAndBranch(HInstruction* instruction,
                             size_t condition_input_index,
                             Label* true_target,
                             Label* false_target);
  void GenerateCompareTestAndBranch(HCondition* condition,
                                    Label* true_target,
                                    Label* false_target);
  void GenerateFPJumps(HCondition* cond, Label* true_label, Label* false_label);
  void GenerateLongComparesAndJumps(HCondition* cond, Label* true_label, Label* false_label);
  void HandleGoto(HInstruction* got, HBasicBlock* successor);

  X86Assembler* const assembler_;
  CodeGeneratorX86* const codegen_;

  DISALLOW_COPY_AND_ASSIGN(InstructionCodeGeneratorX86);
};

class JumpTableRIPFixup;

class CodeGeneratorX86 : public CodeGenerator {
 public:
  CodeGeneratorX86(HGraph* graph,
                   const X86InstructionSetFeatures& isa_features,
                   const CompilerOptions& compiler_options,
                   OptimizingCompilerStats* stats = nullptr);
  virtual ~CodeGeneratorX86() {}

  void GenerateFrameEntry() OVERRIDE;
  void GenerateFrameExit() OVERRIDE;
  void Bind(HBasicBlock* block) OVERRIDE;
  void Move(HInstruction* instruction, Location location, HInstruction* move_for) OVERRIDE;
  void MoveConstant(Location destination, int32_t value) OVERRIDE;
  void MoveLocation(Location dst, Location src, Primitive::Type dst_type) OVERRIDE;
  void AddLocationAsTemp(Location location, LocationSummary* locations) OVERRIDE;

  size_t SaveCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreCoreRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t SaveFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;
  size_t RestoreFloatingPointRegister(size_t stack_index, uint32_t reg_id) OVERRIDE;

  // Generate code to invoke a runtime entry point.
  void InvokeRuntime(QuickEntrypointEnum entrypoint,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path) OVERRIDE;

  void InvokeRuntime(int32_t entry_point_offset,
                     HInstruction* instruction,
                     uint32_t dex_pc,
                     SlowPathCode* slow_path);

  size_t GetWordSize() const OVERRIDE {
    return kX86WordSize;
  }

  size_t GetFloatingPointSpillSlotSize() const OVERRIDE {
    // 8 bytes == 2 words for each spill.
    return 2 * kX86WordSize;
  }

  HGraphVisitor* GetLocationBuilder() OVERRIDE {
    return &location_builder_;
  }

  HGraphVisitor* GetInstructionVisitor() OVERRIDE {
    return &instruction_visitor_;
  }

  X86Assembler* GetAssembler() OVERRIDE {
    return &assembler_;
  }

  const X86Assembler& GetAssembler() const OVERRIDE {
    return assembler_;
  }

  uintptr_t GetAddressOf(HBasicBlock* block) const OVERRIDE {
    return GetLabelOf(block)->Position();
  }

  void SetupBlockedRegisters(bool is_baseline) const OVERRIDE;

  Location AllocateFreeRegister(Primitive::Type type) const OVERRIDE;

  Location GetStackLocation(HLoadLocal* load) const OVERRIDE;

  void DumpCoreRegister(std::ostream& stream, int reg) const OVERRIDE;
  void DumpFloatingPointRegister(std::ostream& stream, int reg) const OVERRIDE;

  // Blocks all register pairs made out of blocked core registers.
  void UpdateBlockedPairRegisters() const;

  ParallelMoveResolverX86* GetMoveResolver() OVERRIDE {
    return &move_resolver_;
  }

  InstructionSet GetInstructionSet() const OVERRIDE {
    return InstructionSet::kX86;
  }

  // Helper method to move a 32bits value between two locations.
  void Move32(Location destination, Location source);
  // Helper method to move a 64bits value between two locations.
  void Move64(Location destination, Location source);

  // Check if the desired_dispatch_info is supported. If it is, return it,
  // otherwise return a fall-back info that should be used instead.
  HInvokeStaticOrDirect::DispatchInfo GetSupportedInvokeStaticOrDirectDispatch(
      const HInvokeStaticOrDirect::DispatchInfo& desired_dispatch_info,
      MethodReference target_method) OVERRIDE;

  // Generate a call to a static or direct method.
  void GenerateStaticOrDirectCall(HInvokeStaticOrDirect* invoke, Location temp) OVERRIDE;
  // Generate a call to a virtual method.
  void GenerateVirtualCall(HInvokeVirtual* invoke, Location temp) OVERRIDE;

  void MoveFromReturnRegister(Location trg, Primitive::Type type) OVERRIDE;

  // Emit linker patches.
  void EmitLinkerPatches(ArenaVector<LinkerPatch>* linker_patches) OVERRIDE;

  // Emit a write barrier.
  void MarkGCCard(Register temp,
                  Register card,
                  Register object,
                  Register value,
                  bool value_can_be_null);

  Label* GetLabelOf(HBasicBlock* block) const {
    return CommonGetLabelOf<Label>(block_labels_, block);
  }

  void Initialize() OVERRIDE {
    block_labels_ = CommonInitializeLabels<Label>();
  }

  bool NeedsTwoRegisters(Primitive::Type type) const OVERRIDE {
    return type == Primitive::kPrimLong;
  }

  bool ShouldSplitLongMoves() const OVERRIDE { return true; }

  Label* GetFrameEntryLabel() { return &frame_entry_label_; }

  const X86InstructionSetFeatures& GetInstructionSetFeatures() const {
    return isa_features_;
  }

  void SetMethodAddressOffset(int32_t offset) {
    method_address_offset_ = offset;
  }

  int32_t GetMethodAddressOffset() const {
    return method_address_offset_;
  }

  int32_t ConstantAreaStart() const {
    return constant_area_start_;
  }

  Address LiteralDoubleAddress(double v, Register reg);
  Address LiteralFloatAddress(float v, Register reg);
  Address LiteralInt32Address(int32_t v, Register reg);
  Address LiteralInt64Address(int64_t v, Register reg);

  Address LiteralCaseTable(HX86PackedSwitch* switch_instr, Register reg, Register value);

  void Finalize(CodeAllocator* allocator) OVERRIDE;

  // Generate a read barrier for a heap reference within `instruction`.
  //
  // A read barrier for an object reference read from the heap is
  // implemented as a call to the artReadBarrierSlow runtime entry
  // point, which is passed the values in locations `ref`, `obj`, and
  // `offset`:
  //
  //   mirror::Object* artReadBarrierSlow(mirror::Object* ref,
  //                                      mirror::Object* obj,
  //                                      uint32_t offset);
  //
  // The `out` location contains the value returned by
  // artReadBarrierSlow.
  //
  // When `index` is provided (i.e. for array accesses), the offset
  // value passed to artReadBarrierSlow is adjusted to take `index`
  // into account.
  void GenerateReadBarrier(HInstruction* instruction,
                           Location out,
                           Location ref,
                           Location obj,
                           uint32_t offset,
                           Location index = Location::NoLocation());

  // If read barriers are enabled, generate a read barrier for a heap reference.
  // If heap poisoning is enabled, also unpoison the reference in `out`.
  void MaybeGenerateReadBarrier(HInstruction* instruction,
                                Location out,
                                Location ref,
                                Location obj,
                                uint32_t offset,
                                Location index = Location::NoLocation());

  // Generate a read barrier for a GC root within `instruction`.
  //
  // A read barrier for an object reference GC root is implemented as
  // a call to the artReadBarrierForRootSlow runtime entry point,
  // which is passed the value in location `root`:
  //
  //   mirror::Object* artReadBarrierForRootSlow(GcRoot<mirror::Object>* root);
  //
  // The `out` location contains the value returned by
  // artReadBarrierForRootSlow.
  void GenerateReadBarrierForRoot(HInstruction* instruction, Location out, Location root);

 private:
  Register GetInvokeStaticOrDirectExtraParameter(HInvokeStaticOrDirect* invoke, Register temp);

  struct PcRelativeDexCacheAccessInfo {
    PcRelativeDexCacheAccessInfo(const DexFile& dex_file, uint32_t element_off)
        : target_dex_file(dex_file), element_offset(element_off), label() { }

    const DexFile& target_dex_file;
    uint32_t element_offset;
    // NOTE: Label is bound to the end of the instruction that has an embedded 32-bit offset.
    Label label;
  };

  // Labels for each block that will be compiled.
  Label* block_labels_;  // Indexed by block id.
  Label frame_entry_label_;
  LocationsBuilderX86 location_builder_;
  InstructionCodeGeneratorX86 instruction_visitor_;
  ParallelMoveResolverX86 move_resolver_;
  X86Assembler assembler_;
  const X86InstructionSetFeatures& isa_features_;

  // Method patch info. Using ArenaDeque<> which retains element addresses on push/emplace_back().
  ArenaDeque<MethodPatchInfo<Label>> method_patches_;
  ArenaDeque<MethodPatchInfo<Label>> relative_call_patches_;
  // PC-relative DexCache access info.
  ArenaDeque<PcRelativeDexCacheAccessInfo> pc_relative_dex_cache_patches_;

  // Offset to the start of the constant area in the assembled code.
  // Used for fixups to the constant area.
  int32_t constant_area_start_;

  // Fixups for jump tables that need to be patched after the constant table is generated.
  ArenaVector<JumpTableRIPFixup*> fixups_to_jump_tables_;

  // If there is a HX86ComputeBaseMethodAddress instruction in the graph
  // (which shall be the sole instruction of this kind), subtracting this offset
  // from the value contained in the out register of this HX86ComputeBaseMethodAddress
  // instruction gives the address of the start of this method.
  int32_t method_address_offset_;

  // When we don't know the proper offset for the value, we use kDummy32BitOffset.
  // The correct value will be inserted when processing Assembler fixups.
  static constexpr int32_t kDummy32BitOffset = 256;

  DISALLOW_COPY_AND_ASSIGN(CodeGeneratorX86);
};

}  // namespace x86
}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_CODE_GENERATOR_X86_H_
