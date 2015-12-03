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

#ifndef ART_COMPILER_OPTIMIZING_INLINER_H_
#define ART_COMPILER_OPTIMIZING_INLINER_H_

#include "invoke_type.h"
#include "optimization.h"

#ifdef QC_STRONG
#define QC_WEAK
#else
#define QC_WEAK __attribute__((weak))
#endif

namespace art {

class CodeGenerator;
class CompilerDriver;
class DexCompilationUnit;
class HGraph;
class HInvoke;
class OptimizingCompilerStats;

class HInliner : public HOptimization {
 public:
  HInliner(HGraph* outer_graph,
           CodeGenerator* codegen,
           const DexCompilationUnit& outer_compilation_unit,
           const DexCompilationUnit& caller_compilation_unit,
           CompilerDriver* compiler_driver,
           StackHandleScopeCollection* handles,
           OptimizingCompilerStats* stats,
           size_t depth = 0)
      : HOptimization(outer_graph, kInlinerPassName, stats),
        outer_compilation_unit_(outer_compilation_unit),
        caller_compilation_unit_(caller_compilation_unit),
        codegen_(codegen),
        compiler_driver_(compiler_driver),
        depth_(depth),
        number_of_inlined_instructions_(0),
        handles_(handles) {}

  void Run() OVERRIDE;

  static constexpr const char* kInlinerPassName = "inliner";

 private:
  bool TryInline(HInvoke* invoke_instruction);
  bool TryBuildAndInline(ArtMethod* resolved_method,
                         HInvoke* invoke_instruction,
                         bool same_dex_file);

  const DexCompilationUnit& outer_compilation_unit_;
  const DexCompilationUnit& caller_compilation_unit_;
  CodeGenerator* const codegen_;
  CompilerDriver* const compiler_driver_;
  const size_t depth_;
  size_t number_of_inlined_instructions_;
  StackHandleScopeCollection* const handles_;

  DISALLOW_COPY_AND_ASSIGN(HInliner);
};

}  // namespace art

#endif  // ART_COMPILER_OPTIMIZING_INLINER_H_
