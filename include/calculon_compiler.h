/* Calculon © 2013 David Given
 * This code is made available under the terms of the Simplified BSD License.
 * Please see the COPYING file for the full license text.
 */

#ifndef CALCULON_COMPILER_H
#define CALCULON_COMPILER_H

#ifndef CALCULON_H
#error "Don't include this, include calculon.h instead."
#endif

class Compiler : public CompilerState
{
public:
	using CompilerState::builder;
	using CompilerState::module;
	using CompilerState::context;

private:
	class ASTNode;
	class ASTVariable;

	typedef Lexer L;
	typedef pair<string, char> Argument;

	using CompilerState::retain;
	using CompilerState::types;
	using CompilerState::intType;
public:
	using CompilerState::realType;
	using CompilerState::doubleType;
	using CompilerState::floatType;
	using CompilerState::booleanType;
private:

	map<string, int> _operatorPrecedence;
	TypeRegistry _typeRegistry;

	class TypeException : public CompilationException
	{
	public:
		TypeException(const string& what, ASTNode* node):
			CompilationException(node->position.formatError(what))
		{
		}
	};

	class SymbolException : public CompilationException
	{
		static string formatError(const string& id, ASTNode* node)
		{
			std::stringstream s;
			s << "unresolved symbol '" << id << "'";
			return node->position.formatError(s.str());
		}

	public:
		SymbolException(const string& id, ASTNode* node):
			CompilationException(formatError(id, node))
		{
		}
	};

	class OperatorException : public CompilationException
	{
		static string formatError(const string& id, ASTNode* node)
		{
			std::stringstream s;
			s << "unknown operator '" << id << "'";
			return node->position.formatError(s.str());
		}

	public:
		OperatorException(const string& id, ASTNode* node):
			CompilationException(formatError(id, node))
		{
		}
	};

	class IntrinsicTypeException : public CompilationException
	{
		static string formatError(const string& id, char type, ASTNode* node)
		{
			std::stringstream s;
			s << "wrong type applied to '" << id << "'";
			return node->position.formatError(s.str());
		}

	public:
		IntrinsicTypeException(const string& id, char type, ASTNode* node):
			CompilationException(formatError(id, type, node))
		{
		}
	};

public:
	Compiler(llvm::LLVMContext& context, llvm::Module* module,
			llvm::ExecutionEngine* engine, const map<string, string>& typealiases):
		CompilerState(context, module, engine),
		_typeRegistry(*this, typealiases)
	{
		types = &_typeRegistry;

		intType = llvm::IntegerType::get(context, 32);
		realType = types->find("real");
		doubleType = llvm::Type::getDoubleTy(context);
		floatType = llvm::Type::getFloatTy(context);
		booleanType = types->find("boolean");

		_operatorPrecedence["and"] = 5;
		_operatorPrecedence["or"] = 5;
		_operatorPrecedence["<"] = 10;
		_operatorPrecedence["<="] = 10;
		_operatorPrecedence[">"] = 10;
		_operatorPrecedence[">="] = 10;
		_operatorPrecedence["=="] = 10;
		_operatorPrecedence["!="] = 10;
		_operatorPrecedence["+"] = 20;
		_operatorPrecedence["-"] = 20;
		_operatorPrecedence["*"] = 30;
		_operatorPrecedence["/"] = 30;
	}

public:
	ToplevelSymbol* compile(std::istream& signaturestream, std::istream& codestream,
			SymbolTable* globals)
	{
		vector<VariableSymbol*> arguments;
		vector<VariableSymbol*> returns;

		L signaturelexer(signaturestream);
		parse_toplevelsignature(signaturelexer, arguments, returns);
		expect_eof(signaturelexer);

		/* Create the special symbol which represents the toplevel function. */

		ToplevelSymbol* toplevelsymbol = retain(new ToplevelSymbol("<toplevel>",
				arguments, returns));

		/* Compile the code to an AST. */

		L codelexer(codestream);
		MultipleSymbolTable symboltable(globals);
		ASTToplevel* ast = parse_toplevel(codelexer, toplevelsymbol, &symboltable);

		/* Ensure we've reached the end of the file. */

		expect(codelexer, L::ENDOFFILE);

		/* Create the interface function from this signature. */

		vector<llvm::Type*> externaltypes;

		for (unsigned i=0; i<arguments.size(); i++)
		{
			VariableSymbol* symbol = arguments[i];
			externaltypes.push_back(symbol->type->llvmx);
		}

		for (unsigned i=0; i<returns.size(); i++)
		{
			VariableSymbol* symbol = returns[i];
			llvm::Type* t = symbol->type->llvmx;
			if (!t->isPointerTy())
				t = t->getPointerTo();
			externaltypes.push_back(t);
		}

		llvm::FunctionType* ft = llvm::FunctionType::get(
				llvm::Type::getVoidTy(context),
				externaltypes, false);

		toplevelsymbol->function = llvm::Function::Create(ft,
				llvm::Function::ExternalLinkage,
				"Entrypoint", module);

		llvm::BasicBlock* bb = llvm::BasicBlock::Create(context, "entry",
			toplevelsymbol->function);
		builder.SetInsertPoint(bb);

		/* Marshal any input parameters to internal types. */

		llvm::Function::arg_iterator ii = toplevelsymbol->function->arg_begin();
		for (unsigned i=0; i<arguments.size(); i++)
		{
			llvm::Value* v = ii;
			VariableSymbol* symbol = arguments[i];

			v->setName(symbol->name);
			if (symbol->type->asVector())
				v = symbol->type->asVector()->loadFromArray(v);
			symbol->value = v;

			symboltable.add(symbol);

			ii++;
		}

		/* ...and remember the LLVM values where the output parameters will be
		 * stored. */

		for (unsigned i=0; i<returns.size(); i++)
		{
			llvm::Value* v = ii;
			VariableSymbol* symbol = returns[i];

			v->setName(symbol->name);
			symbol->value = v;

			ii++;
		}

		/* Generate the IR code. */

		ast->resolveVariables(*this);
		ast->codegen(*this);

		return toplevelsymbol;
	}

private:
	#include "calculon_ast.h"

private:
	void expect(L& lexer, int token)
	{
		if (lexer.token() != token)
		{
			std::stringstream s;
			s << "expected " << lexer.tokenname(token);
			lexer.error(s.str());
		}
		lexer.next();
	}

	void expect_operator(L& lexer, const string& s)
	{
		if ((lexer.token() != L::OPERATOR) || (lexer.id() != s))
			lexer.error(string("expected '"+s+"'"));
		lexer.next();
	}

	void expect_identifier(L& lexer, const string& s)
	{
		if ((lexer.token() != L::IDENTIFIER) || (lexer.id() != s))
			lexer.error(string("expected '"+s+"'"));
		lexer.next();
	}

	void expect_eof(L& lexer)
	{
		if (lexer.token() != L::ENDOFFILE)
			lexer.error("expected EOF");
	}

	void parse_identifier(L& lexer, string& id)
	{
		if (lexer.token() != L::IDENTIFIER)
			lexer.error("expected identifier");

		id = lexer.id();
		lexer.next();
	}

	void parse_list_separator(L& lexer)
	{
		switch (lexer.token())
		{
			case L::COMMA:
				lexer.next();
				break;

			case L::CLOSEPAREN:
				break;

			default:
				lexer.error("expected comma or close parenthesis");
		}
	}

	void parse_typespec(L& lexer, Type*& type)
	{
		if (lexer.token() != L::COLON)
			type = NULL;
		else
		{
			expect(lexer, L::COLON);

			if (lexer.token() != L::IDENTIFIER)
				lexer.error("expected a type name");

			std::stringstream typenm;
			typenm << lexer.id();
			lexer.next();

			if ((lexer.token() == L::OPERATOR) && (lexer.id() == "*"))
			{
				typenm << "*";
				lexer.next();

				if (lexer.token() != L::NUMBER)
					lexer.error("invalid n-vector type specifier");
				int size = (int)lexer.real();
				if ((Real)size != lexer.real())
					lexer.error("n-vector size must be an integer");
				if (size <= 0)
					lexer.error("n-vector size must be greater than 0");

				typenm << size;
				lexer.next();
			}

			type = types->find(typenm.str());
			if (!type)
			{
				std::stringstream s;
				s << "unknown type '" << typenm.str() << "'";
				lexer.error(s.str());
			}
		}
	}

	void parse_paramlist(L& lexer, vector<VariableSymbol*>& list)
	{
		expect(lexer, L::OPENPAREN);

		while (lexer.token() != L::CLOSEPAREN)
		{
			string id;
			parse_identifier(lexer, id);

			Type* type;
			parse_typespec(lexer, type);
			if (!type)
				type = realType;

			VariableSymbol* symbol = retain(new VariableSymbol(id, type));
			list.push_back(symbol);
			parse_list_separator(lexer);
		}

		expect(lexer, L::CLOSEPAREN);
	}

	void parse_functionsignature(L& lexer, vector<VariableSymbol*>& arguments,
			Type*& returntype)
	{
		parse_paramlist(lexer, arguments);
		parse_typespec(lexer, returntype);
		if (!returntype)
			returntype = realType;
	}

	void parse_toplevelsignature(L& lexer, vector<VariableSymbol*>& arguments,
			vector<VariableSymbol*>& returns)
	{
		parse_paramlist(lexer, arguments);
		expect(lexer, L::COLON);
		parse_paramlist(lexer, returns);
	}

	ASTNode* parse_variable_or_function_call(L& lexer)
	{
		Position position = lexer.position();

		string id;
		parse_identifier(lexer, id);

		if (lexer.token() == L::OPENPAREN)
		{
			/* Function call. */
			expect(lexer, L::OPENPAREN);

			vector<ASTNode*> arguments;
			while (lexer.token() != L::CLOSEPAREN)
			{
				arguments.push_back(parse_expression(lexer));
				parse_list_separator(lexer);
			}

			expect(lexer, L::CLOSEPAREN);

			return retain(new ASTFunctionCall(position, id, arguments));
		}
		else
		{
			/* Variable reference. */

			if ((id == "true") || (id == "false"))
				return retain(new ASTBoolean(position, id));
			else if (id == "pi")
				return retain(new ASTConstant(position, M_PI));
			else if (id == "Inf")
				return retain(new ASTConstant(position,
						std::numeric_limits<Real>::infinity()));
			else if (id == "NaN")
				return retain(new ASTConstant(position,
						std::numeric_limits<Real>::quiet_NaN()));
			else if (id == "return")
				return retain(new ASTReturn(position));
			else
				return retain(new ASTVariable(position, id));
		}

		assert(false);
		throw 0;
	}

	ASTNode* parse_leaf(L& lexer)
	{
		switch (lexer.token())
		{
			case L::NUMBER:
			{
				Position position = lexer.position();
				Real value = lexer.real();
				lexer.next();
				return retain(new ASTConstant(position, value));
			}

			case L::OPENPAREN:
			{
				expect(lexer, L::OPENPAREN);
				ASTNode* v = parse_expression(lexer);
				expect(lexer, L::CLOSEPAREN);
				return v;
			}

			case L::OPENBLOCK:
				return parse_vector(lexer);

			case L::OPERATOR:
			case L::IDENTIFIER:
			{
				const string& id = lexer.id();
				if (id == "let")
					return parse_let(lexer);
				else if (id == "if")
					return parse_if(lexer);
				return parse_variable_or_function_call(lexer);
			}
		}

		lexer.error("expected an expression");
		throw (void*) NULL; // do not return
	}

	ASTNode* parse_tight(L& lexer)
	{
		ASTNode* value = parse_leaf(lexer);

		switch (lexer.token())
		{
			case L::DOT:
			{
				Position position = lexer.position();
				expect(lexer, L::DOT);

				string id;
				parse_identifier(lexer, id);

				vector<ASTNode*> parameters;
				parameters.push_back(value);

				if (lexer.token() == L::OPENPAREN)
				{
					lexer.next();

					while (lexer.token() != L::CLOSEPAREN)
					{
						ASTNode* e = parse_expression(lexer);
						parameters.push_back(e);

						if (lexer.token() != L::COMMA)
							break;
						expect(lexer, L::COMMA);
					}

					expect(lexer, L::CLOSEPAREN);
				}

				return retain(new ASTFunctionCall(position, "method "+id,
						parameters));
			}

			case L::OPENBLOCK:
			{
				Position position = lexer.position();
				expect(lexer, L::OPENBLOCK);

				vector<ASTNode*> parameters;
				parameters.push_back(value);

				do
				{
					ASTNode* e = parse_expression(lexer);
					parameters.push_back(e);

					if (lexer.token() != L::COMMA)
						break;
					expect(lexer, L::COMMA);
				}
				while (true);

				expect(lexer, L::CLOSEBLOCK);
				return retain(new ASTFunctionCall(position, "method []",
						parameters));
			}
		};

		return value;
	}

	ASTNode* parse_unary(L& lexer)
	{
		if (lexer.token() == L::OPERATOR)
		{
			Position position = lexer.position();
			string id = lexer.id();

			if ((id == "-") || (id == "not"))
			{
				lexer.next();

				ASTNode* value = parse_tight(lexer);
				vector<ASTNode*> parameters;
				parameters.push_back(value);
				return retain(new ASTFunctionCall(position, "method "+id,
						parameters));
			}
		}

		return parse_tight(lexer);
	}

	ASTNode* parse_binary(L& lexer, int precedence)
	{
		ASTNode* lhs = parse_unary(lexer);

		while (lexer.token() == L::OPERATOR)
		{
			Position position = lexer.position();
			string id = lexer.id();
			int p = _operatorPrecedence[id];
			if (p == 0)
			{
				std::stringstream s;
				s << "unrecognised operator '" << id << "'";
				lexer.error(s.str());
			}

			if (p < precedence)
				break;

			lexer.next();
			ASTNode* rhs = parse_binary(lexer, p+1);

			if (id == "and")
				lhs = retain(new ASTCondition(position, lhs,
						rhs, retain(new ASTBoolean(position, "false"))));
			else if (id == "or")
				lhs = retain(new ASTCondition(position, lhs,
						retain(new ASTBoolean(position, "true")), rhs));
			else
			{
				vector<ASTNode*> parameters;
				parameters.push_back(lhs);
				parameters.push_back(rhs);
				lhs = retain(new ASTFunctionCall(position, "method "+id,
						parameters));
			}
		}

		return lhs;
	}

	ASTNode* parse_expression(L& lexer)
	{
		return parse_binary(lexer, 0);
	}

	ASTFrame* parse_let(L& lexer)
	{
		Position position = lexer.position();

		expect_identifier(lexer, "let");

		string id;
		parse_identifier(lexer, id);

		Type* returntype;
		parse_typespec(lexer, returntype);

		if (lexer.token() == L::OPENPAREN)
		{
			/* Return type for functions defaults to Real. */

			if (!returntype)
				returntype = realType;

			/* Function definition. */

			vector<VariableSymbol*> arguments;
			Type* returntype;
			parse_functionsignature(lexer, arguments, returntype);

			FunctionSymbol* f = retain(
					new FunctionSymbol(id, arguments, returntype));

			expect_operator(lexer, "=");
			ASTNode* value = parse_expression(lexer);
			ASTFunctionBody* definition = retain(
					new ASTFunctionBody(position, f, value));
			expect_identifier(lexer, "in");
			ASTNode* body = parse_expression(lexer);
			return retain(new ASTDefineFunction(position, f, definition, body));
		}
		else
		{
			/* Variable definition. */

			expect_operator(lexer, "=");
			ASTNode* value = parse_expression(lexer);
			expect_identifier(lexer, "in");
			ASTNode* body = parse_expression(lexer);
			return retain(new ASTDefineVariable(position, id, returntype,
					value, body));
		}
	}

	ASTNode* parse_if(L& lexer)
	{
		Position position = lexer.position();

		expect_identifier(lexer, "if");
		ASTNode* condition = parse_expression(lexer);
		expect_identifier(lexer, "then");
		ASTNode* trueval = parse_expression(lexer);
		expect_identifier(lexer, "else");
		ASTNode* falseval = parse_expression(lexer);

		return retain(new ASTCondition(position, condition, trueval, falseval));
	}

	ASTNode* parse_vector(L& lexer)
	{
		Position position = lexer.position();

		expect(lexer, L::OPENBLOCK);

		if ((lexer.token() == L::OPERATOR) && (lexer.id() == "*"))
		{
			/* Vector splat. */

			lexer.next();
			if (lexer.token() != L::NUMBER)
				lexer.error("invalid vector size");
			int size = (int)lexer.real();
			if ((Real)size != lexer.real())
				lexer.error("n-vector size must be an integer");
			if (size <= 0)
				lexer.error("n-vector size must be greater than 0");

			lexer.next();
			ASTNode* e = parse_expression(lexer);
			expect(lexer, L::CLOSEBLOCK);

			return retain(new ASTVectorSplat(position, e, size));
		}
		else
		{
			/* Ordinary vector literal. */

			vector<ASTNode*> elements;
			do
			{
				ASTNode* e = parse_expression(lexer);
				elements.push_back(e);

				if (lexer.token() != L::COMMA)
					break;
				expect(lexer, L::COMMA);
			}
			while (true);

			expect(lexer, L::CLOSEBLOCK);

			return retain(new ASTVector(position, elements));
		}
	}

	ASTToplevel* parse_toplevel(L& lexer, ToplevelSymbol* toplevel,
			SymbolTable* symboltable)
	{
		Position position = lexer.position();
		ASTNode* body = parse_expression(lexer);
		return retain(new ASTToplevel(position, toplevel, body, symboltable));
	}
};

#endif
