CMAKE_FLAGS_LLVM =

-include Makefile.local

MAKEFILE_DIR = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

BUILD_TYPE ?= Release # One of (Debug, Release)
JOBS       ?= 1

BUILDDIR   ?= $(MAKEFILE_DIR)build
INSTALLDIR ?= $(MAKEFILE_DIR)install

CMAKE_GENERATOR ?= Unix Makefiles # One of (Unix Makefiles, Ninja)

#############################################################################

MKDIR = mkdir -p
CMAKE = cmake

#############################################################################

LLVM_REPO = git@github.com:llvm/llvm-project.git
LLVM_FORK = git@gitlab.kuleuven.be:u0126303/llvm-project.git

SRCDIR_LLVM   = $(MAKEFILE_DIR)llvm

BUILDDIR_LLVM = $(BUILDDIR)/llvm

CMAKE_FLAGS_LLVM += -G "$(strip $(CMAKE_GENERATOR))"
ifeq ($(CMAKE_GENERATOR), Ninja)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_COMPILE_JOBS=$(JOBS)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_LINK_JOBS=2
endif
CMAKE_FLAGS_LLVM += -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
CMAKE_FLAGS_LLVM += -DCMAKE_INSTALL_PREFIX=$(INSTALLDIR)
CMAKE_FLAGS_LLVM += -DLLVM_TARGETS_TO_BUILD="RISCV"
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PROJECTS="clang;lld"
CMAKE_FLAGS_LLVM += -DLLVM_USE_LINKER=gold
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PLUGINS=ON
CMAKE_FLAGS_LLVM += -DLLVM_USE_NEWPM=OFF

#CMAKE_FLAGS_LLVM += -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_ASSERTIONS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_CCACHE_BUILD=ON
#For DEBUG builds, use shared libs, unless you have a lot of memory
#CMAKE_FLAGS_LLVM += -DBUILD_SHARED_LIBS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_BUILD_LLVM_DYLIB=ON

#############################################################################

.PHONY: all
all:
	@echo BUILD_TYPE=$(BUILD_TYPE)
	@echo JOBS=$(JOBS)
	@echo CMAKE_GENERATOR=$(CMAKE_GENERATOR)

.PHONY: configure
configure:
	$(MKDIR) $(BUILDDIR_LLVM)
	cd $(BUILDDIR_LLVM) && $(CMAKE) $(CMAKE_FLAGS_LLVM) $(SRCDIR_LLVM)

.PHONY: build
build:
ifneq ($(CMAKE_GENERATOR), Ninja)
	$(CMAKE) --build $(BUILDDIR_LLVM) -- -j$(JOBS)
else
	$(CMAKE) --build $(BUILDDIR_LLVM)
endif

.PHONY: install
install: build
	$(CMAKE) --build $(BUILDDIR_LLVM) --target install
