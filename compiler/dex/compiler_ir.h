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

#ifndef ART_COMPILER_DEX_COMPILER_IR_H_
#define ART_COMPILER_DEX_COMPILER_IR_H_

#include "jni.h"
#include <string>
#include <vector>

#include "arch/instruction_set.h"
#include "base/arena_allocator.h"
#include "base/scoped_arena_allocator.h"
#include "base/timing_logger.h"
#include "invoke_type.h"
#include "safe_map.h"
#include "quick/quick_compiler.h"

namespace art {

class ClassLinker;
class CompilerDriver;
class DexFile;
class Mir2Lir;
class MIRGraph;

constexpr size_t kOptionStringMaxLength = 2048;

/**
 * Structure abstracting pass option values, which can be of type string or integer.
 */
struct OptionContent {
  OptionContent(const OptionContent& option) :
    type(option.type), container(option.container, option.type) {}

  explicit OptionContent(const char* value) :
    type(kString), container(value) {}

  explicit OptionContent(int value) :
    type(kInteger), container(value) {}

  explicit OptionContent(int64_t value) :
    type(kInteger), container(value) {}

  ~OptionContent() {
    if (type == kString) {
      container.StringDelete();
    }
  }

  /**
   * Allows for a transparent display of the option content.
   */
  friend std::ostream& operator<<(std::ostream& out, const OptionContent& option) {
    if (option.type == kString) {
      out << option.container.s;
    } else {
      out << option.container.i;
    }

    return out;
  }

  inline const char* GetString() const {
    return container.s;
  }

  inline int64_t GetInteger() const {
    return container.i;
  }

  /**
   * @brief Used to compare a string option value to a given @p value.
   * @details Will return whether the internal string option is equal to
   * the parameter @p value. It will return false if the type of the
   * object is not a string.
   * @param value The string to compare to.
   * @return Returns whether the internal string option is equal to the
   * parameter @p value.
  */
  inline bool Equals(const char* value) const {
    DCHECK(value != nullptr);
    if (type != kString) {
      return false;
    }
    return !strncmp(container.s, value, kOptionStringMaxLength);
  }

  /**
   * @brief Used to compare an integer option value to a given @p value.
   * @details Will return whether the internal integer option is equal to
   * the parameter @p value. It will return false if the type of the
   * object is not an integer.
   * @param value The integer to compare to.
   * @return Returns whether the internal integer option is equal to the
   * parameter @p value.
  */
  inline bool Equals(int64_t value) const {
    if (type != kInteger) {
      return false;
    }
    return container.i == value;
  }

  /**
   * Describes the type of parameters allowed as option values.
   */
  enum OptionType {
    kString = 0,
    kInteger
  };

  OptionType type;

 private:
  /**
   * Union containing the option value of either type.
   */
  union OptionContainer {
    OptionContainer(const OptionContainer& c, OptionType t) {
      if (t == kString) {
        DCHECK(c.s != nullptr);
        s = strndup(c.s, kOptionStringMaxLength);
      } else {
        i = c.i;
      }
    }

    explicit OptionContainer(const char* value) {
      DCHECK(value != nullptr);
      s = strndup(value, kOptionStringMaxLength);
    }

    explicit OptionContainer(int64_t value) : i(value) {}
    ~OptionContainer() {}

    void StringDelete() {
      if (s != nullptr) {
        free(s);
      }
    }

    char* s;
    int64_t i;
  };

  OptionContainer container;
};

struct CompilationUnit {
  CompilationUnit(ArenaPool* pool, InstructionSet isa, CompilerDriver* driver, ClassLinker* linker);
  CompilationUnit(ArenaPool* pool, InstructionSet isa, CompilerDriver* driver, ClassLinker* linker,
                  const QuickCompiler* compiler);
  ~CompilationUnit();

  void StartTimingSplit(const char* label);
  void NewTimingSplit(const char* label);
  void EndTiming();

  /*
   * Fields needed/generated by common frontend and generally used throughout
   * the compiler.
  */
  CompilerDriver* const compiler_driver;
  ClassLinker* const class_linker;        // Linker to resolve fields and methods.
  const DexFile* dex_file;                // DexFile containing the method being compiled.
  jobject class_loader;                   // compiling method's class loader.
  uint16_t class_def_idx;                 // compiling method's defining class definition index.
  uint32_t method_idx;                    // compiling method's index into method_ids of DexFile.
  uint32_t access_flags;                  // compiling method's access flags.
  InvokeType invoke_type;                 // compiling method's invocation type.
  const char* shorty;                     // compiling method's shorty.
  uint32_t disable_opt;                   // opt_control_vector flags.
  uint32_t enable_debug;                  // debugControlVector flags.
  bool verbose;
  const InstructionSet instruction_set;
  const bool target64;

  // TODO: move memory management to mir_graph, or just switch to using standard containers.
  ArenaAllocator arena;
  ArenaStack arena_stack;  // Arenas for ScopedArenaAllocator.

  std::unique_ptr<MIRGraph> mir_graph;   // MIR container.
  std::unique_ptr<Mir2Lir> cg;           // Target-specific codegen.
  TimingLogger timings;
  bool print_pass;                 // Do we want to print a pass or not?
  const QuickCompiler* compiler_;
  /**
   * @brief Holds pass options for current pass being applied to compilation unit.
   * @details This is updated for every pass to contain the overridden pass options
   * that were specified by user. The pass itself will check this to see if the
   * default settings have been changed. The key is simply the option string without
   * the pass name.
   */
  SafeMap<const std::string, const OptionContent> overridden_pass_options;
};

}  // namespace art

#endif  // ART_COMPILER_DEX_COMPILER_IR_H_
