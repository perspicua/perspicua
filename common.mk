UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    CROSS_COMPILE ?= aarch64-elf-
    LLVM_PATH     := $(shell brew --prefix llvm 2>/dev/null)
    OBJCOPY       ?= $(LLVM_PATH)/bin/llvm-objcopy
    MTOOLS_PATH   := $(shell brew --prefix mtools 2>/dev/null)
    MFORMAT       ?= $(if $(MTOOLS_PATH),$(MTOOLS_PATH)/bin/mformat,mformat)
    MCOPY         ?= $(if $(MTOOLS_PATH),$(MTOOLS_PATH)/bin/mcopy,mcopy)
    MMD           ?= $(if $(MTOOLS_PATH),$(MTOOLS_PATH)/bin/mmd,mmd)
else
    CROSS_COMPILE ?= aarch64-linux-gnu-
    OBJCOPY       ?= llvm-objcopy
    MFORMAT       ?= mformat
    MCOPY         ?= mcopy
    MMD           ?= mmd
endif

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
AR      := $(CROSS_COMPILE)ar
OBJDUMP := $(CROSS_COMPILE)objdump
NM      := $(CROSS_COMPILE)nm
SIZE    := $(CROSS_COMPILE)size

ARCH_FLAGS := -mcpu=cortex-a72+nosimd -mgeneral-regs-only

COMMON_CFLAGS := -Wall -Wextra -ffreestanding -nostdlib $(ARCH_FLAGS) \
                 -std=gnu11 -MMD -MP \
                 -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
                 -mno-outline-atomics -g

ifeq ($(COLOR),0)
  COL_DEFAULT :=
  COL_GREEN   :=
  COL_YELLOW  :=
  COL_BLUE    :=
  COL_MAGENTA :=
  COL_CYAN    :=
else
  COL_DEFAULT := \x1b[0m
  COL_GREEN   := \x1b[32m
  COL_YELLOW  := \x1b[33m
  COL_BLUE    := \x1b[34m
  COL_MAGENTA := \x1b[35m
  COL_CYAN    := \x1b[36m
endif

# Verbosity Control
ifeq ($(V),1)
  Q :=
  print_cc  =
  print_as  =
  print_ld  =
  print_ar  =
  print_img =
  S         :=
else
  Q := @
  S := -s
  print_cc  = @printf "  $(COL_GREEN)CC$(COL_DEFAULT)      %s\n" "$(1)"
  print_as  = @printf "  $(COL_CYAN)AS$(COL_DEFAULT)      %s\n" "$(1)"
  print_ld  = @printf "  $(COL_YELLOW)LD$(COL_DEFAULT)      %s\n" "$(1)"
  print_ar  = @printf "  $(COL_MAGENTA)AR$(COL_DEFAULT)      %s\n" "$(1)"
  print_img = @printf "  $(COL_BLUE)IMG$(COL_DEFAULT)     %s\n" "$(1)"
endif
