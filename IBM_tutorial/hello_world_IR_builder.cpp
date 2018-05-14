#include <stdio.h>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"

int main(){
    llvm::LLVMContext Context;
    llvm::Module* module = new llvm::Module("top", Context);
    llvm::IRBuilder<> builder(Context);
    
    module->dump();
}
