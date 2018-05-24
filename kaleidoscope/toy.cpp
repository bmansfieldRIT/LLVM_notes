/*
* toy.cpp
* Transcribed by hand from the llvm docs (http://llvm.org/docs/tutorial/LangImpl02.html)
* by Brian Mansfield
*/

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "KaleidoscopeJIT.h"

using namespace llvm;

static LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
static std::unique_ptr<Module> TheModule;
static std::map<std::string, Value *> NamedValues;
static std::unique_ptr<KaleidoscopeJIT> TheJIT;

// lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things
enum Token {
    tok_eof = -1,

    // commands
    tok_def = -2,
    tok_extern = -3,

    //primary
    tok_identifier = -4,
    tok_number = -5
};

static std::string IdentifierStr; // filled in if tok_identifier
static double NumVal;              // filled in if tok_number

static int gettok() {
    static int LastChar = ' ';

    // skip any whitespace
    while (isspace(LastChar))
        LastChar = getchar();

    if (isalpha(LastChar)){ // identifier: [a-zA-Z][a-zA-Z0-9]*
        IdentifierStr = LastChar;
        while (isalnum((LastChar = getchar())))
            IdentifierStr += LastChar;
        //printf("%s", IdentifierStr.c_str());
        if (IdentifierStr == "def")
            return tok_def;
        if (IdentifierStr == "extern")
            return tok_extern;
        return tok_identifier;
    }

    if (isdigit(LastChar) || LastChar == '.'){  // Number: [0-9.]+
        std::string NumStr;
        do {
            NumStr += LastChar;
            LastChar = getchar();
        } while ( isdigit(LastChar) || LastChar == '.');

        NumVal = strtod(NumStr.c_str(), nullptr);
        return tok_number;
    }

    if (LastChar == '#'){
        // comment until end of line
        do
            LastChar = getchar();
        while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

        if (LastChar != EOF)
            return gettok();
    }

    if (LastChar == EOF)
        return tok_eof;

    int ThisChar = LastChar;
    LastChar = getchar();
    return ThisChar;
}

namespace {
// ExprAST - Base class for all expression nodes
class ExprAST {
public:
    virtual ~ExprAST() = default;
    virtual Value *codegen() = 0;
};

// NumberExprAST - Expression class for numeric literals like .0
class NumberExprAST : public ExprAST {
    double Val;

public:
    NumberExprAST(double Val) : Val(Val) {}
    Value *codegen() override;
};

// VariableExprAST - Expression class for refreencing a variable
class VariableExprAST : public ExprAST {
    std::string Name;

public:
    VariableExprAST(const std::string &Name) : Name(Name) {}
    Value *codegen() override;
};

// BinaryExprAST - Expression class for a binary operator
class BinaryExprAST : public ExprAST {
    char Op;
    std::unique_ptr<ExprAST> LHS, RHS;

public:
    BinaryExprAST(char op, std::unique_ptr<ExprAST> LHS,
                    std::unique_ptr<ExprAST> RHS)
        : Op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
    Value *codegen() override;
};

// CallExprAST - Expression class for function calls
class CallExprAST : public ExprAST {
    std::string Callee;
    std::vector<std::unique_ptr<ExprAST>> Args;

public:
    CallExprAST(const std::string &Callee,
                std::vector<std::unique_ptr<ExprAST>> Args)
        : Callee(Callee), Args(std::move(Args)) {}
    Value *codegen() override;
};

// PrototypeAST - represents the prototype for a function,
// which captures its name and arguments (implicitly then,
// its number of arguments as well.)
class PrototypeAST {
    std::string Name;
    std::vector<std::string> Args;

public:
    PrototypeAST(const std::string &Name, std::vector<std::string> Args)
        : Name(Name), Args(std::move(Args)) {}
    const std::string &getName() const { return Name; }
    Function *codegen();
};

// FunctionAST - represents a function definition itself
class FunctionAST {
    std::unique_ptr<PrototypeAST> Proto;
    std::unique_ptr<ExprAST> Body;

public:
    FunctionAST(std::unique_ptr<PrototypeAST> Proto,
                std::unique_ptr<ExprAST> Body)
        : Proto(std::move(Proto)), Body(std::move(Body)) {}
    Function *codegen();
};

} // end anonymous namespace

// CurTok/getNextToken - provides a simple token buffer. CurTok is
// the current token the parser is looking at. getNextToken reads
// another token from the lexer and updates CurTok with its results.
static int CurTok;
static int getNextToken() {
    return CurTok = gettok();
}

// LogError* - helper functions for error handling
std::unique_ptr<ExprAST> LogError(const char *Str) {
    fprintf(stderr, "LogError: %s\n", Str);
    return nullptr;
}
std::unique_ptr<PrototypeAST> LogErrorP(const char *Str){
    LogError(Str);
    return nullptr;
}

Value *LogErrorV(const char *Str){
    LogError(Str);
    return nullptr;
}

// numberexpr ::= number
static std::unique_ptr<ExprAST> ParseNumberExpr() {
    auto Result = llvm::make_unique<NumberExprAST>(NumVal);
    getNextToken(); // consume the numer
    return std::move(Result);
}

static std::unique_ptr<ExprAST> ParseExpression();

// parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> ParseParenExpr() {
    getNextToken(); // eat (.
    auto V = ParseExpression();
    if (!V)
        return nullptr;

    if (CurTok != ')')
        return LogError("expected ')'");
    getNextToken(); // eat ).
    return V;
}

// identifierexpr
//  ::= identifier
//  ::= identifier '(' expression* ')'
static std::unique_ptr<ExprAST> ParseIdentifierExpr() {
    std::string IdName = IdentifierStr;

    getNextToken(); // eat identifier

    if (CurTok != '(') // simple variable ref
        return llvm::make_unique<VariableExprAST>(IdName);

    // Call
    getNextToken(); // eat (
    std::vector<std::unique_ptr<ExprAST>> Args;
    if (CurTok != ')'){
        while (true){
            if (auto Arg = ParseExpression())
                Args.push_back(std::move(Arg));
            else
                return nullptr;

            if (CurTok == ')')
                break;

            if (CurTok != ',')
                return LogError("Exprected ')' or ',' in argument list");
            getNextToken();
        }
    }

    // eat the ')'
    getNextToken();

    return llvm::make_unique<CallExprAST>(IdName, std::move(Args));
}

// primary
//  ::= identifierexpr
//  ::= numberexpr
//  ::= parenexpr
static std::unique_ptr<ExprAST> ParsePrimary() {
    switch (CurTok){
    default:
        return LogError("unknown token when expecting an expression");
    case tok_identifier:
        return ParseIdentifierExpr();
    case tok_number:
        return ParseNumberExpr();
    case '(':
        return ParseParenExpr();
    }
}

// BinopPrecedence - This holds the precedence for each binary
// operator that is defined
static std::map<char, int> BinopPrecedence;

// GetTokPrecedence - get the precedence of the pending binary opaerator token
static int GetTokPrecedence(){
    if (!isascii(CurTok)){
        return -1;
    }

    // make sure its a declared binop
    auto TokPrec = BinopPrecedence[CurTok];
    if (TokPrec <= 0) return -1;
    return TokPrec;
}



// binopsrhs
//  := ('+' primary)*
static std::unique_ptr<ExprAST> ParseBinOpRHS(int ExprPrec,
                                               std::unique_ptr<ExprAST> LHS){
    // if this is a binop, find its precedence
    while (true){
        auto TokPrec = GetTokPrecedence();

        // if this is a binop that binds at least as tightly as
        // the current binop, consume it, otherwise we are done
        if (TokPrec < ExprPrec)
            return LHS;

        auto BinOp = CurTok;
        getNextToken(); // eat binop

        // Parse the primary expression after the binary operator
        auto RHS = ParsePrimary();
        if(!RHS)
            return nullptr;

        // if BinOp binds less tightly with RHS than the operator after RHS,
        // let the pending operator take RHS as its LHS
        auto NextPrec = GetTokPrecedence();
        if (TokPrec < NextPrec) {
            RHS = ParseBinOpRHS(TokPrec+1, std::move(RHS));
            if (!RHS)
                return nullptr;
        }

        // Merge LHS/RHS
        LHS = llvm::make_unique<BinaryExprAST>(BinOp, std::move(LHS),
                                                        std::move(RHS));
    }
}

static std::unique_ptr<ExprAST> ParseExpression() {
    auto LHS = ParsePrimary();
    if (!LHS)
        return nullptr;

    return ParseBinOpRHS(0, std::move(LHS));
}


// prototype
//  ::= id '(' id* ')'
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    if (CurTok != tok_identifier)
        return LogErrorP("Expected function name in prototype");

    auto FnName = IdentifierStr;
    getNextToken();

    if (CurTok != '(')
        return LogErrorP("Expected '(' in prototype");

    // read the list of argument names
    std::vector<std::string> ArgNames;
    while (getNextToken() == tok_identifier)
        ArgNames.push_back(IdentifierStr);
    if (CurTok != ')')
        return LogErrorP("Expected ')' in prototype");

    // success
    getNextToken(); // eat ')'

    return llvm::make_unique<PrototypeAST>(FnName, std::move(ArgNames));
}

// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> ParseDefinition(){
    getNextToken(); // eat def
    auto Proto = ParsePrototype();
    if (!Proto) return nullptr;

    if (auto E = ParseExpression())
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    return nullptr;
}

// external ::= 'extern' prototype
static std::unique_ptr<PrototypeAST> ParseExtern(){
    getNextToken(); // eat extern.
    return ParsePrototype();
}

// toplevelexpr ::= expression
static std::unique_ptr<FunctionAST> ParseTopLevelExpr() {
    if (auto E = ParseExpression()){
        // make an anonymous proto
        auto Proto = llvm::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>());
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

Value *NumberExprAST::codegen() {
    return ConstantFP::get(TheContext, APFloat(Val));
}

Value *VariableExprAST::codegen() {
    // look up this variable in the function
    Value *V = NamedValues[Name];
    if (!V)
        LogErrorV("Unknown variable name");
    return V;
}

Value *BinaryExprAST::codegen() {
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R)
        return nullptr;

    switch (Op){
    case '+':
        return Builder.CreateFAdd(L, R, "addtmp");
    case '-':
        return Builder.CreateFSub(L, R, "addtmp");
    case '*':
        return Builder.CreateFMul(L, R, "addtmp");
    case '<':
        L = Builder.CreateFCmpULT(L, R, "addtmp");
        // convert boolean 0 or 1 to double 0.0 or 1.0
        return Builder.CreateUIToFP(L, Type::getDoubleTy(TheContext), "booltmp");
    default:
        return LogErrorV("invalid binary operator");
    }
}

Value *CallExprAST::codegen(){
    // look up the name in the global module table
    Function *CalleeF = TheModule->getFunction(Callee);
    if (!CalleeF)
        return LogErrorV("Unknown function referenced");

    // if argument mismatch error
    if (CalleeF->arg_size() != Args.size())
        return LogErrorV("Incorrect # of arguments passed");

    std::vector<Value *> ArgsV;
    for (unsigned i = 0, e = Args.size(); i != e; ++i){
        ArgsV.push_back(Args[i]->codegen());
        if (!ArgsV.back())
            return nullptr;
    }

    return Builder.CreateCall(CalleeF, ArgsV, "calltmp");
}

Function *PrototypeAST::codegen(){
    // make the function type: double (double, double) etc.
    std::vector<Type*> Doubles(Args.size(),
        Type::getDoubleTy(TheContext));
    FunctionType *FT =
        FunctionType::get(Type::getDoubleTy(TheContext), Doubles, false);
    Function *F =
        Function::Create(FT, Function::ExternalLinkage, Name, TheModule.get());

    // set names for all arguments
    unsigned Idx = 0;
    for (auto &Arg : F->args())
        Arg.setName(Args[Idx++]);

    return F;
}

Function *FunctionAST::codegen(){
    // first, check for an existing function from a previous 'extern' declaration
    Function *TheFunction = TheModule->getFunction(Proto->getName());
    if (!TheFunction)
        TheFunction = Proto->codegen();
    if (!TheFunction)
        return nullptr;
    if (!TheFunction->empty())
        return (Function*)LogErrorV("Function cannot be redefined");

    // create a new basic block to start insertion into
    BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
    Builder.SetInsertPoint(BB);

    // record the function argument in the NamedValue map
    NamedValues.clear();
    for (auto &Arg : TheFunction->args())
        NamedValues[Arg.getName()] = &Arg;

    if (Value *RetVal = Body->codegen()){
        // finish off the function
        Builder.CreateRet(RetVal);

        // validate the generated code, chekcing for consistency
        verifyFunction(*TheFunction);

        // optimize the function
        TheFPM->run(*TheFunction);

        return TheFunction;
    }

    // error reading body, remove function
    TheFunction->eraseFromParent();
    return nullptr;
}

void InitializeModuleAndPassManager(){
    // open a new module
    TheModule = llvm::make_unique<Module>("mycool jit", TheContext)
    TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

    // create a new pass manager attached to it
    TheFPM = llvm::make_unique<FunctionPassManager>(TheModule.get());
    // do simple 'peephole' optimizations and bit-twiddling optimizations
    TheFPM->add(createIntructionCombiningPass());
    // reassociate expressions
    TheFPM->add(createReassociatePass());
    // eliminate common subexpressions
    TheFPM->add(createGVNPass());
    // simplify the control flow graph (delete unreachable block, etc.)
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()){
            fprintf(stderr, "Read function definition: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleExtern() {
    if (auto ProtoAST = ParseExtern()) {
        if (auto *FnIR = ProtoAST->codegen()){
            fprintf(stderr, "Read extern: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void HandleTopLevelExpression() {
    // Evaluate a top-level expression into an anonymous function.
    if (auto FnAST = ParseTopLevelExpr()) {
        if (auto *FnIR = FnAST->codegen()){
            fprintf(stderr, "Read top-level expr: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void MainLoop(){
    while (true){
        fprintf(stderr, "ready> ");
        switch (CurTok){
        case tok_eof:
            return;
        case ';':
            getNextToken();
            break;
        case tok_def:
            HandleDefinition();
            break;
        case tok_extern:
            HandleExtern();
            break;
        default:
            HandleTopLevelExpression();
            break;
        }
    }
}

int main() {

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();

    // Install standard binary operators
    // 1 is the lowest precedence
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 30;
    BinopPrecedence['*'] = 40; // highest

    // prime the first token
    fprintf(stderr, "ready> ");
    getNextToken();

    TheJit = llvm::make_unique<KaleidoscopeJIT>();

    // run the main interpreter loop now
    MainLoop();

    return 0;
}
