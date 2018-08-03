/*
* toy.cpp
* Transcribed by hand from the llvm docs (http://llvm.org/docs/tutorial/LangImpl02.html)
* by Brian Mansfield
*/

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include "KaleidoscopeJIT.h"

using namespace llvm;
using namespace llvm::orc;
using namespace llvm::sys;

static LLVMContext TheContext;
static IRBuilder<> Builder(TheContext);
static std::unique_ptr<Module> TheModule;
static std::map<std::string, AllocaInst*> NamedValues;
static std::unique_ptr<KaleidoscopeJIT> TheJIT;
static std::unique_ptr<legacy::FunctionPassManager> TheFPM;


// lexer returns tokens [0-255] if it is an unknown character, otherwise one
// of these for known things
enum Token {
    tok_eof = -1,

    // commands
    tok_def = -2,
    tok_extern = -3,

    //primary
    tok_identifier = -4,
    tok_number = -5,

    // control flow
    tok_if = -6,
    tok_then = -7,
    tok_else = -8,

    tok_for = -9,
    tok_in = -10,

    tok_binary = -11,
    tok_unary = -12,

    // var definition
    tok_var = -13,
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
        if (IdentifierStr == "if")
            return tok_if;
        if (IdentifierStr == "then")
            return tok_then;
        if (IdentifierStr == "else")
            return tok_else;
        if (IdentifierStr == "for")
            return tok_for;
        if (IdentifierStr == "in")
            return tok_in;
        if (IdentifierStr == "binary")
            return tok_binary;
        if (IdentifierStr == "unary")
            return tok_unary;
        if (IdentifierStr == "var")
            return tok_var;
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
    const std::string &getName() const { return Name; }
};

// VarExprAST - expression class for var/in
class VarExprAST : public ExprAST {
    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
    std::unique_ptr<ExprAST> Body;
public:
    VarExprAST(std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
                std::unique_ptr<ExprAST> Body)
    : VarNames(std::move(VarNames)), Body(std::move(Body)) {}

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

// UnaryExprAST - Expression class for a unary operator
class UnaryExprAST : public ExprAST {
    char Opcode;
    std::unique_ptr<ExprAST> Operand;

public:
    UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
    : Opcode(Opcode), Operand(std::move(Operand)) {}

    Value *codegen() override;
};

// IfExprAST - Expression class for if-then-else control flow statements
class IfExprAST : public ExprAST {
    std::unique_ptr<ExprAST> Cond, Then, Else;
public:
    IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
              std::unique_ptr<ExprAST> Else)
    : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
    Value *codegen() override;
};

// ForExprAST - Expression class for For loops
class ForExprAST : public ExprAST {
    std::string VarName;
    std::unique_ptr<ExprAST> Init, Cond, Step, Body;
public:
    ForExprAST(const std::string &VarName, std::unique_ptr<ExprAST> Init,
            std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Step,
            std::unique_ptr<ExprAST> Body)
    : VarName(VarName), Init(std::move(Init)), Cond(std::move(Cond)), Step(std::move(Step)),
        Body(std::move(Body)) {}
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
    bool IsOperator;
    unsigned Precedence; // precedence if a binary op

public:
    PrototypeAST(const std::string &Name, std::vector<std::string> Args,
                bool IsOperator = false, unsigned Prec = 0)
        : Name(Name), Args(std::move(Args)), IsOperator(IsOperator), Precedence(Prec) {}
    const std::string &getName() const { return Name; }
    Function *codegen();

    bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
    bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

    char getOperatorName() const {
        assert(isUnaryOp() || isBinaryOp());
        return Name[Name.size() - 1];
    }

    unsigned getBinaryPrecedence() const { return Precedence; }
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
static std::unique_ptr<ExprAST> ParsePrimary();

// unary
//      ::= primary
//      ::= '!' unary
static std::unique_ptr<ExprAST> ParseUnary() {
    // if the current token is not an operator, it must be a primary expr
    if (!isascii(CurTok) || CurTok == '(' || CurTok == ',')
        return ParsePrimary();

    // if this is a unary operator, read it
    int Opc = CurTok;
    getNextToken();
    if (auto Operand = ParseUnary())
        return llvm::make_unique<UnaryExprAST>(Opc, std::move(Operand));
    return nullptr;
}

static std::unique_ptr<ExprAST> ParseIfExpr() {
    getNextToken();
    auto Cond = ParseExpression();
    if (!Cond)
        return nullptr;

    if (CurTok != tok_then)
        return LogError("expected then");
    getNextToken();

    auto Then = ParseExpression();
    if (!Then)
        return nullptr;

    if (CurTok != tok_else)
        return LogError("expected else");
    getNextToken();

    auto Else = ParseExpression();
    if (!Else)
        return nullptr;

    return llvm::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                        std::move(Else));
}

// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
static std::unique_ptr<ExprAST> ParseForExpr(){
    getNextToken(); // eat the for

    if (CurTok != tok_identifier)
        return LogError("expected identifier after for");

    std::string IdName = IdentifierStr;
    getNextToken(); // eat identifier

    if (CurTok != '=')
        return LogError("expected '=' after for");
    getNextToken(); // eat '='

    auto Init = ParseExpression();
    if (!Init)
        return nullptr;
    if (CurTok != ',')
        return LogError("expected ',' after for start value");
    getNextToken();

    auto Cond = ParseExpression();
    if (!Cond)
        return nullptr;

    // the step value is optional
    std::unique_ptr<ExprAST> Step;
    if (CurTok == ',') {
        getNextToken();
        Step = ParseExpression();
        if (!Step)
            return nullptr;
    }

    if (CurTok != tok_in)
        return LogError("expected 'in' after for");
    getNextToken(); // eat the 'in'.

    auto Body = ParseExpression();
    if (!Body)
        return nullptr;

    return llvm::make_unique<ForExprAST>(IdName, std::move(Init),
                                        std::move(Cond), std::move(Step),
                                        std::move(Body));

}

// varexpr ::= 'var' identifier ('=' expression)?
//                  (',' identifier ('=' expression)?)* 'in' expression
static std::unique_ptr<ExprAST> ParseVarExpr(){
    getNextToken(); // eat the var

    std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

    // at least one variable name is required
    if (CurTok != tok_identifier)
        return LogError("expected identifier after var");

    while (1) {
        std::string Name = IdentifierStr;
        getNextToken(); // eat identifier

        // read the optional initializer
        std::unique_ptr<ExprAST> Init;
        if (CurTok == '='){
            getNextToken(); // eat the '='.

            Init = ParseExpression();
            if (!Init)
                return nullptr;
        }

        VarNames.push_back(std::make_pair(Name, std::move(Init)));

        // end of var list, exit loop
        if (CurTok != ',')
            break;
        getNextToken(); // eat the ','

        if (CurTok != tok_identifier)
            return LogError("expected identifier list after var");

    }
    // at this point, we have the 'in'
    if (CurTok != tok_in)
        return LogError("Expected 'in' keyword after 'var'");
    getNextToken(); // eat in

    auto Body = ParseExpression();
    if (!Body)
        return nullptr;

    return llvm::make_unique<VarExprAST>(std::move(VarNames), std::move(Body));
}


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
    case tok_if:
        return ParseIfExpr();
    case tok_for:
        return ParseForExpr();
    case tok_var:
        return ParseVarExpr();
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
//  := ('+' unary)*
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
        auto RHS = ParseUnary();
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
    auto LHS = ParseUnary();
    if (!LHS)
        return nullptr;

    return ParseBinOpRHS(0, std::move(LHS));
}

// prototype
//  ::= id '(' id* ')'
//  ::= binary LETTER number? (id, id)
static std::unique_ptr<PrototypeAST> ParsePrototype() {
    std::string FnName;

    unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary
    unsigned BinaryPrecedence = 30;

    switch (CurTok){
    default:
        return LogErrorP("Expected function name in prototype");
    case tok_identifier:
        FnName = IdentifierStr;
        Kind = 0;
        getNextToken();
        break;
    case tok_unary:
        getNextToken();
        if (!isascii(CurTok))
            return LogErrorP("Expected unary operator");
        FnName = "unary";
        FnName += (char)CurTok;
        Kind = 1;
        getNextToken();
        break;
    case tok_binary:
        getNextToken();
        if (!isascii(CurTok))
            return LogErrorP("Expected binary operator");
        FnName = "binary";
        FnName += (char)CurTok;
        Kind = 2;
        getNextToken();

        // read the precedence if present
        if (CurTok == tok_number){
            if (NumVal < 1 || NumVal > 100)
                return LogErrorP("invalid precedence: must be 1..100");
            BinaryPrecedence = (unsigned)NumVal;
            getNextToken();
        }
        break;
    }

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

    // verify right number of names for operator
    if (Kind && ArgNames.size() != Kind)
        return LogErrorP("Invalid number of operands for operator");

    return llvm::make_unique<PrototypeAST>(FnName, std::move(ArgNames), Kind != 0,
                                            BinaryPrecedence);
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
        auto Proto = llvm::make_unique<PrototypeAST>("main", std::vector<std::string>());
        return llvm::make_unique<FunctionAST>(std::move(Proto), std::move(E));
    }
    return nullptr;
}

static std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

// CreateEntryBlockAlloca - Create an alloca instruction in the entry block of
// the function. This is used for mutable variables, etc.
static AllocaInst *CreateEntryBlockAlloca(Function *TheFunction,
                                            const std::string &VarName){
    IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                    TheFunction->getEntryBlock().begin());
    return TmpB.CreateAlloca(Type::getDoubleTy(TheContext), 0, VarName.c_str());
}

Function *getFunction(std::string Name){
    // see if function has already been added to the current module
    if (auto *F = TheModule->getFunction(Name))
        return F;

    // if not, check whether we can codegen the declaration from some existing prototype
    auto FI = FunctionProtos.find(Name);
    if (FI != FunctionProtos.end())
        return FI->second->codegen();

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

    // load the value.
    return Builder.CreateLoad(V, Name.c_str());
}

Value *BinaryExprAST::codegen() {
    // special case '=' because we don't want to emit the LHS as an expression
    if (Op == '='){
        // assignment requires the LHS to be an identifier
        VariableExprAST *LHSE = static_cast<VariableExprAST*>(LHS.get());
        if (!LHSE)
            return LogErrorV("destiniation of '=' must be a variable");

        // codegen the RHS
        Value *Val = RHS->codegen();
        if (Val)
            return nullptr;

        // look up the name
        Value *Variable = NamedValues[LHSE->getName()];
        if (!Variable)
            return LogErrorV("unknown variable name");

        Builder.CreateStore(Val, Variable);
        return Val;
    }

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
        break;
    }

    // if it wasn't a builtin binary operator, it must be a user defined one. emit
    // a call to it.
    Function *F = getFunction(std::string("binary") + Op);
    assert(F && "binary operator not found!");

    Value *Ops[2] = { L, R };
    return Builder.CreateCall(F, Ops, "binop");
}

Value *VarExprAST::codegen() {
    std::vector<AllocaInst *> OldBindings;

    Function *TheFunction = Builder.GetInsertBlock()->getParent();

    // register all variables and emit their initializer
    for (unsigned i = 0, e = VarNames.size(); i != e; ++i){
        const std::string &VarName = VarNames[i].first;
        ExprAST *Init = VarNames[i].second.get();

        // emit the initializer before adding the variable to scope, this prevents
        // the initializer from referencing the variable itself, and permits stuff
        // like this:
        //      var a = 1 in
        //          var a = a in ..   # refers to outer 'a'
        Value *InitVal;
        if (Init){
            InitVal = Init->codegen();
            if (!InitVal)
                return nullptr;
        } else { // if not specified, use 0.0
            InitVal = ConstantFP::get(TheContext, APFloat(0.0));
        }

        AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
        Builder.CreateStore(InitVal, Alloca);

        // remember the old variable binding so that we can restore the binding when
        // we unrecurse
        OldBindings.push_back(NamedValues[VarName]);

        // remember this binding
        NamedValues[VarName] = Alloca;
    }

    // codegen the body, now that all vars are in scope
    Value *BodyVal = Body->codegen();
    if (!BodyVal)
        return nullptr;

    // pop all our variables from scope
    for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
        NamedValues[VarNames[i].first] = OldBindings[i];

    // return the body computation
    return BodyVal;
}

Value *UnaryExprAST::codegen() {
    Value *OperandV = Operand->codegen();
    if (!OperandV)
        return nullptr;

    Function *F = getFunction(std::string("unary") + Opcode);
    if (!F)
        return LogErrorV("Unknown unary operator");

    return Builder.CreateCall(F, OperandV, "unop");
}

Value *IfExprAST::codegen(){
    Value *CondV = Cond->codegen();
    if (!CondV)
        return nullptr;

    // convert condition to a bool by comparing non-equal to 0.0
    CondV = Builder.CreateFCmpONE(
        CondV, ConstantFP::get(TheContext, APFloat(0.0)), "ifcond");

    Function *TheFunction = Builder.GetInsertBlock()->getParent();

    // create blocks for the then and else cases. Insert the 'then' block at the end of the function
    BasicBlock *ThenBB = BasicBlock::Create(TheContext, "then", TheFunction);
    BasicBlock *ElseBB = BasicBlock::Create(TheContext, "else");
    BasicBlock *MergeBB = BasicBlock::Create(TheContext, "ifcont");

    Builder.CreateCondBr(CondV, ThenBB, ElseBB);

    // emit then value
    Builder.SetInsertPoint(ThenBB);

    Value *ThenV = Then->codegen();
    if(!ThenV)
        return nullptr;

    Builder.CreateBr(MergeBB);
    // codegen of 'Then' can change the current block, update ThenBB for the PHI
    ThenBB = Builder.GetInsertBlock();

    // emit else block
    TheFunction->getBasicBlockList().push_back(ElseBB);
    Builder.SetInsertPoint(ElseBB);

    Value *ElseV = Else->codegen();
    if (!ElseV)
        return nullptr;

    Builder.CreateBr(MergeBB);
    // codegen of 'Else' can change the current block, update ElseBB for the PHI.
    ElseBB = Builder.GetInsertBlock();

    // emit merge block
    TheFunction->getBasicBlockList().push_back(MergeBB);
    Builder.SetInsertPoint(MergeBB);
    PHINode *PN =
        Builder.CreatePHI(Type::getDoubleTy(TheContext), 2, "iftmp");

    PN->addIncoming(ThenV, ThenBB);
    PN->addIncoming(ElseV, ElseBB);
    return PN;
}

Value *ForExprAST::codegen(){
    // make the new basic block for the loop header, inserting after current block
    Function *TheFunction = Builder.GetInsertBlock()->getParent();

    // Create an alloca for the variable in the entry block.
    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);

    // emit the start code first, without 'variable' in scope
    Value *InitVal = Init->codegen();
    if (!InitVal)
        return nullptr;

    // store the value into the alloca
    Builder.CreateStore(InitVal, Alloca);

    // make the new basic block for the loop header, inserting after current block.
    BasicBlock *LoopBB = BasicBlock::Create(TheContext, "loop", TheFunction);

    // insert an explicit fall through from the current block to the LoopBB
    Builder.CreateBr(LoopBB);

    // start insertion in LoopBB
    Builder.SetInsertPoint(LoopBB);

    // within the loop, the variable is defined equal to the phi node. If it
    // shadows an existing variable, we have to restore it, so save it now
    AllocaInst *OldVal = NamedValues[VarName];
    NamedValues[VarName] = Alloca;

    // emit the body of the loop. this, like any other expr, can change the
    // current BB. Note that we ignore the value computed by the body, but don't
    // allow an error
    if (!Body->codegen())
        return nullptr;

    // emit the step value
    Value *StepVal = nullptr;
    if (Step){
        StepVal = Step->codegen();
        if (!StepVal)
            return nullptr;
    } else {
        // if not specified, use 1.0
        StepVal = ConstantFP::get(TheContext, APFloat(1.0));
    }

    // compute the end condition
    Value *EndCond = Cond->codegen();
    if (!EndCond)
        return nullptr;

    // reload, increment, and restore the alloca. This handles the case where
    // the body of the loop mutates the variable
    Value *CurVar = Builder.CreateLoad(Alloca, VarName.c_str());
    Value *NextVar = Builder.CreateFAdd(CurVar, StepVal, "nextvar");
    Builder.CreateStore(NextVar, Alloca);

    // convert condition to a bool by comparing non-equal to 0.0
    EndCond = Builder.CreateFCmpONE(
        EndCond, ConstantFP::get(TheContext, APFloat(0.0)), "loopcond");

    BasicBlock *AfterBB =
        BasicBlock::Create(TheContext, "afterloop", TheFunction);

    // insert the conditional branch into the end of LoopEndBB
    Builder.CreateCondBr(EndCond, LoopBB, AfterBB);

    // any new code will be inserted in AfterBB
    Builder.SetInsertPoint(AfterBB);

    // restore the unshadowed Variable
    if (OldVal)
        NamedValues[VarName] = OldVal;
    else
        NamedValues.erase(VarName);

    // for expr always returns 0.0
    return Constant::getNullValue(Type::getDoubleTy(TheContext));
}


Value *CallExprAST::codegen(){
    // look up the name in the global module table
    Function *CalleeF = getFunction(Callee);
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

    // transfer ownership of the protoype to the functionprotos map, but keep a
    // reference to it for use below
    auto &P = *Proto;
    FunctionProtos[Proto->getName()] = std::move(Proto);
    Function *TheFunction = getFunction(P.getName());
    if (!TheFunction)
        return nullptr;

    // if this is an operator, install it
    if (P.isBinaryOp())
        BinopPrecedence[P.getOperatorName()] = P.getBinaryPrecedence();

    // create a new basic block to start insertion into
    BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
    Builder.SetInsertPoint(BB);

    // record the function argument in the NamedValue map
    NamedValues.clear();
    for (auto &Arg : TheFunction->args()){
        // create an alloca for this variable
        AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, Arg.getName());

        // Store the initial value into the alloca
        Builder.CreateStore(&Arg, Alloca);

        // add arguments to variable symbol table
        NamedValues[Arg.getName()] = Alloca;
    }
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
    TheModule = llvm::make_unique<Module>("my cool jit", TheContext);
    TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());

    // create a new pass manager attached to it
    TheFPM = llvm::make_unique<legacy::FunctionPassManager>(TheModule.get());
    // Promote allocas to registers
    TheFPM->add(llvm::createPromoteMemoryToRegisterPass());
    // do simple 'peephole' optimizations and bit-twiddling optimizations
    TheFPM->add(llvm::createInstructionCombiningPass());
    // reassociate expressions
    TheFPM->add(llvm::createReassociatePass());
    // eliminate common subexpressions
    TheFPM->add(llvm::createNewGVNPass());
    // simplify the control flow graph (delete unreachable block, etc.)
    TheFPM->add(llvm::createCFGSimplificationPass());

    TheFPM->doInitialization();
}

static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()){
            fprintf(stderr, "Read function definition: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
            TheJIT->addModule(std::move(TheModule));
            InitializeModuleAndPassManager();
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
            FunctionProtos[ProtoAST->getName()] = std::move(ProtoAST);
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

//===----------------------------------------------------------------------===//
// "Library" functions that can be "extern'd" from user code.
//===----------------------------------------------------------------------===//

#ifdef _WIN32
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

/// putchard - putchar that takes a double and returns 0.
extern "C" DLLEXPORT double putchard(double X) {
  fputc((char)X, stderr);
  return 0;
}

/// printd - printf that takes a double prints it as "%f\n", returning 0.
extern "C" DLLEXPORT double printd(double X) {
  fprintf(stderr, "%f\n", X);
  return 0;
}

static void HandleTopLevelExpression() {
    // Evaluate a top-level expression into an anonymous function.
    if (auto ExprAST = ParseTopLevelExpr()) {
        if (auto *ExprIR = ExprAST->codegen()){
            fprintf(stderr, "Read top-level expression: ");
            ExprIR->print(errs());
            fprintf(stderr, "\n");

            // JIT the module containing the anaymous expression, keeping a
            // handle so we can free it later
            auto H = TheJIT->addModule(std::move(TheModule));
            InitializeModuleAndPassManager();

            // search the JIT for the __anon_expr symbol
            auto ExprSymbol = TheJIT->findSymbol("__anon_expr");
            assert(ExprSymbol && "Function not found");

            // Get the symbols address and cast it to the right type
            // (takes no arguments, returns a double) so we can call it as a
            // native function
            double (*FP)() = (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
            fprintf(stderr, "Evaluated to %f\n", FP());

            // delete the anonymous expression module from the JIT
            TheJIT->removeModule(H);
        }
    } else {
        // Skip token for error recovery.
        getNextToken();
    }
}

static void MainLoop(){
    while (true){
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

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//

int main() {

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();

    // Install standard binary operators
    // 1 is the lowest precedence
    BinopPrecedence['='] = 2;
    BinopPrecedence['<'] = 10;
    BinopPrecedence['+'] = 20;
    BinopPrecedence['-'] = 30;
    BinopPrecedence['*'] = 40; // highest

    // prime the first token
    getNextToken();

    TheJIT = llvm::make_unique<KaleidoscopeJIT>();

    InitializeModuleAndPassManager();

    // run the main interpreter loop now
    MainLoop();

    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();

    auto TargetTriple = sys::getDefaultTargetTriple();
    TheModule->setTargetTriple(TargetTriple);

    std::string Error;
    auto Target = TargetRegistry::lookupTarget(TargetTriple, Error);

    // print an error and exit if we couldn't find the requested target
    // this generally occurs is we've forgotten to initialize the
    // TargetRegistry or we have a bogus target triple
    if (!Target){
        errs() << Error;
        return 1;
    }

    auto CPU = "generic";
    auto Features = "";

    TargetOptions opt;
    auto RM = Optional<Reloc::Model>();
    auto TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);

    TheModule->setDataLayout(TargetMachine->createDataLayout());

    auto Filename = "output.o";
    std::error_code EC;
    raw_fd_ostream dest(Filename, EC, sys::fs::F_None);

    if (EC){
        errs() << "Could not open file: " << EC.message();
        return 1;
    }

    legacy::PassManager pass;
    auto FileType = TargetMachine::CGFT_ObjectFile;

    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)){
        errs() << "TargetMachine can't emit a file of this type";
        return 1;
    }

    pass.run(*TheModule);
    dest.flush();

    outs() << "Wrote " << Filename << "\n";

    return 0;
}
