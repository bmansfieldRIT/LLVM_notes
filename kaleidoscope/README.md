`toy.cpp` is an implementation of the kaleidoscope compiler as described in the llvm tutorial docs (http://llvm.org/docs/tutorial/LangImpl02.html)

#### Compile & Run

```
# Compile
clang++ -g -O3 toy.cpp `llvm-config --cxxflags --ldflags --libs --system-libs core` -o toy
# Run
./toy
```

#### Usage
```
$ ./toy
ready> def foo(a b) a*a + 2*a*b + b*b;
Read function definition:
define double @foo(double %a, double %b) {
entry:
  %multmp = fmul double %a, %a
  %multmp1 = fmul double 2.000000e+00, %a
  %multmp2 = fmul double %multmp1, %b
  %addtmp = fadd double %multmp, %multmp2
  %multmp3 = fmul double %b, %b
  %addtmp4 = fadd double %addtmp, %multmp3
  ret double %addtmp4
}
$
```


### Done
* Lexer
* Parser
* Basic LLVM IR Generator

### To Do
* Better error handling
* Catch more edge cases
* Add File I/O
* Add Optimizations
* Add Control Flow
* Add User Defined Operators
* Add Mutable Variables
