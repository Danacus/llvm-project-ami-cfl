CMAKE_FLAGS_LLVM =

-include Makefile.local

MAKEFILE_DIR = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

BUILD_TYPE ?= Debug
# BUILD_TYPE ?= Release
JOBS       ?= 16

PACKAGE ?= morpheus

BUILDDIR     ?= $(MAKEFILE_DIR)build
INSTALLDIR   ?= $(MAKEFILE_DIR)install
# BUILDDIR     ?= $(MAKEFILE_DIR)build_release
# INSTALLDIR   ?= $(MAKEFILE_DIR)dist

DISTDIR        ?= $(MAKEFILE_DIR)dist
DISTBUILDDIR   ?= $(DISTDIR)/build
DISTINSTALLDIR ?= $(DISTDIR)/$(PACKAGE)/usr/local/$(PACKAGE)

CMAKE_GENERATOR ?= Ninja
 
#############################################################################

MKDIR = mkdir -p
CMAKE = cmake
NICE = nice

#############################################################################

LLVM_REPO = git@github.com:llvm/llvm-project.git
LLVM_FORK = git@gitlab.kuleuven.be:u0126303/llvm-project.git

SRCDIR_LLVM   = $(MAKEFILE_DIR)llvm
SRCDIR_CLANG  = $(MAKEFILE_DIR)clang

BUILDDIR_LLVM = $(BUILDDIR)/llvm

CMAKE_FLAGS_LLVM += -G "$(CMAKE_GENERATOR)"
CMAKE_FLAGS_LLVM += -S $(SRCDIR_LLVM)
ifeq ($(CMAKE_GENERATOR), Ninja)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_COMPILE_JOBS=$(JOBS)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_LINK_JOBS=2
endif
CMAKE_FLAGS_LLVM += -DLLVM_TARGETS_TO_BUILD=RISCV
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PROJECTS="llvm;clang;lld"
CMAKE_FLAGS_LLVM += -DLLVM_USE_LINKER=
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_LLD=1
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PLUGINS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_USE_NEWPM=OFF
CMAKE_FLAGS_LLVM += -DCMAKE_EXPORT_COMPILE_COMMANDS=1
CMAKE_FLAGS_LLVM += -DCMAKE_CXX_COMPILER=clang++ 
CMAKE_FLAGS_LLVM += -DCMAKE_C_COMPILER=clang

DEV_CMAKE_FLAGS_LLVM = $(CMAKE_FLAGS_LLVM)
DEV_CMAKE_FLAGS_LLVM += -B $(BUILDDIR_LLVM)
DEV_CMAKE_FLAGS_LLVM += -DCMAKE_INSTALL_PREFIX=$(INSTALLDIR)
DEV_CMAKE_FLAGS_LLVM += -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

#############################################################################

.PHONY: all
all:
	@echo BUILD_TYPE=$(BUILD_TYPE)
	@echo JOBS=$(JOBS)
	@echo CMAKE_GENERATOR=$(CMAKE_GENERATOR)

.PHONY: configure-build
configure-build:
	$(MKDIR) $(BUILDDIR_LLVM)
	$(CMAKE) $(DEV_CMAKE_FLAGS_LLVM)

.PHONY: build
build:
ifneq ($(CMAKE_GENERATOR), Ninja)
	$(CMAKE) --build $(BUILDDIR_LLVM) -- -j$(JOBS)
else
	$(CMAKE) --build $(BUILDDIR_LLVM)
endif

.PHONY: install
install:
	$(CMAKE) --build $(BUILDDIR_LLVM) --target install

.PHONY: clean
clean:
	#$(RM) $(BUILDDIR)
	#$(RM) $(DISTBUILDDIR)

.PHONY: realclean
realclean:
	$(RM) $(BUILDDIR)
	$(RM) $(INSTALLDIR)
	$(RM) $(DISTDIR)
