`hello_world_IR_builder.cpp` is a transcription of the code contained in the IBM developer tutorial on creating an llvm compiler (https://www.ibm.com/developerworks/library/os-createcompilerllvm1/)

#### Compile & Run
```
# Compile
clang++ -stdlib=libc++ -g -O3 hello_world_IR_builder.cpp `llvm-config --cxxflags --ldflags --libs --system-libs`
# Run
./a.out > code.out
# Run Interpreter
lli code.out
```

#### Notes:
* On my Macbook, running High Sierra, I needed to add `xcrun clang++` to use the xcode toolchain, otherwise it was defaulting to an older version of clang++, without some essential c++11 features used by llvm.
* The tutorial does not use the flag `--system-libs`, I found this necessary, otherwise I got undefined symbols during linking.
* The tutorial returns a void value, which does not match with the signature created for main by the newer version of LLVM. I added an instruction to create a ConstantInt with a value of 0, and returned that instead.
