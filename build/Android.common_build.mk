#
# Copyright (C) 2011 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

ifndef ART_ANDROID_COMMON_BUILD_MK
ART_ANDROID_COMMON_BUILD_MK = true

include art/build/Android.common.mk
include art/build/Android.common_utils.mk

#
# Only non-debug targets are used to improve performance.
#
ART_BUILD_TARGET_NDEBUG := true
ART_BUILD_TARGET_DEBUG := false
ART_BUILD_HOST_NDEBUG := true
ART_BUILD_HOST_DEBUG := false

ifeq ($(ART_BUILD_TARGET_NDEBUG),false)
$(info Disabling ART_BUILD_TARGET_NDEBUG)
endif
ifeq ($(ART_BUILD_TARGET_DEBUG),false)
$(info Disabling ART_BUILD_TARGET_DEBUG)
endif
ifeq ($(ART_BUILD_HOST_NDEBUG),false)
$(info Disabling ART_BUILD_HOST_NDEBUG)
endif
ifeq ($(ART_BUILD_HOST_DEBUG),false)
$(info Disabling ART_BUILD_HOST_DEBUG)
endif

#
# Enable JIT
#
ART_JIT := true

#
# Enable ART Optimizing compiler
#
ART_USE_OPTIMIZING_COMPILER := true


#
# Used to change the default GC. Valid values are CMS, SS, GSS. The default is CMS.
#
ART_DEFAULT_GC_TYPE ?= CMS
art_default_gc_type_cflags := -DART_DEFAULT_GC_TYPE_IS_$(ART_DEFAULT_GC_TYPE)

ART_HOST_CFLAGS :=
ART_TARGET_CFLAGS :=

# Clang build support.

# Host.
ART_HOST_CLANG := false

# Clang on the target. Target builds use GCC by default.
ART_TARGET_CLANG := false
ART_TARGET_CLANG_arm := false
ART_TARGET_CLANG_arm64 :=

define set-target-local-clang-vars
    LOCAL_CLANG := $(ART_TARGET_CLANG)
    $(foreach arch,$(ART_TARGET_SUPPORTED_ARCH),
      ifneq ($$(ART_TARGET_CLANG_$(arch)),)
        LOCAL_CLANG_$(arch) := $$(ART_TARGET_CLANG_$(arch))
      endif)
endef


# GCC-only warnings.
art_gcc_cflags := -Wunused-but-set-parameter

# Clear local variables now their use has ended.
art_clang_cflags :=
art_gcc_cflags :=

ART_CPP_EXTENSION := .cc

ART_C_INCLUDES := \
  external/gtest/include \
  external/icu/icu4c/source/common \
  external/valgrind/include \
  external/valgrind \
  external/vixl/src \
  external/zlib \

# Base set of cflags used by all things ART.
art_cflags := \
  -fno-rtti \
  -std=gnu++11 \
  -ggdb3 \
  -Wall \
  -Werror \
  -Wextra \
  -Wstrict-aliasing \
  -fstrict-aliasing \
  -Wunreachable-code \
  -Wredundant-decls \
  -Wshadow \
  -Wunused \
  -fvisibility=protected \
  $(art_default_gc_type_cflags)

# Missing declarations: too many at the moment, as we use "extern" quite a bit.
#  -Wmissing-declarations \


ifdef ART_IMT_SIZE
  art_cflags += -DIMT_SIZE=$(ART_IMT_SIZE)
else
  # Default is 64
  art_cflags += -DIMT_SIZE=64
endif

ifeq ($(ART_USE_OPTIMIZING_COMPILER),true)
  art_cflags += -DART_USE_OPTIMIZING_COMPILER=1
endif

ifeq ($(ART_HEAP_POISONING),true)
  art_cflags += -DART_HEAP_POISONING=1
endif

ifeq ($(ART_USE_READ_BARRIER),true)
  art_cflags += -DART_USE_READ_BARRIER=1
endif

ifeq ($(ART_USE_TLAB),true)
  art_cflags += -DART_USE_TLAB=1
endif

# Cflags for non-debug ART and ART tools.
art_non_debug_cflags := \
  -O3

# Force non-debug cflags for ART and ART tools.
art_debug_cflags := $(art_non_debug_cflags)

art_host_non_debug_cflags := $(art_non_debug_cflags)
art_target_non_debug_cflags := $(art_non_debug_cflags)

ifeq ($(HOST_OS),linux)
  # Larger frame-size for host clang builds today
  ifneq ($(ART_COVERAGE),true)
    ifneq ($(NATIVE_COVERAGE),true)
      ifndef SANITIZE_HOST
        art_host_non_debug_cflags += -Wframe-larger-than=2700
      endif
      art_target_non_debug_cflags += -Wframe-larger-than=1728
    endif
  endif
endif

ifndef LIBART_IMG_HOST_BASE_ADDRESS
  $(error LIBART_IMG_HOST_BASE_ADDRESS unset)
endif
ART_HOST_CFLAGS += $(art_cflags) -DART_BASE_ADDRESS=$(LIBART_IMG_HOST_BASE_ADDRESS)
ART_HOST_CFLAGS += -DART_DEFAULT_INSTRUCTION_SET_FEATURES=default

ifndef LIBART_IMG_TARGET_BASE_ADDRESS
  $(error LIBART_IMG_TARGET_BASE_ADDRESS unset)
endif
ART_TARGET_CFLAGS += $(art_cflags) -DART_TARGET -DART_BASE_ADDRESS=$(LIBART_IMG_TARGET_BASE_ADDRESS)

ART_HOST_NON_DEBUG_CFLAGS := $(art_host_non_debug_cflags)
ART_TARGET_NON_DEBUG_CFLAGS := $(art_target_non_debug_cflags)
ART_HOST_DEBUG_CFLAGS := $(art_debug_cflags)
ART_TARGET_DEBUG_CFLAGS := $(art_debug_cflags)

ifndef LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA
  LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA=-0x1000000
endif
ifndef LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA
  LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA=0x1000000
endif
ART_HOST_CFLAGS += -DART_BASE_ADDRESS_MIN_DELTA=$(LIBART_IMG_HOST_MIN_BASE_ADDRESS_DELTA)
ART_HOST_CFLAGS += -DART_BASE_ADDRESS_MAX_DELTA=$(LIBART_IMG_HOST_MAX_BASE_ADDRESS_DELTA)

ifndef LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA
  LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA=-0x1000000
endif
ifndef LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA
  LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA=0x1000000
endif
ART_TARGET_CFLAGS += -DART_BASE_ADDRESS_MIN_DELTA=$(LIBART_IMG_TARGET_MIN_BASE_ADDRESS_DELTA)
ART_TARGET_CFLAGS += -DART_BASE_ADDRESS_MAX_DELTA=$(LIBART_IMG_TARGET_MAX_BASE_ADDRESS_DELTA)

# To use oprofile_android --callgraph, uncomment this and recompile with "mmm art -B -j16"
# ART_TARGET_CFLAGS += -fno-omit-frame-pointer -marm -mapcs

# Clear locals now they've served their purpose.
art_cflags :=
art_debug_cflags :=
art_non_debug_cflags :=
art_host_non_debug_cflags :=
art_target_non_debug_cflags :=
art_default_gc_type :=
art_default_gc_type_cflags :=

# GCC lacks libc++ assumed atomic operations, grab via libatomic.
ART_HOST_LDLIBS += -latomic

ART_TARGET_LDFLAGS :=

# $(1): ndebug_or_debug
define set-target-local-cflags-vars
  LOCAL_CFLAGS += $(ART_TARGET_CFLAGS)
  LOCAL_LDFLAGS += $(ART_TARGET_LDFLAGS)
  art_target_cflags_ndebug_or_debug := $(1)
  ifeq ($$(art_target_cflags_ndebug_or_debug),debug)
    LOCAL_CFLAGS += $(ART_TARGET_DEBUG_CFLAGS)
  else
    LOCAL_CFLAGS += $(ART_TARGET_NON_DEBUG_CFLAGS)
  endif

  # Clear locally used variables.
  art_target_cflags_ndebug_or_debug :=
endef

# Build host and target as non-debug.
ART_BUILD_TARGET := true
ART_BUILD_HOST := true
ART_BUILD_NDEBUG := true
ART_BUILD_DEBUG := false

endif # ART_ANDROID_COMMON_BUILD_MK
