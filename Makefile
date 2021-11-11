CMAKE_FLAGS_LLVM =

-include Makefile.local

MAKEFILE_DIR = $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

BUILD_TYPE ?= Release # One of (Debug, Release)
JOBS       ?= 1

PACKAGE ?= morpheus

BUILDDIR     ?= $(MAKEFILE_DIR)build
INSTALLDIR   ?= $(MAKEFILE_DIR)$(PACKAGE)

DISTDIR        ?= $(MAKEFILE_DIR)dist
DISTBUILDDIR   ?= $(DISTDIR)/build
DISTINSTALLDIR ?= $(DISTDIR)/$(PACKAGE)

CMAKE_GENERATOR ?= Unix Makefiles # One of (Unix Makefiles, Ninja)

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

DISTBUILDDIR_LLVM = $(DISTBUILDDIR)/llvm

CMAKE_FLAGS_LLVM += -G "$(strip $(CMAKE_GENERATOR))"
CMAKE_FLAGS_LLVM += -S $(SRCDIR_LLVM)
CMAKE_FLAGS_LLVM += -B $(BUILDDIR_LLVM)
ifeq ($(CMAKE_GENERATOR), Ninja)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_COMPILE_JOBS=$(JOBS)
CMAKE_FLAGS_LLVM += -DLLVM_PARALLEL_LINK_JOBS=2
endif
CMAKE_FLAGS_LLVM += -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
CMAKE_FLAGS_LLVM += -DCMAKE_INSTALL_PREFIX=$(INSTALLDIR)
CMAKE_FLAGS_LLVM += -DLLVM_TARGETS_TO_BUILD="RISCV"
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PROJECTS="llvm;clang;lld"
CMAKE_FLAGS_LLVM += -DLLVM_USE_LINKER=gold
CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_PLUGINS=ON
CMAKE_FLAGS_LLVM += -DLLVM_USE_NEWPM=OFF

#CMAKE_FLAGS_LLVM += -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_ENABLE_ASSERTIONS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_CCACHE_BUILD=ON
#For DEBUG builds, use shared libs, unless you have a lot of memory
#CMAKE_FLAGS_LLVM += -DBUILD_SHARED_LIBS=ON
#CMAKE_FLAGS_LLVM += -DLLVM_BUILD_LLVM_DYLIB=ON

DIST_CMAKE_FLAGS_LLVM =
DIST_CMAKE_FLAGS_LLVM += -G "$(strip $(CMAKE_GENERATOR))"
DIST_CMAKE_FLAGS_LLVM += -DCMAKE_INSTALL_PREFIX=$(DISTINSTALLDIR)
DIST_CMAKE_FLAGS_LLVM += -C $(SRCDIR_CLANG)/cmake/caches/DistributionExample.cmake
DIST_CMAKE_FLAGS_LLVM += -S $(SRCDIR_LLVM)
DIST_CMAKE_FLAGS_LLVM += -B $(DISTBUILDDIR_LLVM)

#############################################################################

.PHONY: all
all:
	@echo BUILD_TYPE=$(BUILD_TYPE)
	@echo JOBS=$(JOBS)
	@echo CMAKE_GENERATOR=$(CMAKE_GENERATOR)

.PHONY: configure
configure:
	$(MKDIR) $(BUILDDIR_LLVM)
	$(CMAKE) $(CMAKE_FLAGS_LLVM)

.PHONY: build
build:
ifneq ($(CMAKE_GENERATOR), Ninja)
	$(NICE) $(CMAKE) --build $(BUILDDIR_LLVM) -- -j$(JOBS)
else
	$(NICE) $(CMAKE) --build $(BUILDDIR_LLVM)
endif

.PHONY: install
install: build
	$(CMAKE) --build $(BUILDDIR_LLVM) --target install

.PHONY: dist-configure
dist-configure:
	$(MKDIR) $(DISTBUILDDIR)
	$(CMAKE) $(DIST_CMAKE_FLAGS_LLVM) $(SRCDIR_LLVM)

.PHONY: dist-build
dist-build:
	$(CMAKE) --build $(DISTBUILDDIR_LLVM) --target stage2-distribution

.PHONY: dist-install
dist-install: dist-build
	$(CMAKE) --build $(DISTBUILDDIR_LLVM) --target stage2-install-distribution

.PHONY: clean
clean:
	#$(RM) $(BUILDDIR)
	#$(RM) $(DISTBUILDDIR)

.PHONY: realclean
realclean:
	$(RM) $(BUILDDIR)
	$(RM) $(INSTALLDIR)
	$(RM) $(DISTDIR)
