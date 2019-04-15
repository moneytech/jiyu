

#include "llvm.h"
#include "ast.h"
#include "compiler.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

using namespace llvm;

void LLVM_Generator::init() {
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmParsers();
    InitializeAllAsmPrinters();

    std::string TargetTriple = llvm::sys::getDefaultTargetTriple();
    std::string Error;
    auto Target = TargetRegistry::lookupTarget(TargetTriple, Error);

    // Print an error and exit if we couldn't find the requested target.
    // This generally occurs if we've forgotten to initialise the
    // TargetRegistry or we have a bogus target triple.
    if (!Target) {
      errs() << Error;
      return;
    }

    auto CPU = "generic";
    auto Features = "";

    TargetOptions opt;
    auto RM = Optional<Reloc::Model>();
    TargetMachine = Target->createTargetMachine(TargetTriple, CPU, Features, opt, RM);



    llvm_context = new LLVMContext();
    llvm_module = new Module("Htn Module", *llvm_context);
    irb = new IRBuilder<>(*llvm_context);

    type_void = Type::getVoidTy(*llvm_context);
    type_i8   = Type::getInt8Ty(*llvm_context);
    type_i16  = Type::getInt16Ty(*llvm_context);
    type_i32  = Type::getInt32Ty(*llvm_context);
    type_i64  = Type::getInt64Ty(*llvm_context);

    type_string_length = nullptr;
    if (TargetMachine->getPointerSize(0) == 4) {
        type_string_length = type_i32;
    } else if (TargetMachine->getPointerSize(0) == 8) {
        type_string_length = type_i64;
    }

    assert(type_string_length);


    // Matches the definition in general.h
    type_string = StructType::create(*llvm_context, { type_i8->getPointerTo(), type_string_length }, "string", true/*packed*/);
}

void LLVM_Generator::finalize() {
    // @Cleanup duplicate
    std::string TargetTriple = llvm::sys::getDefaultTargetTriple();

    llvm_module->setDataLayout(TargetMachine->createDataLayout());
    llvm_module->setTargetTriple(TargetTriple);

    auto Filename = "output.o";
    std::error_code EC;
    raw_fd_ostream dest(Filename, EC, sys::fs::F_None);

    if (EC) {
      errs() << "Could not open file: " << EC.message();
      return;
    }

    legacy::PassManager pass;
    auto FileType = TargetMachine::CGFT_ObjectFile;

    llvm_module->dump();

    pass.add(createVerifierPass(false));
    if (TargetMachine->addPassesToEmitFile(pass, dest, nullptr, FileType)) {
      errs() << "TargetMachine can't emit a file of this type";
      return;
    }

    pass.run(*llvm_module);
    dest.flush();
}

static StringRef string_ref(String s) {
    return StringRef(s.data, s.length);
}

Type *LLVM_Generator::get_type(Ast_Type_Info *type) {
    if (type->type == Ast_Type_Info::VOID) {
        return type_void;
    }

    if (type->type == Ast_Type_Info::INTEGER) {
        switch (type->size) {
            case 1: return type_i8;
            case 2: return type_i16;
            case 4: return type_i32;
            case 8: return type_i64;
            default: assert(false);
        }
    }

    if (type->type == Ast_Type_Info::STRING) {
        return type_string;
    }

    if (type->type == Ast_Type_Info::POINTER) {
        auto pointee = get_type(type->pointer_to);
        return pointee->getPointerTo();
    }

    assert(false);
    return nullptr;
}

FunctionType *LLVM_Generator::create_function_type(Ast_Function *function) {
    Array<Type *> arguments;

    for (auto &arg : function->arguments) {
        if (arg->type_info == compiler->type_void) continue;

        Type *type = get_type(arg->type_info);
        arguments.add(type);
    }

    for (auto &ret : function->returns) {
        if (ret->type_info == compiler->type_void) continue;
        Type *type = get_type(ret->type_info)->getPointerTo();
        arguments.add(type);
    }
    return FunctionType::get(type_void, ArrayRef<Type *>(arguments.data, arguments.count), false);
}

Value *LLVM_Generator::get_value_for_decl(Ast_Declaration *decl) {
    for (auto &it : decl_value_map) {
        if (it.item1 == decl) {
            return it.item2;
        }
    }

    return nullptr;
}

static Value *create_alloca_in_entry(IRBuilder<> *irb, Type *type) {
    auto current_block = irb->GetInsertBlock();

    auto func = current_block->getParent();

    BasicBlock *entry = &func->getEntryBlock();
    irb->SetInsertPoint(entry->getTerminator());
    Value *alloca = irb->CreateAlloca(type);

    irb->SetInsertPoint(current_block);

    return alloca;
}

Value *LLVM_Generator::create_string_literal(Ast_Literal *lit) {
    assert(lit->literal_type == Ast_Literal::STRING);

    bool add_null = true;
    Constant *data = irb->CreateGlobalStringPtr(string_ref(lit->string_value));
    Constant *length = ConstantInt::get(type_string_length, lit->string_value.length);

    return ConstantStruct::get(type_string, { data, length });
}

Value *LLVM_Generator::emit_expression(Ast_Expression *expression, bool is_lvalue) {
    switch (expression->type) {
        case AST_SCOPE: {
            auto scope = static_cast<Ast_Scope *>(expression);
            emit_scope(scope);

            return nullptr;
        }

        case AST_BINARY_EXPRESSION: {
            auto bin = static_cast<Ast_Binary_Expression *>(expression);

            if (bin->operator_type == Token::EQUALS) {
                Value *left  = emit_expression(bin->left,  true);
                Value *right = emit_expression(bin->right, false);

                irb->CreateStore(right, left);
                return nullptr;
            } else {
                assert(false);
            }
        }

        case AST_LITERAL: {
            auto lit = static_cast<Ast_Literal *>(expression);

            switch (lit->literal_type) {
                case Ast_Literal::INTEGER: return ConstantInt::get(get_type(lit->type_info), lit->integer_value);
                case Ast_Literal::STRING:  return create_string_literal(lit);
                default: return nullptr;
            }
        }

        case AST_IDENTIFIER: {
            auto ident = static_cast<Ast_Identifier *>(expression);
            assert(ident->resolved_declaration);

            assert(ident->resolved_declaration->type == AST_DECLARATION);
            auto decl = static_cast<Ast_Declaration *>(ident->resolved_declaration);

            auto value = get_value_for_decl(decl);

            if (!is_lvalue) return irb->CreateLoad(value);

            return value;
        }

        case AST_DECLARATION: {
            auto decl = static_cast<Ast_Declaration *>(expression);
            if (decl->initializer_expression) {
                auto value = emit_expression(decl->initializer_expression);
                irb->CreateStore(value, get_value_for_decl(decl));
            }
            return nullptr;
        }

        case AST_FUNCTION_CALL: {
            auto call = static_cast<Ast_Function_Call *>(expression);

            Array<Value *> args;
            for (auto &it : call->argument_list) {
                auto value = emit_expression(it);
                args.add(value);
            }

            auto target_function = static_cast<Ast_Function *>(call->identifier->resolved_declaration);
            auto func = get_or_create_function(target_function);

            assert(func);
            // func->dump();
            return irb->CreateCall(func, ArrayRef<Value *>(args.data, args.count));
        }

        case AST_DEREFERENCE: {
            auto deref = static_cast<Ast_Dereference *>(expression);

            auto lhs = emit_expression(deref->left, true);

            assert(deref->element_path_index >= 0);
            assert(deref->byte_offset >= 0);

            auto valueptr = irb->CreateGEP(lhs, { ConstantInt::get(type_i32, 0), ConstantInt::get(type_i32, deref->element_path_index) });
            if (!is_lvalue) return irb->CreateLoad(valueptr);
			return valueptr;
		}
    }

    return nullptr;
}

Function *LLVM_Generator::get_or_create_function(Ast_Function *function) {
    assert(function->identifier);
    String name = function->identifier->name->name;

    auto func = llvm_module->getFunction(string_ref(name));

    if (!func) {
        FunctionType *function_type = create_function_type(function);
        func = Function::Create(function_type, GlobalValue::LinkageTypes::ExternalLinkage, string_ref(name), llvm_module);
    }

    return func;
}

void LLVM_Generator::emit_scope(Ast_Scope *scope) {
    // setup variable mappings
    for (auto &it : scope->declarations) {
        if (it->type != AST_DECLARATION) continue;
        auto decl = static_cast<Ast_Declaration *>(it);

        auto alloca = create_alloca_in_entry(irb, get_type(it->type_info));

        if (decl->identifier) {
            alloca->setName(string_ref(decl->identifier->name->name));
        }

        assert(get_value_for_decl(decl) == nullptr);
        decl_value_map.add(MakeTuple(decl, alloca));
    }

    for (auto &it : scope->statements) {
        emit_expression(it);
    }
}

void LLVM_Generator::emit_function(Ast_Function *function) {
    assert(function->identifier && function->identifier->name);

    Function *func = get_or_create_function(function);
    if (!function->scope) return; // forward declaration of external thing

    // create entry block
    BasicBlock *entry = BasicBlock::Create(*llvm_context, "entry", func);
    BasicBlock *starting_block = BasicBlock::Create(*llvm_context, "start", func);

    irb->SetInsertPoint(entry);
    irb->CreateBr(starting_block);

    irb->SetInsertPoint(starting_block);
    emit_scope(function->scope);
    irb->CreateRetVoid();

    // func->dump();
    decl_value_map.clear();
}
