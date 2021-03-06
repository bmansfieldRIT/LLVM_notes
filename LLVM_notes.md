# LLVM Cheatsheet

https://en.wikipedia.org/wiki/LLVM

## About LLVM:
collection of modular and reusable compiler and toolchain technologies
used to develop compiler front and back ends
written in C++
designed for compile-time, link-time, run-times nd idle-time optimization of programs in arbitrary programming languages
LLVM initially stood for Low Level Virtual Machine, name now just refers to the umbrella project of the LLVM IR, LLVM Debugger, LLVM implementation of the C++ standard library, etc.
LLVM administered by LLVM foundation, president & compiler engineer Tanya Lattner
GCC has historically outperformed LLVM by about 10% on average, newer reports indicate that speeds between the two are now comparable

## History:
work started on LLVM in 2000 at UofIllinois at Urbana Champaign
under direction of Vikram Adve and Chris Lattner
originally developed as research infrastructure to investigate dynamic compilation techniques for static and dynamic programming languages
written to be a replacement for the existing code generator in the GCC stack
released under a permissive free software license
in 2005, Apple hires Lattner to work on LLVM for use in Apple development systems
becomes and integral part of macOS and iOS
In 2012, awarded a ACM Software System Award by the Acm
in 2013, Sony uses Clang (a front end for LLVM) in SDK for ps4

## Features:
LLVM provides a middle layer for compilers, taking IR and outputting optimized IR
that IR can be converted and linked into machine dependent assembly language code for a target platform
LLVM can accept GCC IR
supports a language independent instruction set and type system
each instruction is in static single assignment form (SSA) - each var(typed register) is assigned once then frozen
helps simplify analysis of dependancies between variables
allows code to be statically compiled, like GCC, or JIT compiled like Java
JIT compiler can optimize unneeded static branches out of a program at runtime (useful for partial evaluation)
example: used in OpenGL pipeline on Mac OSX Leopard to support missing hardware features. leave code in IR, then compile based on what features the GPU actually has, for high end GPUs leave code as is, for low end GPUs, may need to simulate some advanced instructions in the CPU.


## LLVM type System:
basic types: integer, floating point
derived types: pointers, arrays, vectors, structures, and functions
example: a class in C++ can be represented in LLVM by a mix of structures, functions, and arrays of pointers

### Components:

Front ends:
many GCC front ends have been modified to work with LLVM (Ada, C, C++, D, Delphi, Fortran, Haskell, Obj C, Swift)
Clang supports C, C+, Obj C, aims to more easily replace C/Obj C compiler in GCC with a system more easily integrated into IDEs and wider support for multithreading

The IR:
strongly typed RISC (reduced instruction set computing) instruction set
abstracts away details of target
has infinite set of temporary registers %01, %02…
supports 3 (functionally equivalent) forms of IR: human readable assembly, C++ object, dense bitcode for serializing

Backends:
as of Version 3.4, supports many instruction sets including ARM, Qualcomm Hexagon, MIPS, Nvidia Parallel Thread Execution, PowerPC, AMD TeraScale, x86, x86-64, XCore
LLVM Machine Code (MC) subproject is LLVM’s framework for translating machine instructions between textual forms and machine code. formerly relied on system assembler (common), MC supports most LLVM targets, x86, x86-64, ARM, ARM64

Linker:
lld project is a built-in platform independent linker for LLVM
removes dependance on third party linkers
supports ELF, PE/COFF, Mach-O

Hello world in IR:
@.str = internal constant [14 x i8] c"hello, world\0A\00"

declare i32 @printf(i8*, ...)

define i32 @main(i32 %argc, i8** %argv) nounwind {
entry:
    %tmp1 = getelementptr [14 x i8], [14 x i8]* @.str, i32 0, i32 0
    %tmp2 = call i32 (i8*, ...) @printf( i8* %tmp1 ) nounwind
    ret i32 0
}


The Architecture of Open Source Applications Chapter 11

http://www.aosabook.org/en/llvm.html


Problems with Other Compiler Designs:
Modular 3 phase design allows front ends, backend to be dropped in to target new languages/architectures
front end devs never have to worry about back end devs
more work on llvm optimizer itself leads to overall better optimization than individual language compilers

Other modular designs in compilers:
Java uses a JIT compiler, runtime support, and byte code format. So anyone can produce byte code and use Java runtime and JIT compiler. But then, forced to use garbage collection, particular object model, very little flexibility. suboptimal performance when compiling languages very unlike Java (ex C)

Generate C Code, send through C compiler. can use optimizer, code generator.good flexibility, control over runtime, easy to understand. no efficient implementation of exception handling, slows compilation speeds, poor debugging experience, problematic for langs that C doesn’t support features of (ex guaranteed tail calls)

GCC model. many front and backends, evolving into a cleaner design, now supports GIMPLE tuples as the optimizer design representation. But GCC is a monolith application, cant just pull in parts to get debugging info, static analysis, etc. front end parts generate back end data, back end uses front end structures for debugging info, etc.

LLVM IR:
designed to host mid level analyses and optimizations
designed to support lightweight runtime optimizations, cross-function/ interprocedual optimizations, whole program analysis, aggressive restructuring transformations.

IR must be easy for front end to generate, and expressive enough to perform important optimizations for real targets

instructions are in three address form, take some number of inputs and produce and output in a different register
IR is like a lowe level, RISC like virtual instructions et
supports linear sequences of simple instructions like add, subtrat, compare, branch

strongly typed in a simple type system
i32 is a 32 bit integer
i32** is a pointer to a pointer to a 32 bit integer

calling convention abstracted through call and ret instructions and explicit arguments
infinite set of temporary registers available named with he % character

IR implemented three ways: textual format, in memory data structure inspected and modified by optimizations, and dense efficient bitcode binary on disk.
llvm-as assembles textual .ll file into .bc bitcode
llvm-dis turns .bc into .ll

IR examples:
define i32 @add1(i32 %a, i32 %b){ //  defining a function
	entry:	// entry label is always this?
		%tmp1 = add i32 %a, %b // built in functions don’t need the @ character
		ret i32 %tmp1
}

calling a function:
%tmp4 = call i32 @add1(i32 %a, i32 %b)

comparison:
%tmp1 = icmp eq i32 %a, 0

branching:
br i1 %tmp1, label %done, label %recursive // what is i1? user defined labels always use %notation

Writing an IR Optimization:
optimizations are written in C++ (what LLVM is written in)
pattern:
look for a pattern o be transformed
verify that transformation is safe, correct for matched code
do transformation, update code

easy optimization is arthimetic identities (x-0 = x, x-x = 0)
use match() and m_() functions to perform declarative pattern matching operations on LLVM IR code
ex:
// X - 0 -> X
if (match(Op1, m_Zero()))
  return Op0;


driver code to replace opcodes with simplified instructions:
for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I)
  if (Value *V = SimplifyInstruction(I))
    I->replaceAllUsesWith(V);

LLVM is a Collection of Libraries:
the optimizer runs a series of passes on compilation
for Clang, running -O0 would run no optimizations, -O3 would run 67 optimizations (as of LLVM 2.8)
Each pass derives from the Pass class
most passes written in a single .cpp file
their subclass of Pass defined in an anonymous namespace (completely private to defining file)
code outside file needs to get the pass, so a single function is exported to create the file

ex:
namespace {
  class Hello : public FunctionPass {
  public:
    // Print out the names of functions in the LLVM IR being optimized.
    virtual bool runOnFunction(Function &F) {
      cerr << "Hello: " << F.getName() << "\n";
      return false;
    }
  };
}

FunctionPass *createHelloPass() { return new Hello(); }


passes are compiled into .o files, the built into series of archive .a (on unix) files

Code Generation:
works in a similar way to IR. Code generation algorithms can often be shared, so code generation is done in a series of passes.
passes: instruction selection, register allocation, scheduling, code layout optimization, assembly emission
ex. x86 has few registers, needs register-pressure-reducing scheduler, PowerPC has many registers, needs latency optimizing scheduler

Target Description Files
each generic pass would need to know about the specific architecture of the target
ex. shared register allocator pass needs to know the register file of each target + restraints between instructions and register operands
each target provides a target description in a declarative domain-specific language (.td file) processed by tblgen tool

Interesting Capabilities of LLVM’s Modular Design
LLVM IR can be de/serialized to bitcode
Link Time Optimization
Clang is capable of this, using -O4 or -flto
usually compiler only sees one .c w/headers at a time
instead of completely compiling, generate .o files and then perform link-time optimization before code generation
while many modern compilers can do this, it is expensive and slow serialization process. in LLVM, this is natural process, and works across different source languages because IR is source lang neutral

can even perform install-time optimization, when software would be boxed and shipped, so that scheduling can be done only when you know the specifics of which target the software will be run on

Unit testing with LLVM
want to check that a particular optimization is running properly
ex. constant propagation pass
; RUN: opt < %s -constprop -S | FileCheck %s
define i32 @test() {
  %A = add i32 4, 5
  ret i32 %A
  ; CHECK: @test()
  ; CHECK: ret i32 9
}

BugPoint LLVM Debugger:
reduces input to small example, and finds the optimization responsible for optimizer crash
uses techniques similar to Delta Debugging


Closing thoughts, retrospective on Design:
APIs will often be aggressively changed, even the core IR, so backwards compatibility is not a major concern for LLVM
this is done in the name of rapid forward progress/prototyping

# Create a working compiler with the LLVM framework, Part 1

https://www.ibm.com/developerworks/library/os-createcompilerllvm1/
