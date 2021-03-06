// ****************************************************************************
//  interpreter.cpp                                              ELFE project
// ****************************************************************************
//
//   File Description:
//
//    An interpreter for ELFE that does not rely on LLVM at all
//
//
//
//
//
//
//
//
// ****************************************************************************
//  (C) 2015 Christophe de Dinechin <christophe@taodyne.com>
//  (C) 2015 Taodyne SAS
// ****************************************************************************

#include "interpreter.h"
#include "gc.h"
#include "info.h"
#include "errors.h"
#include "types.h"
#include "renderer.h"
#include "basics.h"

#include <cmath>
#include <algorithm>


ELFE_BEGIN



// ============================================================================
//
//   Evaluation cache - Recording what has been evaluated and how
//
// ============================================================================

typedef std::map<Tree_p, Tree_p> EvalCache;



// ============================================================================
//
//    Primitive cache for 'opcode' and 'C' bindings
//
// ============================================================================

Opcode *SetInfo(Infix *decl, Opcode *opcode)
// ----------------------------------------------------------------------------
//    Create a new info for the given callback
// ----------------------------------------------------------------------------
{
    decl->right->SetInfo<Opcode>(opcode);
    return opcode;
}


Opcode *OpcodeInfo(Infix *decl)
// ----------------------------------------------------------------------------
//    Check if we have an opcode in the definition
// ----------------------------------------------------------------------------
{
    Tree *right = decl->right;
    Opcode *info = right->GetInfo<Opcode>();
    if (info)
        return info;

    // Check if the declaration is something like 'X -> opcode Foo'
    // If so, lookup 'Foo' in the opcode table the first time to record it
    if (Prefix *prefix = right->AsPrefix())
        if (Name *name = prefix->left->AsName())
            if (name->value == "opcode")
                if (Name *opName = prefix->right->AsName())
                    if (Opcode *opcode = Opcode::Find(prefix, opName->value))
                        return SetInfo(decl, opcode->Clone());

    return NULL;
}



// ============================================================================
//
//    Parameter binding
//
// ============================================================================

struct Bindings
// ----------------------------------------------------------------------------
//   Structure used to record bindings
// ----------------------------------------------------------------------------
{
    typedef bool value_type;

    Bindings(Context *context, Context *locals,
             Tree *test, EvalCache &cache, TreeList &args)
        : context(context), locals(locals),
          test(test), cache(cache), resultType(NULL), args(args) {}

    // Tree::Do interface
    bool  DoInteger(Integer *what);
    bool  DoReal(Real *what);
    bool  DoText(Text *what);
    bool  DoName(Name *what);
    bool  DoPrefix(Prefix *what);
    bool  DoPostfix(Postfix *what);
    bool  DoInfix(Infix *what);
    bool  DoBlock(Block *what);

    // Evaluation and binding of values
    void  MustEvaluate(bool updateContext = false);
    Tree *MustEvaluate(Context *context, Tree *what);
    void  Bind(Name *name, Tree *value);
    void  BindClosure(Name *name, Tree *value);

private:
    Context_p  context;
    Context_p  locals;
    Tree_p     test;
    EvalCache  &cache;

public:
    Tree_p      resultType;
    TreeList   &args;
};


inline bool Bindings::DoInteger(Integer *what)
// ----------------------------------------------------------------------------
//   The pattern contains an integer: check we have the same
// ----------------------------------------------------------------------------
{
    MustEvaluate();
    if (Integer *ival = test->AsInteger())
        if (ival->value == what->value)
            return true;
    Ooops("Integer $1 does not match $2", what, test);
    return false;
}


inline bool Bindings::DoReal(Real *what)
// ----------------------------------------------------------------------------
//   The pattern contains a real: check we have the same
// ----------------------------------------------------------------------------
{
    MustEvaluate();
    if (Real *rval = test->AsReal())
        if (rval->value == what->value)
            return true;
    Ooops("Real $1 does not match $2", what, test);
    return false;
}


inline bool Bindings::DoText(Text *what)
// ----------------------------------------------------------------------------
//   The pattern contains a real: check we have the same
// ----------------------------------------------------------------------------
{
    MustEvaluate();
    if (Text *tval = test->AsText())
        if (tval->value == what->value)         // Do delimiters matter?
            return true;
    Ooops("Text $1 does not match $2", what, test);
    return false;
}


inline bool Bindings::DoName(Name *what)
// ----------------------------------------------------------------------------
//   The pattern contains a name: bind it as a closure, no evaluation
// ----------------------------------------------------------------------------
{
    Save<Context_p> saveContext(context, context);

    // The test value may have been evaluated
    EvalCache::iterator found = cache.find(test);
    if (found != cache.end())
        test = (*found).second;

    // If there is already a binding for that name, value must match
    // This covers both a pattern with 'pi' in it and things like 'X+X'
    if (Tree *bound = locals->Bound(what))
    {
        MustEvaluate(true);
        bool result = Tree::Equal(bound, test);
        IFTRACE(eval)
            std::cerr << "  ARGCHECK: "
                      << test << " vs " << bound
                      << (result ? " MATCH" : " FAILED")
                      << "\n";
        if (!result)
            Ooops("Name $1 does not match $2", bound, test);
        return result;
    }

    IFTRACE(eval)
        std::cerr << "  CLOSURE " << ContextStack(context->CurrentScope())
                  << what << "=" << test << "\n";
    BindClosure(what, test);
    return true;
}


bool Bindings::DoBlock(Block *what)
// ----------------------------------------------------------------------------
//   The pattern contains a block: look inside
// ----------------------------------------------------------------------------
{
    if (Block *testBlock = test->AsBlock())
        if (testBlock->opening == what->opening &&
            testBlock->closing == what->closing)
            test = testBlock->child;

    return what->child->Do(this);
}


bool Bindings::DoPrefix(Prefix *what)
// ----------------------------------------------------------------------------
//   The pattern contains prefix: check that the left part matches
// ----------------------------------------------------------------------------
{
    // The test itself should be a prefix
    if (Prefix *pfx = test->AsPrefix())
    {

        // If we call 'sin X' and match 'sin 3', check if names match
        if (Name *name = what->left->AsName())
        {
            if (Name *testName = pfx->left->AsName())
            {
                if (name->value == testName->value)
                {
                    test = pfx->right;
                    return what->right->Do(this);
                }
                else
                {
                    Ooops("Prefix name $1 does not match $2", name, testName);
                    return false;
                }
            }
        }

        // For other cases, we must go deep inside each prefix to check
        test = pfx->left;
        if (!what->left->Do(this))
            return false;
        test = pfx->right;
        if (what->right->Do(this))
            return true;
    }

    // All other cases are a mismatch
    Ooops("Prefix $1 does not match $2", what, test);
    return false;
}


bool Bindings::DoPostfix(Postfix *what)
// ----------------------------------------------------------------------------
//   The pattern contains posfix: check that the right part matches
// ----------------------------------------------------------------------------
{
    // The test itself should be a postfix
    if (Postfix *pfx = test->AsPostfix())
    {

        // If we call 'X!' and match '3!', check if names match
        if (Name *name = what->right->AsName())
        {
            if (Name *testName = pfx->right->AsName())
            {
                if (name->value == testName->value)
                {
                    test = pfx->left;
                    return what->left->Do(this);
                }
                else
                {
                    Ooops("Postfix name $1 does not match $2", name, testName);
                    return false;
                }
            }
        }

        // For other cases, we must go deep inside each prefix to check
        test = pfx->right;
        if (!what->right->Do(this))
            return false;
        test = pfx->left;
        if (what->left->Do(this))
            return true;
    }

    // All other cases are a mismatch
    Ooops("Postfix $1 does not match $2", what, test);
    return false;
}


bool Bindings::DoInfix(Infix *what)
// ----------------------------------------------------------------------------
//   The complicated case: various declarations
// ----------------------------------------------------------------------------
{
    Save<Context_p> saveContext(context, context);

    // Check if we have typed arguments, e.g. X:integer
    if (what->name == ":")
    {
        Name *name = what->left->AsName();
        if (!name)
        {
            Ooops("Invalid declaration, $1 is not a name", what->left);
            return false;
        }

        // Typed name: evaluate type and check match
        Tree *type = MustEvaluate(context, what->right);
        Tree *checked = TypeCheck(context, type, test);
        if (!checked || type == ELFE::value_type)
        {
            MustEvaluate(type != ELFE::value_type);
            checked = TypeCheck(context, type, test);
        }
        if (checked)
        {
            Bind(name, checked);
            return true;
        }

        // Type mismatch
        Ooops("Type $1 does not contain $2", type, test);
        return false;
    }

    // Check if we have typed declarations, e.g. X+Y as integer
    if (what->name == "as")
    {
        if (resultType)
        {
            Ooops("Duplicate return type declaration $1", what);
            Ooops("Previously declared type was $1", resultType);
        }
        resultType = MustEvaluate(context, what->right);
        return what->left->Do(this);
    }

    // Check if we have a guard clause
    if (what->name == "when")
    {
        // It must pass the rest (need to bind values first)
        if (!what->left->Do(this))
            return false;

        // Here, we need to evaluate in the local context, not eval one
        Tree *check = MustEvaluate(locals, what->right);
        if (check == elfe_true)
            return true;
        else if (check != elfe_false)
            Ooops ("Invalid guard clause, $1 is not a boolean", check);
        else
            Ooops("Guard clause $1 is not verified", what->right);
        return false;
    }

    // In all other cases, we need an infix with matching name
    Infix *ifx = test->AsInfix();
    if (!ifx)
    {
        // Try to get an infix by evaluating what we have
        MustEvaluate(true);
        ifx = test->AsInfix();
    }
    if (ifx)
    {
        if (ifx->name != what->name)
        {
            Ooops("Infix names $1 and $2 don't match", what->Position())
                .Arg(ifx->name).Arg(what->name);
            return false;
        }

        test = ifx->left;
        if (!what->left->Do(this))
            return false;
        test = ifx->right;
        if (what->right->Do(this))
            return true;
    }

    // Mismatch
    Ooops("Infix $1 does not match $2", what, test);
    return false;
}


void Bindings::MustEvaluate(bool updateContext)
// ----------------------------------------------------------------------------
//   Evaluate 'test', ensuring that each bound arg is evaluated at most once
// ----------------------------------------------------------------------------
{
    Tree *evaluated = cache[test];
    if (!evaluated)
    {
        evaluated = EvaluateClosure(context, test);
        cache[test] = evaluated;
        IFTRACE(eval)
            std::cerr << "  TEST(" << test << ") = "
                      << "NEW(" << evaluated << ")\n";
    }
    else
    {
        IFTRACE(eval)
            std::cerr << "  TEST(" << test << ") = "
                      << "OLD(" << evaluated << ")\n";
    }

    test = evaluated;
    if (updateContext)
        if (Tree *inside = IsClosure(test, &context))
            test = inside;

}


Tree *Bindings::MustEvaluate(Context *context, Tree *tval)
// ----------------------------------------------------------------------------
//   Ensure that each bound arg is evaluated at most once
// ----------------------------------------------------------------------------
{
    Tree *evaluated = cache[tval];
    if (!evaluated)
    {
        evaluated = EvaluateClosure(context, tval);
        cache[tval] = evaluated;
        IFTRACE(eval)
            std::cerr << "  NEED(" << tval << ") = "
                      << "NEW(" << evaluated << ")\n";
    }
    else
    {
        IFTRACE(eval)
            std::cerr << "  NEED(" << tval << ") = "
                      << "OLD(" << evaluated << ")\n";
    }
    if (Tree *inside = IsClosure(evaluated, NULL))
        evaluated = inside;
    return evaluated;
}


void Bindings::Bind(Name *name, Tree *value)
// ----------------------------------------------------------------------------
//   Enter a new binding in the current context, remember left and right
// ----------------------------------------------------------------------------
{
    IFTRACE(eval)
        std::cerr << "  BIND " << name << "=" << ShortTreeForm(value) <<"\n";
    args.push_back(value);
    locals->Define(name, value);
}


void Bindings::BindClosure(Name *name, Tree *value)
// ----------------------------------------------------------------------------
//   Enter a new binding in the current context, preserving its environment
// ----------------------------------------------------------------------------
{
    value = MakeClosure(context, value);
    Bind(name, value);
}



// ============================================================================
//
//   Main evaluation loop for the interpreter
//
// ============================================================================

static Tree *error_result = NULL;


static Tree *evalLookup(Scope *evalScope, Scope *declScope,
                        Tree *self, Infix *decl, void *ec)
// ----------------------------------------------------------------------------
//   Calllback function to check if the candidate matches
// ----------------------------------------------------------------------------
{
    static uint depth = 0;
    Save<uint> saveDepth(depth, depth+1);
    IFTRACE(eval)
        std::cerr << "EVAL" << depth << "(" << self
                  << ") from " << decl->left << "\n";
    if (depth > MAIN->options.stack_depth)
    {
        Ooops("Stack depth exceeded evaluating $1", self);
        return error_result = elfe_error;
    }
    else if (error_result)
    {
        return error_result;
    }

    // Create the scope for evaluation
    Context_p context = new Context(evalScope);
    Context_p locals  = NULL;
    Tree *result = NULL;

    // Check if the decl is an opcode or C binding
    Errors *errors = MAIN->errors;
    uint errCount = errors->Count();
    Opcode *opcode = OpcodeInfo(decl);
    if (errors->Count() != errCount)
        return NULL;

    // If we lookup a name or a number, just return it
    Tree *defined = RewriteDefined(decl->left);
    Tree *resultType = tree_type;
    TreeList args;
    if (defined->IsLeaf())
    {
        // Must match literally, or we don't have a candidate
        if (!Tree::Equal(defined, self))
        {
            IFTRACE(eval)
                std::cerr << "EVAL" << depth << "(" << self
                          << ") from constant " << decl->left
                          << " MISMATCH\n";
            return NULL;
        }
        locals = context;
    }
    else
    {
        // Retrieve the evaluation cache for arguments
        EvalCache *cache = (EvalCache *) ec;

        // Create the scope for evaluation and local bindings
        locals = new Context(declScope);
        locals->CreateScope();

        // Check bindings of arguments to declaration, exit if fails
        Bindings  bindings(context, locals, self, *cache, args);
        if (!decl->left->Do(bindings))
        {
            IFTRACE(eval)
                std::cerr << "EVAL" << depth << "(" << self
                          << ") from " << decl->left
                          << " MISMATCH\n";
            return NULL;
        }
        if (bindings.resultType)
            resultType = bindings.resultType;
    }

    // Check if the right is "self"
    if (result == elfe_self)
    {
        IFTRACE(eval)
            std::cerr << "EVAL" << depth << "(" << self
                      << ") from " << decl->left
                      << " SELF\n";
        return self;
    }

    // Check if we have builtins (opcode or C bindings)
    if (opcode)
    {
        // Cached callback
        uint offset = args.size();
        std::reverse(args.begin(), args.end());
        args.push_back(decl->right);
        args.push_back(context->CurrentScope());
        Data data = &args[offset];
        opcode->Run(data);
        result = DataResult(data);
        IFTRACE(eval)
            std::cerr << "EVAL" << depth << "(" << self
                      << ") OPCODE " << opcode->OpID()
                      << "(" << args << ") = "
                      << result << "\n";
        return result;
    }

    // Normal case: evaluate body of the declaration in the new context
    result = decl->right;
    if (resultType != tree_type)
        result = new Infix("as", result, resultType, self->Position());

    result = MakeClosure(locals, result);
    IFTRACE(eval)
        std::cerr << "EVAL" << depth << " BINDINGS: "
                  << ContextStack(locals->CurrentScope())
                  << "\n" << locals << "\n"
                  << "EVAL(" << self
                  << ") = (" << result << ")\n";
    return result;
}


inline Tree *encloseResult(Context *context, Scope *old, Tree *what)
// ----------------------------------------------------------------------------
//   Encapsulate result with a closure if context is not evaluation context
// ----------------------------------------------------------------------------
{
    if (context->CurrentScope() != old)
        what = MakeClosure(context, what);
    return what;
}


static Tree *Instructions(Context_p context, Tree_p what)
// ----------------------------------------------------------------------------
//   Evaluate the input tree once declarations have been processed
// ----------------------------------------------------------------------------
{
    Tree_p      result = what;
    Scope_p     originalScope = context->CurrentScope();

    // Loop to avoid recursion for a few common cases, e.g. sequences, blocks
    while (what)
    {
        // First attempt to look things up
        EvalCache cache;
        if (Tree *eval = context->Lookup(what, evalLookup, &cache))
        {
            if (eval == elfe_error)
                return eval;
            MAIN->errors->Clear();
            result = eval;
            if (Tree *inside = IsClosure(eval, &context))
            {
                what = inside;
                continue;
            }
            return encloseResult(context, originalScope, eval);
        }

        kind whatK = what->Kind();
        switch (whatK)
        {
        case INTEGER:
        case REAL:
        case TEXT:
            return what;

        case NAME:
            Ooops("No name matches $1", what);
            return encloseResult(context, originalScope, what);

        case BLOCK:
        {
            // Evaluate child in a new context
            context->CreateScope();
            what = ((Block *) (Tree *) what)->child;
            bool hasInstructions = context->ProcessDeclarations(what);
            if (context->IsEmpty())
                context->PopScope();
            if (hasInstructions)
                continue;
            return encloseResult(context, originalScope, what);
        }

        case PREFIX:
        {
            // If we have a prefix on the left, check if it's a closure
            if (Tree *closed = IsClosure(what, &context))
            {
                what = closed;
                continue;
            }

            // If we have a name on the left, lookup name and start again
            Prefix *pfx = (Prefix *) (Tree *) what;
            Tree   *callee = pfx->left;

            // Check if we had something like '(X->X+1) 31' as closure
            Context_p calleeContext = NULL;
            if (Tree *inside = IsClosure(callee, &calleeContext))
                callee = inside;

            if (Name *name = callee->AsName())
                // A few cases where we don't interpret the result
                if (name->value == "type"   ||
                    name->value == "extern" ||
                    name->value == "data")
                    return what;

            // This variable records if we evaluated the callee
            Tree *newCallee = NULL;
            Tree *arg = pfx->right;

            // If we have an infix on the left, check if it's a single rewrite
            if (Infix *lifx = callee->AsInfix())
            {
                // Check if we have a function definition
                if (lifx->name == "->")
                {
                    // If we have a single name on the left, like (X->X+1)
                    // interpret that as a lambda function
                    if (Name *lfname = lifx->left->AsName())
                    {
                        // Case like '(X->X+1) Arg':
                        // Bind arg in new context and evaluate body
                        context = new Context(context);
                        context->Define(lfname, arg);
                        what = lifx->right;
                        continue;
                    }

                    // Otherwise, enter declaration and retry, e.g.
                    // '(X,Y->X+Y) (2,3)' should evaluate as 5
                    context = new Context(context);
                    context->Define(lifx->left, lifx->right);
                    what = arg;
                    continue;
                }
            }

            // Other cases: evaluate the callee, and if it changed, retry
            if (!newCallee)
            {
                Context_p newContext = new Context(context);
                newCallee = EvaluateClosure(newContext, callee);
            }

            if (newCallee != callee)
            {
                // We need to evaluate argument in current context
                arg = Instructions(context, arg);

                // We built a new context if left was a block
                if (Tree *inside = IsClosure(newCallee, &context))
                {
                    what = arg;
                    // Check if we have a single definition on the left
                    if (Infix *ifx = inside->AsInfix())
                        if (ifx->name == "->")
                            what = new Prefix(newCallee, arg, pfx->Position());
                }
                else
                {
                    // Other more regular cases
                    what = new Prefix(newCallee, arg, pfx->Position());
                }
                continue;
            }

            // If we get there, we didn't find anything interesting to do
            Ooops("No prefix matches $1", what);
            return encloseResult(context, originalScope, what);
        }

        case POSTFIX:
        {
            // Check if there is a form that matches
            Ooops("No postifx matches $1", what);
            return encloseResult(context, originalScope, what);
        }

        case INFIX:
        {
            Infix *infix = (Infix *) (Tree *) what;
            text name = infix->name;

            // Check sequences
            if (name == ";" || name == "\n")
            {
                // Sequences: evaluate left, then right
                Context *leftContext = context;
                Tree *left = Instructions(leftContext, infix->left);
                if (left != infix->left)
                    result = left;
                what = infix->right;
                continue;
            }

            // Check declarations
            if (name == "->")
            {
                // Declarations evaluate last non-declaration result, or self
                return encloseResult(context, originalScope, result);
            }

            // Check type matching
            if (name == "as")
            {
                result = TypeCheck(context, infix->right, infix->left);
                if (!result)
                {
                    Ooops("Value $1 does not match type $2",
                          infix->left, infix->right);
                    result = infix->left;
                }
                return encloseResult(context, originalScope, result);
            }

            // Check scoped reference
            if (name == ".")
            {
                Tree *left = Instructions(context, infix->left);
                IsClosure(left, &context);
                what = infix->right;
                continue;
            }

            // All other cases: failure
            Ooops("No infix matches $1", what);
            return encloseResult(context, originalScope, what);
        }
        } // switch
    }// while(what)

    return encloseResult(context, originalScope, result);
}


Tree *EvaluateClosure(Context *context, Tree *what)
// ----------------------------------------------------------------------------
//    Evaluate 'what', possibly returned as a closure in case not in 'context'
// ----------------------------------------------------------------------------
{
    // Create scope for declarations, and evaluate in this context
    Tree_p result = what;
    Errors *errors = MAIN->errors;
    uint errCount = errors->Count();
    if (context->ProcessDeclarations(what) && errCount == errors->Count())
        result = Instructions(context, what);

    // This is a safe point for checking collection status
    GarbageCollector::SafePoint();

    return result;
}



// ============================================================================
//
//     Type checking
//
// ============================================================================

struct Expansion
// ----------------------------------------------------------------------------
//   A structure to expand a type-matched structure
// ----------------------------------------------------------------------------
{
    Expansion(Context *context): context(context) {}

    typedef Tree *value_type;

    Tree *  DoInteger(Integer *what)
    {
        return what;
    }
    Tree *  DoReal(Real *what)
    {
        return what;
    }
    Tree *  DoText(Text *what)
    {
        return what;
    }
    Tree *  DoName(Name *what)
    {
        if (Tree *bound = context->Bound(what))
        {
            if (Tree *eval = IsClosure(bound, NULL))
                bound = eval;
            return bound;
        }
        return what;
    }
    Tree *  DoPrefix(Prefix *what)
    {
        Tree *left  = what->left->Do(this);
        Tree *right = what->right->Do(this);
        if (left != what->left || right != what->right)
            return new Prefix(left, right, what->Position());
        return what;
    }
    Tree *  DoPostfix(Postfix *what)
    {
        Tree *left  = what->left->Do(this);
        Tree *right = what->right->Do(this);
        if (left != what->left || right != what->right)
            return new Postfix(left, right, what->Position());
        return what;

    }
    Tree *  DoInfix(Infix *what)
    {
        if (what->name == ":" || what->name == "as" || what->name == "when")
            return what->left->Do(this);
        Tree *left  = what->left->Do(this);
        Tree *right = what->right->Do(this);
        if (left != what->left || right != what->right)
            return new Infix(what->name, left, right, what->Position());
        return what;
    }

    Tree *  DoBlock(Block *what)
    {
        Tree *chld = what->child->Do(this);
        if (chld != what->child)
            return new Block(chld,what->opening,what->closing,what->Position());
        return what;
    }

    Context_p context;
};


static Tree *formTypeCheck(Context *context, Tree *shape, Tree *value)
// ----------------------------------------------------------------------------
//    Check a value against a type shape
// ----------------------------------------------------------------------------
{
    // Strip outermost block if there is one
    if (Block *block = shape->AsBlock())
        shape = block->child;

    // Check if the shape matches
    Context_p locals = new Context(context);
    EvalCache cache;
    TreeList  args;
    Bindings  bindings(context, locals, value, cache, args);
    if (!shape->Do(bindings))
    {
        IFTRACE(eval)
            std::cerr << "TYPECHECK: shape mismatch for " << value << "\n";
        return NULL;
    }

    // Reconstruct the resulting value from the shape
    Expansion expand(locals);
    value = shape->Do(expand);

    // The value is associated to the symbols we extracted
    IFTRACE(eval)
        std::cerr << "TYPECHECK: shape match for " << value << "\n";
    return MakeClosure(locals, value);
}


Tree *TypeCheck(Context *scope, Tree *type, Tree *value)
// ----------------------------------------------------------------------------
//   Check if 'value' matches 'type' in the given context
// ----------------------------------------------------------------------------
{
    IFTRACE(eval)
        std::cerr << "TYPECHECK " << value << " against " << type << "\n";

    // Accelerated type check for the builtin or constructed types
    if (TypeCheckOpcode *builtin = type->GetInfo<TypeCheckOpcode>())
    {
        // If this is marked as builtin, check if the test passes
        if (Tree *converted = builtin->Check(scope, value))
        {
            IFTRACE(eval)
                std::cerr << "TYPECHECK " << value
                          << " as " << converted << "\n";
            return converted;
        }
    }
    else
    {
        // Check a type like 'type (X, Y)'
        if (Prefix *ptype = type->AsPrefix())
            if (Name *ptypename = ptype->left->AsName())
                if (ptypename->value == "type")
                    return formTypeCheck(scope, ptype->right, value);

        IFTRACE(eval)
            std::cerr << "TYPECHECK: no code for " << type
                      << " opcode is " << type->GetInfo<Opcode>()
                      << "\n";
    }


    // No direct or converted match, end of game
    IFTRACE(eval)
        std::cerr << "TYPECHECK " << value << " FAILED\n";
    return NULL;
}



// ============================================================================
//
//    Include the opcodes
//
// ============================================================================

#include "opcodes.h"
#include "interpreter.tbl"

ELFE_END
