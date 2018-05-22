/*
* TestClass.cpp
* Transcribed from the IBM tutorial on writing LLVM passes
* (https://www.ibm.com/developerworks/library/os-createcompilerllvm2/index.html)
* By Brian Mansfield
*/

#include <cstdio>
#include "llvm/Pass.h"
#include "llvm/IR/Function.h"

class TestClass : public llvm::FunctionPass {
public:
    // constructor takes a char ID, content of ID not critical (LLVM uses address of char)
    TestClass() : llvm::FunctionPass(TestClass::ID){}
    // implement the virtual function from Function.h
    virtual bool runOnFunction(llvm::Function &F){
        if (F.getName().startswith("hello")){
            printf("%s", "Function name starts with hello\n");
        }
        return false;
    }
    static char ID; // could be a global
};
char TestClass::ID = 'a';
// tell LLVM that this is a new pass
// Located in PassSupport.h
// first parameter is the name of the pass to be used on the command line with opt
static llvm::RegisterPass<TestClass> global_("test_llvm", "test llvm", false, false);
