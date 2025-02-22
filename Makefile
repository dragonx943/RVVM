# Makefile :)
override NAME    := rvvm
override SRCDIR  := src
override VERSION := 0.7

#
# Determine build host features
#

ifdef WINDIR
# Set by a Windows host, rule out Cygwin via uname
override HOST_UNAME := $(firstword $(shell uname -o 2>/dev/null) Windows)
ifeq ($(OS),Windows_NT)
# Clean up garbage OS env passed on Windows by default
override OS :=
endif
else
# Assume a POSIX host
override HOST_UNAME := $(firstword $(shell uname -s 2>/dev/null) POSIX)
endif

ifeq ($(HOST_UNAME),Windows)
override HOST_WINDOWS := 1
override NULL_STDERR := 2>nul
override HOST_CPUS := $(firstword $(NUMBER_OF_PROCESSORS) 1)
ifeq ($(PROCESSOR_ARCHITECTURE),AMD64)
override HOST_ARCH := x86_64
else
ifeq ($(PROCESSOR_ARCHITECTURE),ARM64)
override HOST_ARCH := arm64
else
override HOST_ARCH := i386
endif
endif

else
override HOST_POSIX := 1
override NULL_STDERR := 2>/dev/null
override HOST_CPUS := $(shell nproc $(NULL_STDERR) || sysctl -n hw.ncpu $(NULL_STDERR) || echo 1)
override HOST_ARCH := $(firstword $(shell uname -m 2>/dev/null) Unknown)
endif

#
# Some eye-candy stuff
#

override EMPTY :=
override SPACE := $(EMPTY) $(EMPTY)

override RESET   := $(shell tput me   $(NULL_STDERR) || tput sgr0 $(NULL_STDERR)    || printf "\\033[0m" $(NULL_STDERR))
override BOLD    := $(shell tput md   $(NULL_STDERR) || tput bold $(NULL_STDERR)    || printf "\\033[1m" $(NULL_STDERR))
override RED     := $(shell tput AF 1 $(NULL_STDERR) || tput setaf 1 $(NULL_STDERR) || printf "\\033[31m" $(NULL_STDERR))$(BOLD)
override GREEN   := $(shell tput AF 2 $(NULL_STDERR) || tput setaf 2 $(NULL_STDERR) || printf "\\033[32m" $(NULL_STDERR))$(BOLD)
override YELLOW  := $(shell tput AF 3 $(NULL_STDERR) || tput setaf 3 $(NULL_STDERR) || printf "\\033[33m" $(NULL_STDERR))$(BOLD)
override WHITE   := $(shell tput AF 7 $(NULL_STDERR) || tput setaf 7 $(NULL_STDERR) || printf "\\033[37m" $(NULL_STDERR))$(BOLD)
override TEXT    := $(RESET)$(BOLD)

$(info $(RESET))
ifneq (,$(findstring UTF, $(LANG)))
$(info $(EMPTY)  ██▀███   ██▒   █▓ ██▒   █▓ ███▄ ▄███▓)
$(info $(EMPTY) ▓██ ▒ ██▒▓██░   █▒▓██░   █▒▓██▒▀█▀ ██▒)
$(info $(EMPTY) ▓██ ░▄█ ▒ ▓██  █▒░ ▓██  █▒░▓██    ▓██░)
$(info $(EMPTY) ▒██▀▀█▄    ▒██ █░░  ▒██ █░░▒██    ▒██ )
$(info $(EMPTY) ░██▓ ▒██▒   ▒▀█░     ▒▀█░  ▒██▒   ░██▒)
$(info $(EMPTY) ░ ▒▓ ░▒▓░   ░ █░     ░ █░  ░ ▒░   ░  ░)
$(info $(EMPTY)   ░▒ ░ ▒░   ░ ░░     ░ ░░  ░  ░      ░)
$(info $(EMPTY)   ░░   ░      ░░       ░░  ░      ░   )
$(info $(EMPTY)    ░           ░        ░         ░   )
$(info $(EMPTY)               ░        ░              )
$(info $(EMPTY))
endif

# Message prefixes
override INFO_PREFIX := $(TEXT)[$(YELLOW)INFO$(TEXT)]
override WARN_PREFIX := $(TEXT)[$(RED)WARN$(TEXT)]

# Automatically parallelize build
JOBS ?= $(HOST_CPUS)
override MAKEFLAGS += -j $(JOBS)

#
# Determine build target features for cross-compilation
#

# Get compiler target triplet (arch-vendor-kernel-abi)
override CC_TRIPLET := $(firstword $(shell $(CC) $(CFLAGS) -print-multiarch $(NULL_STDERR))$(shell $(CC) $(CFLAGS) -dumpmachine $(NULL_STDERR)))
ifeq (,$(findstring -,$(CC_TRIPLET)))
override CC_TRIPLET :=
endif

# Try to detect target OS via target triplet
ifneq (,$(findstring linux,$(CC_TRIPLET)))
override OS := Linux
endif
ifneq (,$(findstring android,$(CC_TRIPLET)))
override OS := Android
endif
ifneq (,$(findstring mingw,$(CC_TRIPLET))$(findstring windows,$(CC_TRIPLET)))
override OS := Windows
endif
ifneq (,$(findstring darwin,$(CC_TRIPLET))$(findstring macos,$(CC_TRIPLET)))
override OS := Darwin
endif
ifneq (,$(findstring emscripten,$(CC_TRIPLET)))
override OS := Emscripten
endif
ifneq (,$(findstring haiku,$(CC_TRIPLET)))
override OS := Haiku
endif
ifneq (,$(findstring serenity,$(CC_TRIPLET)))
override OS := Serenity
endif
ifneq (,$(findstring redox,$(CC_TRIPLET)))
override OS := Redox
endif

# Assume target OS matches host if triplet didn't match any known cross toolchains
ifndef OS
override OS := $(HOST_UNAME)
ifneq ($(CC),cc)
$(info $(INFO_PREFIX) Assuming target OS=$(OS), set explicitly if cross-compiling$(RESET))
endif
endif

# Detect target arch
ifndef ARCH
ifneq (,$(findstring -,$(CC_TRIPLET)))
# Get target arch from target triplet
override ARCH := $(firstword $(subst -, ,$(CC_TRIPLET)))
else
# This may fail on some compilers, fallback to host arch then
override ARCH := $(HOST_ARCH)
ifneq ($(CC),cc)
$(info $(INFO_PREFIX) Assuming target ARCH=$(ARCH), set explicitly if cross-compiling$(RESET))
endif
endif
endif

# Lower-case the string
override tolower = $(subst A,a,$(subst B,b,$(subst C,c,$(subst D,d,$(subst E,e,$(subst F,f,$(subst G,g,$(subst H,h,$(subst I,i,$(subst J,j,$(subst K,k,$(subst L,l,$(subst M,m,$(subst N,n,$(subst O,o,$(subst P,p,$(subst Q,q,$(subst R,r,$(subst S,s,$(subst T,t,$(subst U,u,$(subst V,v,$(subst W,w,$(subst X,x,$(subst Y,y,$(subst Z,z,$1))))))))))))))))))))))))))

# Canonize architecture name
# amd64 -> x86_64
# aarch64 -> arm64
# x86 -> i386
# x86_64 -> i386 (If -m32 is used)
override canonize_arch = $(patsubst x86_64,$(if $(filter -m32,$(CFLAGS)),i386,x86_64),$(patsubst amd64,x86_64,$(patsubst aarch64,arm64,$(patsubst x64,x86_64,$(patsubst x86,i386,$1)))))

# Use canonic architecture naming
override ARCH := $(call canonize_arch,$(ARCH))
override HOST_ARCH := $(call canonize_arch,$(HOST_ARCH))

# Use lower-case OS name in release directory name
override OS_PRETTY := $(OS)
override OS := $(call tolower,$(OS))

# For cross-compile checking
override TARGET_CROSS := $(if $(filter-out $(ARCH),$(HOST_ARCH))$(filter-out $(OS_PRETTY),$(HOST_UNAME)),$(ARCH)-$(OS))

# If PKG_CONFIG is not set, default to pkg-config for properly set up cross-compilation or native build
ifneq (,$(if $(PKG_CONFIG),,$(if $(TARGET_CROSS),$(PKG_CONFIG_LIBDIR)$(PKG_CONFIG_SYSROOT_DIR),yes)))
override PKG_CONFIG := pkg-config
endif

# Pass -static to pkg-config if needed
ifneq (,$(PKG_CONFIG))
override PKG_CONFIG := $(PKG_CONFIG) $(filter -static,$(LDFLAGS))
endif

ifeq (0,$(USE_FULL_LINKING))
override HAS_PKG_CONFIG := 1
else
override HAS_PKG_CONFIG := $(if $(PKG_CONFIG),$(if $(shell $(PKG_CONFIG) --version $(NULL_STDERR)),1,0),0)
endif

#
# Set up target-specific build options
#

# Windows-specific build options
ifeq ($(OS),windows)
override LDFLAGS += -static
override BIN_EXT := .exe
override LIB_EXT := .dll
USE_WIN32_GUI ?= 1
else

# Emscripten-specific build options
ifeq ($(OS),emscripten)
override CFLAGS += -pthread
override LDFLAGS += -s TOTAL_MEMORY=512MB
override BIN_EXT := .html
override LIB_EXT := .so
USE_SDL ?= 2
USE_NET ?= 0
USE_LIB ?= 0
else

# POSIX build options
override BIN_EXT :=
ifeq ($(OS),darwin)
override LIB_EXT := .dylib
else
override LIB_EXT := .so
endif

# Check for lib presence before linking (there is no pthread on Android, etc)
ifneq (,$(findstring main, $(shell $(CC) -pthread $(CFLAGS) $(LDFLAGS) -lpthread 2>&1)))
override CFLAGS += -pthread
endif
ifneq (,$(findstring main, $(shell $(CC) $(CFLAGS) $(LDFLAGS) -lpthread 2>&1)))
override LDFLAGS += -lpthread
endif
ifneq (,$(findstring main, $(shell $(CC) $(CFLAGS) $(LDFLAGS) -lrt 2>&1)))
override LDFLAGS += -lrt
endif
ifneq (,$(findstring main, $(shell $(CC) $(CFLAGS) $(LDFLAGS) -ldl 2>&1)))
override LDFLAGS += -ldl
endif

# Linking to libatomic on x86_64 is redundant and may cause issues
ifeq (,$(findstring -,$(CC_TRIPLET))$(filter-out x86_64,$(ARCH)))
ifneq (,$(findstring main, $(shell $(CC) $(CFLAGS) $(LDFLAGS) -latomic 2>&1)))
override LDFLAGS += -latomic
else
override CFLAGS += -DNO_LIBATOMIC
endif
endif

# Set some addiional options based on POSIX flavor

ifneq (,$(findstring linux,$(OS))$(findstring bsd,$(OS))$(findstring sunos,$(OS)))
# Enable X11 on Linux, *BSD, SunOS (Solaris) by default
USE_X11 ?= 1
endif

ifeq ($(OS),haiku)
USE_HAIKU_GUI ?= 1
endif

ifneq (,$(findstring darwin,$(OS))$(findstring serenity,$(OS)))
# Enable SDL2 on Darwin, Serenity by default
USE_SDL ?= 2
endif

ifneq (,$(findstring redox,$(OS)))
# Enable SDL1 and disable networking on Redox by default
USE_SDL ?= 1
USE_NET ?= 0
endif

endif
endif

#
# Default build configuration
#

# CPU features
USE_RV32 ?= 1
USE_RV64 ?= 1
USE_FPU ?= 1
USE_RVV ?= 0

# Infrastructure
USE_DEBUG ?= 0
USE_DEBUG_FULL ?= 0
USE_SPINLOCK_DEBUG ?= 1
USE_LIB ?= 1
USE_LIB_STATIC ?= 0
USE_JNI ?= 1
USE_ISOLATION ?= 1

# Acceleration/accessibility
USE_JIT ?= 1
USE_GUI ?= 1
USE_SDL ?= 0
USE_NET ?= 1
USE_GDBSTUB ?= 1

# Devices
USE_FDT ?= 1
USE_PCI ?= 1
USE_VFIO ?= 1

override BUILD_TYPE := release
ifeq ($(USE_DEBUG_FULL),1)
# Full debug with much less optimizations
override BUILD_TYPE := debug
endif

# Build output directory
BUILDDIR ?= $(BUILD_TYPE).$(OS).$(ARCH)

# Executable file name
BINARY ?= $(NAME)_$(ARCH)$(BIN_EXT)

# Determine build commit id
GIT_COMMIT ?= $(firstword $(shell git describe --match=NeVeRmAtCh_TaG --always --dirty $(NULL_STDERR)))
ifneq (,$(GIT_COMMIT))
override VERSION := $(VERSION)-$(GIT_COMMIT)
endif

#
# Set up sources, libs, CFLAGS & LDFLAGS
#

# Generic compiler flags
override CFLAGS := -I$(SRCDIR) -DRVVM_VERSION=\"$(VERSION)\" $(CFLAGS)

# Select sources to compile
override SRC := $(wildcard $(SRCDIR)/*.c $(SRCDIR)/devices/*.c)

# Useflag sources
override SRC_USE_WIN32_GUI := $(SRCDIR)/devices/win32window.c
override SRC_CXX_USE_HAIKU_GUI := $(SRCDIR)/devices/haiku_window.cpp
override SRC_USE_X11 := $(SRCDIR)/devices/x11window_xlib.c
override SRC_USE_SDL := $(SRCDIR)/devices/sdl_window.c
override SRC_USE_WAYLAND := $(SRCDIR)/devices/wayland_window.c

override SRC_USE_TAP_LINUX := $(SRCDIR)/devices/tap_linux.c
override SRC_USE_NET := $(SRCDIR)/networking.c $(SRCDIR)/devices/tap_user.c
override SRC_USE_JIT := $(SRCDIR)/rvjit/rvjit.c $(SRCDIR)/rvjit/rvjit_emit.c
override SRC_USE_JNI := $(SRCDIR)/bindings/jni/rvvm_jni.c
override SRC_USE_RV32 := $(SRCDIR)/cpu/riscv32_interpreter.c
override SRC_USE_RV64 := $(SRCDIR)/cpu/riscv64_interpreter.c

# Useflag CFLAGS
override CFLAGS_USE_DEBUG := -DDEBUG -g -fno-omit-frame-pointer
override CFLAGS_USE_DEBUG_FULL := -DUSE_DEBUG -DDEBUG -O0 -ggdb -fno-omit-frame-pointer
override CFLAGS_USE_LIB := -fPIC

# Useflag LDFLAGS
# Needed for floating-point functions like fetestexcept/feraiseexcept
override LDFLAGS_USE_FPU := -lm

# Useflag LIBS
override LIBS_USE_SDL := sdl$(subst 1,,$(USE_SDL))
override LIBS_USE_X11 := x11 xext
override LIBS_USE_WAYLAND := wayland-client xkbcommon

# Useflag dependencies
override NEED_USE_X11 := USE_GUI HAS_PKG_CONFIG
override NEED_USE_SDL := USE_GUI HAS_PKG_CONFIG
override NEED_USE_WAYLAND := USE_GUI HAS_PKG_CONFIG
override NEED_USE_WIN32_GUI := USE_GUI
override NEED_USE_HAIKU_GUI := USE_GUI
override NEED_USE_JNI := USE_LIB
override NEED_USE_GDBSTUB := USE_NET

#
# Target-specific useflags handling
#

ifeq ($(OS),windows)
# Windows-specific libraries to link to
ifneq (,$(findstring main, $(subst WinMain,main,$(shell $(CC) $(CFLAGS) $(LDFLAGS) -lgdi32 2>&1))))
# On WinCE it's not expected to link gdi32
override LDFLAGS_USE_WIN32_GUI := -lgdi32
endif
ifneq (,$(findstring main, $(subst WinMain,main,$(shell $(CC) $(CFLAGS) $(LDFLAGS) -lws2 2>&1))))
# On WinCE there is no _32 suffix
override LDFLAGS_USE_NET := -lws2
else
override LDFLAGS_USE_NET := -lws2_32
endif
endif

ifeq ($(OS),haiku)
# Haiku-specific libraries to link to
override LDFLAGS_USE_HAIKU_GUI := -lbe
override LDFLAGS_USE_NET := -lnetwork
endif

ifeq ($(OS),sunos)
# Solaris-specific libraries to link to
override LDFLAGS_USE_NET := -lsocket
endif

ifeq ($(OS),emscripten)
# Request SDL port to be enabled in Emscripten
override CFLAGS_USE_SDL := -s USE_SDL=$(USE_SDL)
endif

# Check if RVJIT supports the target architecture
ifeq ($(USE_JIT),1)
ifeq (,$(findstring 86,$(ARCH))$(findstring arm,$(ARCH))$(findstring riscv,$(ARCH)))
override USE_JIT := 0
$(info $(INFO_PREFIX) No RVJIT support for current target$(RESET))
endif
endif

ifeq ($(USE_TAP_LINUX),1)
$(info $(WARN_PREFIX) Linux TAP is deprecated in favor of USE_NET due to checksum issues)
endif

#
# Useflag automation magic
#

override USEFLAGS := $(sort $(filter USE_%,$(.VARIABLES)))

# Filter out all conditionally compiled C/C++ sources
override SRC_CONDITIONAL := $(filter SRC_USE_%,$(.VARIABLES))
override SRC := $(filter-out $(foreach cond_src,$(SRC_CONDITIONAL),$($(cond_src))),$(SRC))
override SRC_CXX := $(filter-out $(foreach cond_src,$(SRC_CONDITIONAL),$($(cond_src))),$(SRC_CXX))

# Disable all useflags which depend on another disabled useflags
override _ := $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(foreach need_useflag,$(NEED_$(useflag)),\
$(if $(filter 0,$($(need_useflag))),$(eval override $(useflag) := 0)$(info $(WARN_PREFIX) $(useflag) depends on $(need_useflag)$(RESET))))))

# Include actually enabled C/C++ sources
override SRC := $(SRC) $(sort $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(SRC_$(useflag)))))
override SRC_CXX := $(SRC_CXX) $(strip $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(SRC_CXX_$(useflag)))))


# Handle library include paths / linking when pkg-config is available
ifneq (,$(PKG_CONFIG))
override LIBS := $(strip $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(LIBS_$(useflag)))))
override _ := $(foreach lib, $(LIBS),$(if $(shell $(PKG_CONFIG) $(lib) --cflags --libs $(NULL_STDERR)),,$(info $(WARN_PREFIX) Possibly missing library: $(lib)$(RESET))))

# Set libraries include paths
override CFLAGS := $(CFLAGS) $(sort $(foreach lib, $(LIBS),$(shell $(PKG_CONFIG) $(lib) --cflags-only-I $(NULL_STDERR))))

ifeq ($(USE_FULL_LINKING),1)
# Proper library linking (Do not use sort here for correct linking order)
override LDFLAGS := $(LDFLAGS) $(strip $(foreach lib, $(LIBS),$(shell $(PKG_CONFIG) $(lib) --libs $(NULL_STDERR))))
else
# Pass libdir rpath to linker for dynamic loader to work on Nix, etc
ifneq (,$(findstring linux,$(OS))$(findstring darwin,$(OS)))
override WL_RPATH := -Wl,-rpath,
override LIBDIRS := $(sort $(foreach lib, $(LIBS),$(shell $(PKG_CONFIG) $(lib) --variable libdir $(NULL_STDERR))))
override LDFLAGS := $(LDFLAGS) $(sort $(foreach libdir, $(LIBDIRS),$(WL_RPATH)$(libdir)))
endif
endif
endif


# Set useflags CFLAGS
override CFLAGS := $(CFLAGS) $(strip $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(CFLAGS_$(useflag)))))

# Set useflags LDFLAGS
override LDFLAGS := $(LDFLAGS) $(strip $(foreach useflag,$(USEFLAGS),$(if $(filter-out 0,$($(useflag))),$(LDFLAGS_$(useflag)))))

# Set useflags C definitions
override CFLAGS := $(CFLAGS) $(strip $(foreach useflag, $(USEFLAGS),$(if $(filter-out 0,$($(useflag))),-D$(useflag)=$($(useflag)))))

#
# Output & Object files handling
#

# Output directories / files
override OBJDIR := $(BUILDDIR)/obj
override BINARY := $(BUILDDIR)/$(BINARY)
override SHARED := $(BUILDDIR)/lib$(NAME)$(LIB_EXT)
override STATIC := $(BUILDDIR)/lib$(NAME)_static.a

# Combine the object files
override OBJS := $(SRC:$(SRCDIR)/%.c=$(OBJDIR)/%.o) $(SRC_CXX:$(SRCDIR)/%.cpp=$(OBJDIR)/%.o)
override LIB_OBJS := $(filter-out main.o,$(OBJS))
override DEPS := $(OBJS:.o=.d)
override DIRS := $(sort $(BUILDDIR) $(OBJDIR) $(dir $(OBJS)))

# Create directories for object files
ifeq ($(HOST_POSIX),1)
override _ := $(shell mkdir -p $(DIRS))
else
override _ := $(foreach directory,$(DIRS),$(shell mkdir $(subst /,\\,$(directory)) $(NULL_STDERR))$(shell mkdir $(directory) $(NULL_STDERR)))
endif

#
# Detect compiler brand & features, set up optimization/warning options
#

override CC_INFO := $(shell $(CC) -v 2>&1)
override CC_INFO_TMP := $(CC_INFO)
override CC_FULL_VERSION := $(strip $(foreach cc_word,$(CC_INFO),$(if $(filter version,$(word 2,$(CC_INFO_TMP))),$(wordlist 1,3,$(CC_INFO_TMP)))\
$(eval override CC_INFO_TMP := $(wordlist 2,$(words $(CC_INFO_TMP)),$(CC_INFO_TMP)))))
override CC_BRAND := $(firstword $(CC_FULL_VERSION))
override CC_VERSION := $(word 3,$(CC_FULL_VERSION))
ifeq (,$(findstring .,$(CC_VERSION)))
override CC_VERSION := $(shell $(CC) -dumpfullversion -dumpversion $(NULL_STDERR))
endif
ifeq ($(ARCH),e2k)
# It's not a real GCC, workaround issues by explicitly marking it as different compiler brand
override CC_BRAND := ПТН ПНХ
endif
ifeq (,$(CC_BRAND))
override CC_BRAND := Unknown
endif

# Compiler version checks
override CC_AT_LEAST_2_0 := $(filter-out 1.%,$(CC_VERSION))
override CC_AT_LEAST_3_0 := $(filter-out 2.%,$(CC_AT_LEAST_2_0))
override CC_AT_LEAST_4_0 := $(filter-out 3.%,$(CC_AT_LEAST_3_0))
override CC_AT_LEAST_5_0 := $(filter-out 4.%,$(CC_AT_LEAST_4_0))
override CC_AT_LEAST_6_0 := $(filter-out 5.%,$(CC_AT_LEAST_5_0))
override CC_AT_LEAST_7_0 := $(filter-out 6.%,$(CC_AT_LEAST_6_0))

# Check LTO support
override LTO_CHECK_OUT := $(OBJDIR)/lto_lest$(BIN_EXT)
override LTO_SUPPORTED := $(wildcard $(LTO_CHECK_OUT))
ifeq (,$(LTO_SUPPORTED))
override LTO_ERROR := $(shell echo "int main(){return 0;}" | $(CC) -flto -xc -o $(LTO_CHECK_OUT) - 2>&1)
ifeq (,$(findstring lto,$(LTO_ERROR))$(findstring LTO,$(LTO_ERROR)))
override LTO_SUPPORTED := 1
else
$(info $(INFO_PREFIX) LTO is not supported by this toolchain: $(wordlist 2,8,$(LTO_ERROR))$(RESET))
endif
endif

override CC_STD := -std=c99
override CXX_STD :=

# Warning options (Strict safety/portability, stack/object size limits)
# Need at least GCC 7.0 or Clang 7.0
# -Wbad-function-cast, -Wcast-align, need fixes in codebase
ifneq (,$(CC_AT_LEAST_7_0))
override WARN_OPTS := -Wall -Wextra -Wshadow -Wvla -Wpointer-arith -Walloca -Wduplicated-cond \
-Wtrampolines -Wlarger-than=1048576 -Wframe-larger-than=32768 -Wdouble-promotion -Werror=return-type
else
# Conservative warning options for older compilers
override WARN_OPTS := -Wall -Wextra
endif

# Set up optimization options based on the compiler brand
ifneq (,$(findstring clang,$(CC_INFO))$(findstring LLVM,$(CC_INFO)))
# LLVM Clang or derivatives (Zig CC, Emscripten)
override CC_PRETTY := LLVM Clang $(CC_VERSION)

override CC_STD := -std=gnu99
ifneq (,$(CC_AT_LEAST_4_0))
override CC_STD := -std=gnu11 -Wstrict-prototypes -Wold-style-definition
override CXX_STD := -std=gnu++11
endif

override CFLAGS := -O2 $(if $(LTO_SUPPORTED),-flto) $(if $(CC_AT_LEAST_4_0),-frounding-math) -fvisibility=hidden -fno-math-errno \
$(WARN_OPTS) -Wno-unknown-warning-option -Wno-unsupported-floating-point-opt -Wno-ignored-optimization-argument \
-Wno-missing-braces -Wno-missing-field-initializers -Wno-ignored-pragmas -Wno-atomic-alignment $(CFLAGS)

else
ifeq ($(CC_BRAND),gcc)
# GNU GCC or derivatives (MinGW)
override CC_PRETTY := GCC $(CC_VERSION)

override CC_STD := -std=gnu99
ifneq (,$(CC_AT_LEAST_5_0))
override CC_STD := -std=gnu11 -Wstrict-prototypes -Wold-style-declaration -Wold-style-definition
override CXX_STD := -std=gnu++11
endif

override CFLAGS := -O2 $(if $(LTO_SUPPORTED),-flto=auto) -frounding-math $(if $(CC_AT_LEAST_4_0),-fvisibility=hidden -fno-math-errno) $(if $(CC_AT_LEAST_6_0),-fno-plt) \
$(WARN_OPTS) -Wno-missing-braces $(if $(CC_AT_LEAST_4_0),-Wno-missing-field-initializers) $(CFLAGS)

else
# Toy compiler (TCC, Chibicc, Cproc)
override CC_PRETTY := $(RED)$(CC_BRAND) $(CC_VERSION)

endif
endif

#
# Check previous build flags, force a rebuild if necessary
#

override CFLAGS_TXT := $(OBJDIR)/cflags.txt
override LDFLAGS_TXT := $(OBJDIR)/ldflags.txt
override CURR_CFLAGS := $(CC) $(CC_VERSION) $(CFLAGS)
override CURR_LDFLAGS := $(LD) $(CC_VERSION) $(LDFLAGS)
sinclude $(CFLAGS_TXT) $(LDFLAGS_TXT)

ifneq ($(CURR_CFLAGS),$(PREV_CFLAGS))
ifneq (,$(PREV_CFLAGS))
$(info $(INFO_PREFIX) CFLAGS changed, doing a full rebuild$(RESET))
endif
override MAKEFLAGS += -B
else
ifneq ($(CURR_LDFLAGS),$(PREV_LDFLAGS))
$(info $(INFO_PREFIX) LDFLAGS changed, relinking binaries$(RESET))
override _ := $(shell rm $(BINARY) $(SHARED) $(NULL_STDERR))
endif
endif

ifneq (,$(filter-out 3.%,$(MAKE_VERSION)))
override _ := $(file >$(CFLAGS_TXT),PREV_CFLAGS := $(CURR_CFLAGS))
override _ := $(file >$(LDFLAGS_TXT),PREV_LDFLAGS := $(CURR_LDFLAGS))
else
override _ := $(shell echo "PREV_CFLAGS := $(subst ",\\",$(CURR_CFLAGS))" > $(CFLAGS_TXT))
override _ := $(shell echo "PREV_LDFLAGS := $(subst ",\\",$(CURR_LDFLAGS))" > $(LDFLAGS_TXT))
endif

#
# Compiler invocation helpers
#

override DO_CC = $(CC) $(CC_STD) $(CFLAGS) -MMD -MF $(patsubst %.o, %.d, $@) -o $@ -c $<
override DO_CXX = $(CXX) $(CXX_STD) $(CFLAGS) -MMD -MF $(patsubst %.o, %.d, $@) -o $@ -c $<

# Link using CC or CXX if any C++ code is present
override CC_LD := $(CC)
ifneq (,$(strip $(SRC_CXX)))
override CC_LD := $(CXX)
endif

#
# Print build information
#

$(info $(TEXT)Detected OS: $(GREEN)$(OS_PRETTY)$(RESET))
$(info $(TEXT)Detected CC: $(GREEN)$(CC_PRETTY)$(RESET))
$(info $(TEXT)Target arch: $(GREEN)$(ARCH)$(RESET))
$(info $(TEXT)Version:     $(GREEN)RVVM $(VERSION)$(RESET))
$(info $(EMPTY))

#
# Make targets
#

.PHONY: all         # Build everything (Default)
all: bin lib

.PHONY: bin         # Build the main executable
bin: $(BINARY)

.PHONY: lib         # Build shared / static libraries based on useflags
lib: $(if $(findstring 1,$(USE_LIB)),$(SHARED)) $(if $(findstring 1,$(USE_LIB_STATIC)),$(STATIC))

# Ignore deleted header files
%.h:
	@:

# C object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c Makefile
	$(info $(TEXT)[$(YELLOW)CC$(TEXT)] $< $(RESET))
	@$(DO_CC)

# C++ object files
$(OBJDIR)/%.o: $(SRCDIR)/%.cpp Makefile
	$(info $(TEXT)[$(YELLOW)CXX$(TEXT)] $< $(RESET))
	@$(DO_CXX)

# Main binary
$(BINARY): $(OBJS)
	$(info $(TEXT)[$(GREEN)LD$(TEXT)] $@ $(RESET))
	@$(CC_LD) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@

# Shared library
$(SHARED): $(LIB_OBJS)
	$(info $(TEXT)[$(GREEN)LD$(TEXT)] $@ $(RESET))
	@$(CC_LD) $(CFLAGS) $(LIB_OBJS) $(LDFLAGS) -shared -o $@

# Static library
$(STATIC): $(LIB_OBJS)
	$(info $(TEXT)[$(GREEN)AR$(TEXT)] $@ $(RESET))
	@$(AR) -rcs $@ $(LIB_OBJS)

.PHONY: test        # Run RISC-V tests
test: bin
	$(if $(wildcard $(BUILDDIR)/riscv-tests.tar.gz),,@cd "$(BUILDDIR)"; curl -LO "https://github.com/LekKit/riscv-tests/releases/download/rvvm-tests/riscv-tests.tar.gz")
	@tar xzf "$(BUILDDIR)/riscv-tests.tar.gz" -C $(BUILDDIR)
ifeq ($(USE_RV32),1)
	@echo
	@echo "$(INFO_PREFIX) Running RISC-V Tests (riscv32)$(RESET)"
	@echo
	@for file in "$(BUILDDIR)/riscv-tests/rv32"*; do \
		result=$$($(BINARY) $$file -nonet -nogui -rv32 | tr -d '\0'); \
		result="$${result##* }"; \
		if [ "$$result" -eq "0" ]; then \
		echo "$(TEXT)[$(GREEN)PASS$(TEXT)] $$file$(RESET)"; \
		else \
		echo "$(TEXT)[$(RED)FAIL: $$result$(TEXT)] $$file$(RESET)"; \
		exit -1; \
		fi; \
	done
endif
ifeq ($(USE_RV64),1)
	@echo
	@echo "$(INFO_PREFIX) Running RISC-V Tests (riscv64)$(RESET)"
	@echo
	@for file in "$(BUILDDIR)/riscv-tests/rv64"*; do \
		result=$$($(BINARY) $$file -nonet -nogui -rv64 | tr -d '\0'); \
		result="$${result##* }"; \
		if [ "$$result" -eq "0" ]; then \
		echo "$(TEXT)[$(GREEN)PASS$(TEXT)] $$file$(RESET)"; \
		else \
		echo "$(TEXT)[$(RED)FAIL: $$result$(TEXT)] $$file$(RESET)"; \
		exit -1; \
		fi; \
	done
endif

override CPPCHECK_GENERIC_OPTIONS := -f -j$(JOBS) --inline-suppr --std=c99 -q -I $(SRCDIR)
override CPPCHECK_SUPPRESS_OPTIONS :=  --suppress=unmatchedSuppression --suppress=missingIncludeSystem \
--suppress=constParameterPointer --suppress=constVariablePointer --suppress=constParameterCallback \
--suppress=constVariable --suppress=variableScope --suppress=knownConditionTrueFalse \
--suppress=unusedStructMember --suppress=uselessAssignmentArg --suppress=unreadVariable --suppress=syntaxError
ifneq ($(CPPCHECK_FAST),1)
override CPPCHECK_GENERIC_OPTIONS += --check-level=exhaustive
else
override CPPCHECK_SUPPRESS_OPTIONS += --suppress=normalCheckLevelMaxBranches
endif

.PHONY: cppcheck    # Run cppcheck static analysis
cppcheck:
	$(info $(INFO_PREFIX) Running Cppcheck analysis$(RESET))
ifeq ($(CPPCHECK_ALL),1)
	@cppcheck $(CPPCHECK_GENERIC_OPTIONS) $(CPPCHECK_SUPPRESS_OPTIONS) --enable=all --inconclusive $(SRCDIR)
else
	@cppcheck $(CPPCHECK_GENERIC_OPTIONS) $(CPPCHECK_SUPPRESS_OPTIONS) --enable=warning,performance,portability $(SRCDIR)
endif

.PHONY: clean       # Clean the build directory
clean:
	$(info $(INFO_PREFIX) Cleaning up$(RESET))
ifeq ($(HOST_POSIX),1)
	@-rm -f $(BINARY) $(SHARED)
	@-rm -r $(OBJDIR)
else
	@-rm -f $(BINARY) $(SHARED) $(NULL_STDERR) ||:
	@-rm -r $(OBJDIR) $(NULL_STDERR) ||:
	@-del $(subst /,\\, $(BINARY) $(SHARED)) $(NULL_STDERR) ||:
	@-rmdir /S /Q $(subst /,\\, $(OBJDIR)) $(NULL_STDERR) ||:
endif

# System-wide install
DESTDIR ?=
PREFIX  ?= /usr

# Handle all the weird GNU-style installation variables
prefix      ?= $(PREFIX)
exec_prefix ?= $(prefix)
bindir      ?= $(exec_prefix)/bin
libdir      ?= $(exec_prefix)/lib
includedir  ?= $(prefix)/include
datarootdir ?= $(prefix)/share
datadir     ?= $(datarootdir)

define GEN_PKGCONFIG
prefix=$(prefix)\n\
exec_prefix=$(exec_prefix)\n\
libdir=$(libdir)\n\
includedir=$(includedir)\n\
\n\
Name: rvvm\n\
Description: The RISC-V Virtual Machine Library\n\
URL: https://github.com/LekKit/RVVM\n\
Version: $(VERSION)\n\
Requires.private: $(LIBS)\n\
Libs: -L$(libdir) -lrvvm\n\
Cflags: -I$(includedir) -DLIBRVVM_SHARED\n
endef

override GEN_PKGCONFIG := $(subst \n ,\n,$(GEN_PKGCONFIG))

.PHONY: install     # Install the package
install: all
ifeq ($(HOST_POSIX),1)
	$(info $(INFO_PREFIX) Installing to prefix $(DESTDIR)$(prefix)$(RESET))
	@install -d                            $(DESTDIR)$(bindir)
	@install -m 0755 $(BINARY)             $(DESTDIR)$(bindir)/rvvm
ifneq (,$(findstring 1,$(USE_LIB)$(USE_LIB_STATIC)))
	@install -d                            $(DESTDIR)$(libdir)
	@install -d                            $(DESTDIR)$(libdir)/pkgconfig
	@printf "$(GEN_PKGCONFIG)" >           $(DESTDIR)$(libdir)/pkgconfig/rvvm.pc
	@install -d                            $(DESTDIR)$(includedir)/rvvm/
	@install -m 0644 $(SRCDIR)/rvvmlib.h   $(DESTDIR)$(includedir)/rvvm/rvvmlib.h
	@install -m 0644 $(SRCDIR)/fdtlib.h    $(DESTDIR)$(includedir)/rvvm/fdtlib.h
	@install -m 0644 $(SRCDIR)/devices/*.h $(DESTDIR)$(includedir)/rvvm/
endif
ifeq ($(USE_LIB),1)
	@install -m 0755 $(SHARED)             $(DESTDIR)$(libdir)/librvvm$(LIB_EXT)
endif
ifeq ($(USE_LIB_STATIC),1)
	@install -m644 $(STATIC)               $(DESTDIR)$(libdir)/librvvm_static.a
endif
	@install -d                            $(DESTDIR)$(datadir)/licenses/rvvm/
	@install -m 0644 LICENSE*              $(DESTDIR)$(datadir)/licenses/rvvm/
else
	$(info $(WARN_PREFIX) Install target unsupported on non-POSIX!$(RESET))
endif

.PHONY: help        # Show this help message
help:
	$(info $(INFO_PREFIX) Available make useflags:$(RESET))
	$(foreach useflag, $(USEFLAGS),$(info $(EMPTY) $(useflag)=$($(useflag))))
	$(info $(INFO_PREFIX) Available make targets:$(RESET))
	@grep '^.PHONY:' Makefile | sed 's/\.PHONY://g'
	@echo $(NULL_STDERR)

.PHONY: info        # Show this help message
info: help

.PHONY: list        # Show this help message
list: help

sinclude $(DEPS)
