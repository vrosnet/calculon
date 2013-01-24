#ifndef CALCULON_AST_H
#define CALCULON_AST_H

#ifndef CALCULON_H
#error "Don't include this, include calculon.h instead."
#endif

class ASTFrame;
class ASTFunction;

struct ASTNode : public Object
{
	ASTNode* parent;
	Position position;

	ASTNode(const Position& position):
		parent(NULL),
		position(position)
	{
	}

	virtual ~ASTNode()
	{
	}

	virtual llvm::Value* codegen(Compiler& compiler) = 0;

	llvm::Value* codegen_to_real(Compiler& compiler)
	{
		llvm::Value* v = codegen(compiler);

		if (v->getType() != compiler.realType)
			throw TypeException("type mismatch: expected a real", this);
		return v;
	}

	llvm::Value* codegen_to_vector(Compiler& compiler)
	{
		llvm::Value* v = codegen(compiler);

		if (v->getType() != compiler.vectorType)
			throw TypeException("type mismatch: expected a vector", this);
		return v;
	}

	virtual void resolveVariables(Compiler& compiler)
	{
	}

	virtual ASTFrame* getFrame()
	{
		return parent->getFrame();
	}
};

struct ASTConstant : public ASTNode
{
	Real value;

	ASTConstant(const Position& position, Real value):
		ASTNode(position),
		value(value)
	{
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		return llvm::ConstantFP::get(compiler.context,
				llvm::APFloat(value));
	}
};

struct ASTVariable : public ASTNode
{
	string id;
	ValuedSymbol* symbol;

	using ASTNode::getFrame;
	using ASTNode::position;

	ASTVariable(const Position& position, const string& id):
		ASTNode(position),
		id(id), symbol(NULL)
	{
	}

	void resolveVariables(Compiler& compiler)
	{
		SymbolTable* symbolTable = getFrame()->symbolTable;
		Symbol* s = symbolTable->resolve(id);
		if (!s)
			throw SymbolException(id, this);
		symbol = s->isValued();
		if (!symbol)
		{
			std::stringstream s;
			s << "attempt to get the value of '" << id << "', which is not a variable";
			throw CompilationException(position.formatError(s.str()));
		}
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		return symbol->value;
	}
};

struct ASTVector : public ASTNode
{
	ASTNode* x;
	ASTNode* y;
	ASTNode* z;

	ASTVector(const Position& position, ASTNode* x, ASTNode* y, ASTNode* z):
		ASTNode(position),
		x(x), y(y), z(z)
	{
		x->parent = y->parent = z->parent = this;
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = llvm::UndefValue::get(compiler.vectorType);

		llvm::Value* xv = x->codegen_to_real(compiler);
		llvm::Value* yv = y->codegen_to_real(compiler);
		llvm::Value* zv = z->codegen_to_real(compiler);

		v = compiler.builder.CreateInsertElement(v, xv, compiler.xindex);
		v = compiler.builder.CreateInsertElement(v, yv, compiler.yindex);
		v = compiler.builder.CreateInsertElement(v, zv, compiler.zindex);

		return v;
	}

	void resolveVariables(Compiler& compiler)
	{
		x->resolveVariables(compiler);
		y->resolveVariables(compiler);
		z->resolveVariables(compiler);
	}
};

struct ASTExtract : public ASTNode
{
	ASTNode* value;
	string id;

	ASTExtract(const Position& position, ASTNode* value, const string& id):
		ASTNode(position),
		value(value), id(id)
	{
		value->parent = this;
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = value->codegen_to_vector(compiler);

		llvm::Value* element = NULL;
		if (id == "x")
			element = compiler.xindex;
		else if (id == "y")
			element = compiler.yindex;
		else if (id == "z")
			element = compiler.zindex;
		assert(element);

		return compiler.builder.CreateExtractElement(v, element);
	}

	void resolveVariables(Compiler& compiler)
	{
		value->resolveVariables(compiler);
	}
};

struct ASTUnary : public ASTNode
{
	string id;
	ASTNode* value;

	ASTUnary(const Position& position, const string& id, ASTNode* value):
		ASTNode(position),
		id(id), value(value)
	{
		value->parent = this;
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = value->codegen(compiler);
		char type = compiler.llvmToType(v->getType());

		if (id == "-")
		{
			switch (type)
			{
				case REAL:
				case VECTOR:
					v = compiler.builder.CreateFNeg(v);
					break;

				default:
					throw IntrinsicTypeException(id, type, this);
			}
		}
		else
			throw SymbolException(id, this);

		return v;
	}

	void resolveVariables(Compiler& compiler)
	{
		value->resolveVariables(compiler);
	}
};

struct ASTFrame : public ASTNode
{
	SymbolTable* symbolTable;

	using ASTNode::parent;

	ASTFrame(const Position& position):
		ASTNode(position),
		symbolTable(NULL)
	{
	}

	ASTFrame* getFrame()
	{
		return this;
	}
};

struct ASTDefineVariable : public ASTFrame
{
	string id;
	char type;
	ASTNode* value;
	ASTNode* body;

	VariableSymbol* _symbol;
	using ASTFrame::symbolTable;
	using ASTNode::parent;

	ASTDefineVariable(const Position& position,
			const string& id, char type,
			ASTNode* value, ASTNode* body):
		ASTFrame(position),
		id(id), value(value), body(body),
		_symbol(NULL)
	{
		body->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		symbolTable = compiler.retain(new SingletonSymbolTable(
				parent->getFrame()->symbolTable));
		_symbol = compiler.retain(new VariableSymbol(id, type));
		symbolTable->add(_symbol);

		value->parent = parent;
		value->resolveVariables(compiler);
		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		llvm::Value* v = value->codegen(compiler);
		_symbol->value = v;

		return body->codegen(compiler);
	}
};

struct ASTFunctionBody : public ASTFrame
{
	FunctionSymbol* function;
	ASTNode* body;

	using ASTNode::parent;
	using ASTFrame::symbolTable;

	ASTFunctionBody(const Position& position,
			FunctionSymbol* function, ASTNode* body):
		ASTFrame(position),
		function(function), body(body)
	{
		body->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		if (!symbolTable)
			symbolTable = compiler.retain(new MultipleSymbolTable(
				parent->getFrame()->symbolTable));

		const vector<VariableSymbol*>& arguments = function->arguments;
		for (typename vector<VariableSymbol*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			symbolTable->add(*i);
		}

		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		/* Create the LLVM function type. */

		const vector<VariableSymbol*>& arguments = function->arguments;
		vector<llvm::Type*> llvmtypes;
		for (int i=0; i<arguments.size(); i++)
		{
			VariableSymbol* symbol = arguments[i];
			llvmtypes.push_back(compiler.getInternalType(symbol->type));
		}

		llvm::Type* returntype = compiler.getInternalType(function->returntype);
		llvm::FunctionType* ft = llvm::FunctionType::get(
				returntype, llvmtypes, false);

		/* Create the function. */

		llvm::Function* f = llvm::Function::Create(ft,
				llvm::Function::InternalLinkage,
				function->name, compiler.module);
		function->function = f;

		/* Bind the argument symbols to their LLVM values. */

		{
			int i = 0;
			for (llvm::Function::arg_iterator ii = f->arg_begin(),
					ee = f->arg_end(); ii != ee; ii++)
			{
				llvm::Value* v = ii;
				VariableSymbol* symbol = arguments[i];

				v->setName(symbol->name);
				symbol->value = v;
				i++;
			}
		}

		/* Generate the code. */

		llvm::BasicBlock* toplevel = llvm::BasicBlock::Create(
				compiler.context, "", f);

		llvm::BasicBlock* bb = compiler.builder.GetInsertBlock();
		llvm::BasicBlock::iterator bi = compiler.builder.GetInsertPoint();
		compiler.builder.SetInsertPoint(toplevel);

		llvm::Value* v = body->codegen(compiler);
		compiler.builder.CreateRet(v);
		if (v->getType() != returntype)
			throw TypeException(
					"function does not return the type it's declared to return", this);

		compiler.builder.SetInsertPoint(bb, bi);

		return f;
	}
};

struct ASTDefineFunction : public ASTFrame
{
	FunctionSymbol* function;
	ASTNode* definition;
	ASTNode* body;

	using ASTNode::parent;
	using ASTFrame::symbolTable;

	ASTDefineFunction(const Position& position, FunctionSymbol* function,
			ASTNode* definition, ASTNode* body):
		ASTFrame(position),
		function(function), definition(definition), body(body)
	{
		definition->parent = body->parent = this;
	}

	void resolveVariables(Compiler& compiler)
	{
		symbolTable = compiler.retain(new SingletonSymbolTable(
				parent->getFrame()->symbolTable));
		symbolTable->add(function);

		definition->resolveVariables(compiler);
		body->resolveVariables(compiler);
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		definition->codegen(compiler);
		return body->codegen(compiler);
	}
};

struct ASTFunctionCall : public ASTNode
{
	string id;
	vector<ASTNode*> arguments;
	CallableSymbol* function;

	using ASTNode::position;
	using ASTNode::getFrame;

	ASTFunctionCall(const Position& position, const string& id,
			const vector<ASTNode*>& arguments):
		ASTNode(position),
		id(id), arguments(arguments),
		function(NULL)
	{
		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			(*i)->parent = this;
		}
	}

	void resolveVariables(Compiler& compiler)
	{
		Symbol* symbol = getFrame()->symbolTable->resolve(id);
		if (!symbol)
			throw SymbolException(id, this);
		function = symbol->isCallable();
		if (!function)
		{
			std::stringstream s;
			s << "attempt to call '" << id << "', which is not a function";
			throw CompilationException(position.formatError(s.str()));
		}

		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			(*i)->resolveVariables(compiler);
		}
	}

	llvm::Value* codegen(Compiler& compiler)
	{
		vector<llvm::Value*> parameters;
		for (typename vector<ASTNode*>::const_iterator i = arguments.begin(),
				e = arguments.end(); i != e; i++)
		{
			llvm::Value* v = (*i)->codegen(compiler);
			parameters.push_back(v);
		}

		compiler.position = position;
		return function->emitCall(compiler, parameters);
	}
};

struct ASTToplevel : public ASTFunctionBody
{
	using ASTFrame::symbolTable;

	ASTToplevel(const Position& position, FunctionSymbol* symbol,
			ASTNode* body, SymbolTable* st):
		ASTFunctionBody(position, symbol, body)
	{
		symbolTable = st;
	}
};

#endif