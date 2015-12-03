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

#include "pc_relative_fixups_x86.h"

namespace art {
namespace x86 {

/**
 * Finds instructions that need the constant area base as an input.
 */
class PCRelativeHandlerVisitor : public HGraphVisitor {
 public:
  explicit PCRelativeHandlerVisitor(HGraph* graph) : HGraphVisitor(graph), base_(nullptr) {}

  void MoveBaseIfNeeded() {
    if (base_ != nullptr) {
      // Bring the base closer to the first use (previously, it was in the
      // entry block) and relieve some pressure on the register allocator
      // while avoiding recalculation of the base in a loop.
      base_->MoveBeforeFirstUserAndOutOfLoops();
    }
  }

 private:
  void VisitAdd(HAdd* add) OVERRIDE {
    BinaryFP(add);
  }

  void VisitSub(HSub* sub) OVERRIDE {
    BinaryFP(sub);
  }

  void VisitMul(HMul* mul) OVERRIDE {
    BinaryFP(mul);
  }

  void VisitDiv(HDiv* div) OVERRIDE {
    BinaryFP(div);
  }

  void VisitReturn(HReturn* ret) OVERRIDE {
    HConstant* value = ret->InputAt(0)->AsConstant();
    if ((value != nullptr && Primitive::IsFloatingPointType(value->GetType()))) {
      ReplaceInput(ret, value, 0, true);
    }
  }

  void VisitInvokeStaticOrDirect(HInvokeStaticOrDirect* invoke) OVERRIDE {
    HandleInvoke(invoke);
  }

  void VisitInvokeVirtual(HInvokeVirtual* invoke) OVERRIDE {
    HandleInvoke(invoke);
  }

  void VisitInvokeInterface(HInvokeInterface* invoke) OVERRIDE {
    HandleInvoke(invoke);
  }

  void BinaryFP(HBinaryOperation* bin) {
    HConstant* rhs = bin->InputAt(1)->AsConstant();
    if (rhs != nullptr && Primitive::IsFloatingPointType(bin->GetResultType())) {
      ReplaceInput(bin, rhs, 1, false);
    }
  }

  void VisitPackedSwitch(HPackedSwitch* switch_insn) OVERRIDE {
    // We need to replace the HPackedSwitch with a HX86PackedSwitch in order to
    // address the constant area.
    InitializePCRelativeBasePointer();
    HGraph* graph = GetGraph();
    HBasicBlock* block = switch_insn->GetBlock();
    HX86PackedSwitch* x86_switch = new (graph->GetArena()) HX86PackedSwitch(
        switch_insn->GetStartValue(),
        switch_insn->GetNumEntries(),
        switch_insn->InputAt(0),
        base_,
        switch_insn->GetDexPc());
    block->ReplaceAndRemoveInstructionWith(switch_insn, x86_switch);
  }

  void InitializePCRelativeBasePointer() {
    // Ensure we only initialize the pointer once.
    if (base_ != nullptr) {
      return;
    }

    // Insert the base at the start of the entry block, move it to a better
    // position later in MoveBaseIfNeeded().
    base_ = new (GetGraph()->GetArena()) HX86ComputeBaseMethodAddress();
    HBasicBlock* entry_block = GetGraph()->GetEntryBlock();
    entry_block->InsertInstructionBefore(base_, entry_block->GetFirstInstruction());
    DCHECK(base_ != nullptr);
  }

  void ReplaceInput(HInstruction* insn, HConstant* value, int input_index, bool materialize) {
    InitializePCRelativeBasePointer();
    HX86LoadFromConstantTable* load_constant =
        new (GetGraph()->GetArena()) HX86LoadFromConstantTable(base_, value, materialize);
    insn->GetBlock()->InsertInstructionBefore(load_constant, insn);
    insn->ReplaceInput(load_constant, input_index);
  }

  void HandleInvoke(HInvoke* invoke) {
    // If this is an invoke-static/-direct with PC-relative dex cache array
    // addressing, we need the PC-relative address base.
    HInvokeStaticOrDirect* invoke_static_or_direct = invoke->AsInvokeStaticOrDirect();
    if (invoke_static_or_direct != nullptr && invoke_static_or_direct->HasPcRelativeDexCache()) {
      InitializePCRelativeBasePointer();
      // Add the extra parameter base_.
      DCHECK(!invoke_static_or_direct->HasCurrentMethodInput());
      invoke_static_or_direct->AddSpecialInput(base_);
    }
    // Ensure that we can load FP arguments from the constant area.
    for (size_t i = 0, e = invoke->InputCount(); i < e; i++) {
      HConstant* input = invoke->InputAt(i)->AsConstant();
      if (input != nullptr && Primitive::IsFloatingPointType(input->GetType())) {
        ReplaceInput(invoke, input, i, true);
      }
    }
  }

  // The generated HX86ComputeBaseMethodAddress in the entry block needed as an
  // input to the HX86LoadFromConstantTable instructions.
  HX86ComputeBaseMethodAddress* base_;
};

void PcRelativeFixups::Run() {
  PCRelativeHandlerVisitor visitor(graph_);
  visitor.VisitInsertionOrder();
  visitor.MoveBaseIfNeeded();
}

}  // namespace x86
}  // namespace art
