override NAME    := RVVM
override DESC    := The RVVM Project
override URL     := https://github.com/LekKit/RVVM
override VERSION := v0.7-git
override SRCDIR  := src

override define LOGO
  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓
 ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒
 ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░
 ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██
 ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒
 ░ ▒▓ ░▒▓░   ░ █░     ░ █░  ░ ▒░   ░  ░
   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░
   ░░   ░      ░░       ░░  ░      ░
    ░           ░        ░         ░
               ░        ░
endef

ifneq (,$(filter linux,$(OS)))
# Enable Wayland on Linux by default
#USE_WAYLAND ?= 1
endif

ifneq (,$(filter linux %bsd sunos,$(OS)))
# Enable X11 on Linux, *BSD, Solaris by default
USE_X11 ?= 1
endif

ifneq (,$(filter windows,$(OS)))
USE_WIN32_GUI ?= 1
endif

ifneq (,$(filter haiku,$(OS)))
USE_HAIKU_GUI ?= 1
endif

ifneq (,$(filter darwin serenity,$(OS)))
# Enable SDL2 on Darwin, Serenity by default
USE_SDL ?= 2
endif

ifneq (,$(filter redox,$(OS)))
# Enable SDL1 and disable networking on Redox by default
USE_SDL ?= 1
USE_NET ?= 0
USE_LIB ?= 0
endif

ifneq (,$(filter emscripten,$(OS)))
# Enable SDL2 on Emscripten by default
USE_SDL ?= 2
endif

# CPU features
USE_RV32 ?= 1
USE_RV64 ?= 1
USE_FPU  ?= 1
USE_RVV  ?= 0

# Infrastructure
USE_SPINLOCK_DEBUG ?= 1
USE_JNI            ?= 1
USE_ISOLATION      ?= 1

# Acceleration/accessibility
USE_JIT     ?= 1
USE_GUI     ?= 1
USE_SDL     ?= 0
USE_NET     ?= 1
USE_GDBSTUB ?= 1

# Devices
USE_FDT  ?= 1
USE_PCI  ?= 1
USE_VFIO ?= 1

ifneq (,$(call var_use,USE_TAP_LINUX))
$(call log_warn,Linux TAP is deprecated in favor of USE_NET due to checksum issues)
endif

#
# Useflag handling
#

# Useflag conditional sources
override SRC_USE_WIN32_GUI := $(SRCDIR)/devices/win32window.c
override SRC_USE_HAIKU_GUI := $(SRCDIR)/devices/haiku_window.cpp
override SRC_USE_X11       := $(SRCDIR)/devices/x11window_xlib.c
override SRC_USE_SDL       := $(SRCDIR)/devices/sdl_window.c
override SRC_USE_WAYLAND   := $(SRCDIR)/devices/wayland_window.c
override SRC_USE_TAP_LINUX := $(SRCDIR)/devices/tap_linux.c
override SRC_USE_NET       := $(SRCDIR)/networking.c $(SRCDIR)/devices/tap_user.c
override SRC_USE_JIT       := $(SRCDIR)/rvjit/rvjit.c $(SRCDIR)/rvjit/rvjit_emit.c
override SRC_USE_JNI       := $(SRCDIR)/bindings/jni/rvvm_jni.c
override SRC_USE_RV32      := $(SRCDIR)/cpu/riscv32_interpreter.c
override SRC_USE_RV64      := $(SRCDIR)/cpu/riscv64_interpreter.c

# Useflag dependencies
override RVJIT_SUPPORTS_ARCH := $(if $(filter i386 x86_64 arm% riscv%,$(ARCH)),1)

override DEPS_USE_X11       := USE_GUI
override DEPS_USE_SDL       := USE_GUI
override DEPS_USE_WAYLAND   := USE_GUI
override DEPS_USE_WIN32_GUI := USE_GUI
override DEPS_USE_HAIKU_GUI := USE_GUI
override DEPS_USE_JNI       := USE_LIB USE_NET
override DEPS_USE_GDBSTUB   := USE_NET
override DEPS_USE_JIT       := RVJIT_SUPPORTS_ARCH

# Libraries
override LIBS_USE_SDL     := sdl$(firstword $(USE_SDL) 1)
override LIBS_USE_X11     := x11 xext
override LIBS_USE_WAYLAND := wayland-client xkbcommon

#
# Prepare targets
#

override BIN_TARGETS := rvvm
override LIB_TARGETS := rvvm # TODO: rvvm_libretro FTBFS

override bin_src_rvvm          := $(SRCDIR)/main.c
override lib_src_rvvm_libretro := $(SRCDIR)/bindings/libretro/libretro.c
override lib_src_rvvm          := $(filter-out $(bin_src_rvvm) $(lib_src_rvvm_libretro),$(call recursive_match,$(SRCDIR),*.c *.cpp *.cc *.cxx))

override bin_libs_rvvm := rvvm
override lib_libs_rvvm := $(if $(call var_use,USE_FULL_LINKING),$(LIBS_USE_SDL) $(LIBS_USE_X11) $(LIBS_USE_WAYLAND))

#
# Tests
#

override RVVM := $(call bin_target,rvvm)

override TEST_DATA_TAR_LINK := https://github.com/LekKit/riscv-tests/releases/download/rvvm-tests/riscv-tests.tar.gz
override TEST_DATA_TAR_FILE := $(lastword $(subst /,$(SPACE),$(TEST_DATA_TAR_LINK)))

override test_result = $(call println,$(TEXT)[$(if $(filter 0,$1),$(GREEN)PASS,$(RED)FAIL: $1)$(TEXT)] $2)$(if $(filter 0,$1),,fail)

test:
	$(if $(call paths_exist,$(BUILDDIR)/riscv-tests),,$(call shell_ex,cd $(BUILDDIR) && curl -LO $(TEST_DATA_TAR_LINK) && tar xzf $(TEST_DATA_TAR_FILE)))
ifneq (,$(call var_use,USE_RV32))
	$(call println,)
	$(call log_info,Running RISC-V Tests (riscv32))
	$(call println,)
	@$(if $(strip $(foreach test,$(call recursive_wildcard,$(BUILDDIR)/riscv-tests/rv32*),$(call test_result,$(lastword $(call shell_ex,$(RVVM) $(test) -nonet -nogui -rv32 $(NULL_STDERR))),$(test)))),exit 1)
endif
ifneq (,$(call var_use,USE_RV64))
	$(call println,)
	$(call log_info,Running RISC-V Tests (riscv32))
	$(call println,)
	@$(if $(strip $(foreach test,$(call recursive_wildcard,$(BUILDDIR)/riscv-tests/rv64*),$(call test_result,$(lastword $(call shell_ex,$(RVVM) $(test) -nonet -nogui -rv64 $(NULL_STDERR))),$(test)))),exit 1)
endif
	@:
