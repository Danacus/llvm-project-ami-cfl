# LLVM Compiler with Control Flow Linearization using Architectural Mimicry (AMi)

This repository contains LLVM 16 extended with:

- Secrecy labels in clang, lowered to function parameter attributes in LLVM IR, and further lowered to the backend through the SelectionDAG.
- Automatic control flow linearization making use of architectural mimicry (RISC-V backend).

References:

- [Compiler Support for Control-Flow Linearization Leveraging Hardware Defenses](https://daan.vanoverloop.xyz/masters-thesis-ami-pcfl.pdf), master's thesis of Daan Vanoverloop (that's me!)
- "Hardware Support to Accelerate Side-channel Resistant Programs" by Winderix et al. (under submission)

## Usage instructions

The following LLVM options can be used to configure control flow linearization methods:

| Option                             | Values (default in **bold**) | Description                                                                                                                                                                                                                                                        |
|------------------------------------|------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| -riscv-enable-ami-linearization    | true, **false**              | Enable linearization using architectural mimicry (AMi)                                                                                                                                                                                                             |
| -riscv-enable-molnar-linearization | true, **false**              | Enable linearization using Molnar's method, based on state-of-the-art research of Borrello et al. and Wu et al.                                                                                                                                                    |
| -riscv-ami-general-linearization   | true, **false**              | When linearization using AMi is enabled, enable linearization of reducible control flow by using the method based on Partial Control Flow Linearization by Moll and Hack. If set to false, secret-dependent branches in the input IR are assumed to be structured. |

## Example of Annotated Program

Below is an example of an annotated program. Note that at the time of writing this README, support for the upcoming C2x standard must be enabled in clang with `-std=c2x`.

```c
// Macro to make annotation of types easier
#define _secret_ [[clang::annotate_type("secret")]]

// Only needed for linearization with Molnar's method, set to 0xffffffff initially
#define CFL_VAR  __attribute__((section("cfl_data")))
CFL_VAR unsigned int cfl_taken = (unsigned)-1;

static int v;

// This function is marked as mimicable, hence it can safely be called while in mimicry mode
[[clang::noinline, clang::mimicable]]
static void foo(int i) {
  v++;
}

// This function takes a secret integer a and public integer b as input, and returns a secret integer
int _secret_ ifthenloop(int _secret_ a, int b) {
  v = 0;

  if (a < b) {
    int i;

    #pragma clang loop unroll(disable)
    for (i=0; i<3; i++) {
      foo(i);
    }
  }

  return v;
}

int main(void) {
  // When linearized, both function calls should take the same number of cycles to execute
  (void) ifthenloop(1, 2);
  (void) ifthenloop(2, 1);

  return 0;
}
```

To compile and harden this example, the following command can be used:

`clang -nostdlib -O3 --target=riscv32 -march=rv32im -std=c2x -mllvm -riscv-enable-ami-linearization=true -mllvm -riscv-ami-general-linearization=true main.c`

Alternatively, `llc` can be invoked on IR code to generate RISC-V assembly:

`llc -march=riscv32 -riscv-enable-ami-linearization=true -riscv-ami-general-linearization=true main.ll -o main.s`

(original README below)

# The LLVM Compiler Infrastructure

This directory and its sub-directories contain the source code for LLVM,
a toolkit for the construction of highly optimized compilers,
optimizers, and run-time environments.

The README briefly describes how to get started with building LLVM.
For more information on how to contribute to the LLVM project, please
take a look at the
[Contributing to LLVM](https://llvm.org/docs/Contributing.html) guide.

## Getting Started with the LLVM System

Taken from [here](https://llvm.org/docs/GettingStarted.html).

### Overview

Welcome to the LLVM project!

The LLVM project has multiple components. The core of the project is
itself called "LLVM". This contains all of the tools, libraries, and header
files needed to process intermediate representations and convert them into
object files. Tools include an assembler, disassembler, bitcode analyzer, and
bitcode optimizer. It also contains basic regression tests.

C-like languages use the [Clang](http://clang.llvm.org/) frontend. This
component compiles C, C++, Objective-C, and Objective-C++ code into LLVM bitcode
-- and from there into object files, using LLVM.

Other components include:
the [libc++ C++ standard library](https://libcxx.llvm.org),
the [LLD linker](https://lld.llvm.org), and more.

### Getting the Source Code and Building LLVM

The LLVM Getting Started documentation may be out of date. The [Clang
Getting Started](http://clang.llvm.org/get_started.html) page might have more
accurate information.

This is an example work-flow and configuration to get and build the LLVM source:

1. Checkout LLVM (including related sub-projects like Clang):

     * ``git clone https://github.com/llvm/llvm-project.git``

     * Or, on windows, ``git clone --config core.autocrlf=false
    https://github.com/llvm/llvm-project.git``

2. Configure and build LLVM and Clang:

     * ``cd llvm-project``

     * ``cmake -S llvm -B build -G <generator> [options]``

        Some common build system generators are:

        * ``Ninja`` --- for generating [Ninja](https://ninja-build.org)
          build files. Most llvm developers use Ninja.
        * ``Unix Makefiles`` --- for generating make-compatible parallel makefiles.
        * ``Visual Studio`` --- for generating Visual Studio projects and
          solutions.
        * ``Xcode`` --- for generating Xcode projects.

        Some common options:

        * ``-DLLVM_ENABLE_PROJECTS='...'`` and ``-DLLVM_ENABLE_RUNTIMES='...'`` ---
          semicolon-separated list of the LLVM sub-projects and runtimes you'd like to
          additionally build. ``LLVM_ENABLE_PROJECTS`` can include any of: clang,
          clang-tools-extra, cross-project-tests, flang, libc, libclc, lld, lldb,
          mlir, openmp, polly, or pstl. ``LLVM_ENABLE_RUNTIMES`` can include any of
          libcxx, libcxxabi, libunwind, compiler-rt, libc or openmp. Some runtime
          projects can be specified either in ``LLVM_ENABLE_PROJECTS`` or in
          ``LLVM_ENABLE_RUNTIMES``.

          For example, to build LLVM, Clang, libcxx, and libcxxabi, use
          ``-DLLVM_ENABLE_PROJECTS="clang" -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi"``.

        * ``-DCMAKE_INSTALL_PREFIX=directory`` --- Specify for *directory* the full
          path name of where you want the LLVM tools and libraries to be installed
          (default ``/usr/local``). Be careful if you install runtime libraries: if
          your system uses those provided by LLVM (like libc++ or libc++abi), you
          must not overwrite your system's copy of those libraries, since that
          could render your system unusable. In general, using something like
          ``/usr`` is not advised, but ``/usr/local`` is fine.

        * ``-DCMAKE_BUILD_TYPE=type`` --- Valid options for *type* are Debug,
          Release, RelWithDebInfo, and MinSizeRel. Default is Debug.

        * ``-DLLVM_ENABLE_ASSERTIONS=On`` --- Compile with assertion checks enabled
          (default is Yes for Debug builds, No for all other build types).

      * ``cmake --build build [-- [options] <target>]`` or your build system specified above
        directly.

        * The default target (i.e. ``ninja`` or ``make``) will build all of LLVM.

        * The ``check-all`` target (i.e. ``ninja check-all``) will run the
          regression tests to ensure everything is in working order.

        * CMake will generate targets for each tool and library, and most
          LLVM sub-projects generate their own ``check-<project>`` target.

        * Running a serial build will be **slow**. To improve speed, try running a
          parallel build. That's done by default in Ninja; for ``make``, use the option
          ``-j NNN``, where ``NNN`` is the number of parallel jobs to run.
          In most cases, you get the best performance if you specify the number of CPU threads you have.
          On some Unix systems, you can specify this with ``-j$(nproc)``.

      * For more information see [CMake](https://llvm.org/docs/CMake.html).

Consult the
[Getting Started with LLVM](https://llvm.org/docs/GettingStarted.html#getting-started-with-llvm)
page for detailed information on configuring and compiling LLVM. You can visit
[Directory Layout](https://llvm.org/docs/GettingStarted.html#directory-layout)
to learn about the layout of the source code tree.

## Getting in touch

Join [LLVM Discourse forums](https://discourse.llvm.org/), [discord chat](https://discord.gg/xS7Z362) or #llvm IRC channel on [OFTC](https://oftc.net/).

The LLVM project has adopted a [code of conduct](https://llvm.org/docs/CodeOfConduct.html) for
participants to all modes of communication within the project.
