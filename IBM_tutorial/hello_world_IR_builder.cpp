#include <stdio.h>
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"

int main(){

    // provides the context for variable creation, etc.
    // usefulf or multiple threads, for now we use the default global context
    llvm::LLVMContext Context;

    // begin the program by creating an LLVM module
    llvm::Module* module = new llvm::Module("top", Context);

    // provides the API to create LLVM instructions and insert them into basic blocks
    // construct simply here by passing in the global context
    llvm::IRBuilder<> builder(Context);

    // associate a return type for the main function
    llvm::FunctionType *funcType = llvm::FunctionType::get(builder.getInt32Ty(), false);

    // create the main function, given the return type
    llvm::Function *mainFunc = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, "main", module);

    // define the set of instructions that main has to execute
    // define the basic block first, where the instructions will reside
    // define a label for the block in its constructor
    llvm::BasicBlock *entry = llvm::BasicBlock::Create(Context, "entrypoint", mainFunc);

    // tell the LLVM engine where to insert the next instructions
    builder.SetInsertPoint(entry);

    // create the global string
    llvm::Value *helloWorld = builder.CreateGlobalStringPtr("Hello World!\n");

    // declare the puts method
    // first must make the appropriate FunctionType*
    // puts accepts i8* as the input argument, so create an array of arguments containing only that
    std::vector<llvm::Type*> putsArgs;
    putsArgs.push_back(builder.getInt8Ty()->getPointerTo());
    // similar to a vector, does not contain any underlying data
    // primarily used to wrap data blocks (arrays, vectors)
    llvm::ArrayRef<llvm::Type*> argsRef(putsArgs);

    // puts returns i32
    llvm::FunctionType *putsType = llvm::FunctionType::get(builder.getInt32Ty(), argsRef, false);

    // create the function
    // false indicates that no variable number of arguments follows

    llvm::Constant *putsFunc = module->getOrInsertFunction("puts", putsType);

    // create the call to puts
    builder.CreateCall(putsFunc, helloWorld);

    // create the return value;
    llvm::ConstantInt *retval = llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0, false);
    builder.CreateRet(retval);

    // show the modules contents
    module->dump();
}
