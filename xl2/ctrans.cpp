// ****************************************************************************
//  ctrans.cpp                      (C) 1992-2003 Christophe de Dinechin (ddd) 
//                                                                 XL2 project 
// ****************************************************************************
// 
//   File Description:
// 
//     XL to C translator
// 
//     This is a very bare-bones translator, designed only to allow
//     low-cost bootstrap of the XL compiler.
// 
//     We care here only about the subset of XL that has the
//     same semantics than C, or semantics that "look and feel" like C
// 
// 
// ****************************************************************************
// This document is confidential.
// Do not redistribute without written permission
// ****************************************************************************
// * File       : $RCSFile$
// * Revision   : $Revision$
// * Date       : $Date$
// ****************************************************************************

#include "ctrans.h"
#include "tree.h"
#include "parser.h"
#include <stdio.h>
#include <ctype.h>
#include <map>
#include <iostream>


// ============================================================================
// 
//    Typedefs and globals
// 
// ============================================================================

struct XL2CTranslation;
typedef void (*infix_fn) (XLInfix *);
typedef void (*prefix_fn) (XLPrefix *);
typedef std::map<text, text>            name_map;
typedef std::map<text, infix_fn>        infix_map;
typedef std::map<text, prefix_fn>       prefix_map;

name_map        XLNames;
name_map        XLModules;
prefix_map      XLUnaryOps;
infix_map       XLBinaryOps;
int in_parameter_declaration = 0;
int in_procedure = 0;
int in_namespace = 0;
text in_struct = "";
text in_typedef = "";
text in_enum = "";


#define NAME(x,y)
#define SYMBOL(x,y)
#define INFIX(x)        extern void infix_##x(XLInfix *);
#define PREFIX(x)       extern void prefix_##x(XLPrefix *);
#define BINARY(x,y)     extern void infix_##y(XLInfix *);
#define UNARY(x,y)      extern void prefix_##y(XLPrefix *);
#include "ctrans.tbl"

#define out     (std::cout)



// ============================================================================
// 
//   Initialization
// 
// ============================================================================

void XLInitCTrans()
// ----------------------------------------------------------------------------
//    Initialize the translator
// ----------------------------------------------------------------------------
{
#define NAME(x,y)       XLNames[#x] = #y;
#define SYMBOL(x,y)     XLNames[x] = y;
#define INFIX(x)        XLBinaryOps[#x] = infix_##x;
#define PREFIX(x)       XLUnaryOps[#x] = prefix_##x;
#define BINARY(x,y)     XLBinaryOps[x] = infix_##y;
#define UNARY(x,y)      XLUnaryOps[x] = prefix_##y;
#include "ctrans.tbl"

    out << "#include \"xl_lib.h\"\n";
}



// ============================================================================
// 
//   Translation engine
// 
// ============================================================================

text XLNormalize (text name)
// ----------------------------------------------------------------------------
//   Return the normalized version of the text
// ----------------------------------------------------------------------------
{
    int i, max = name.length();
    text result = "";
    for (i = 0; i < max; i++)
        if (name[i] != '_')
            result += tolower(name[i]);
    return result;
}


text XLModuleName(XLTree *tree)
// ----------------------------------------------------------------------------
//   Change a tree into a module name
// ----------------------------------------------------------------------------
{
    XLName *name = dynamic_cast<XLName *> (tree);
    if (name)
    {
        text modname = XLNormalize(name->value);
        if (!XLModules.count(modname))
            XLModules[modname] = modname;
        return modname;
    }
    XLInfix *infix = dynamic_cast<XLInfix *> (tree);
    if (infix && infix->name == text("."))
    {
        text result = XLModuleName(infix->left);
        result += ".";
        result += XLModuleName(infix->right);
        return result;
    }
    return "???";
}


struct XL2CTranslation : XLAction
// ----------------------------------------------------------------------------
//   The default action indicates there is more to do...
// ----------------------------------------------------------------------------
{
    XL2CTranslation() {}

    // Override these...
    bool Integer(XLInteger *input);
    bool Real(XLReal *input);
    bool String(XLString *input);
    bool Name(XLName *input);
    bool Block(XLBlock *input);
    bool Prefix(XLPrefix *input);
    bool Infix(XLInfix *input);
    bool Builtin(XLBuiltin *input);
};


bool XL2CTranslation::Integer(XLInteger *input)
// ----------------------------------------------------------------------------
//    Translation of an integer...
// ----------------------------------------------------------------------------
{
    out << input->value;
}


bool XL2CTranslation::Real(XLReal *input)
// ----------------------------------------------------------------------------
//   Translation of a real number
// ----------------------------------------------------------------------------
{
    out << input->value;
}


bool XL2CTranslation::String(XLString *input)
// ----------------------------------------------------------------------------
//    Translation of a string
// ----------------------------------------------------------------------------
{
    out << input->quote << input->value << input->quote;
}


bool XL2CTranslation::Name(XLName *input)
// ----------------------------------------------------------------------------
//    Translation of a name
// ----------------------------------------------------------------------------
{
    text name = XLNormalize(input->value);
    if (XLNames.count(name))
        out << XLNames[name];
    else
        out << name;
}


bool XL2CTranslation::Block(XLBlock *input)
// ----------------------------------------------------------------------------
//    Translate a block
// ----------------------------------------------------------------------------
{
    switch(input->opening)
    {
    case '\t':
        out << "{\n";
        XL2C(input->child);
        if (in_enum.length())
            out << "}\n";
        else
            out << ";\n}\n";
        break;
    default:
        out << input->opening;
        XL2C(input->child);
        out << input->closing;
        break;
    }        
}


bool XL2CTranslation::Prefix(XLPrefix *input)
// ----------------------------------------------------------------------------
//    Translate a prefix function or operator
// ----------------------------------------------------------------------------
{
    XLName *name = dynamic_cast<XLName *> (input->left);
    text nname;
    if (name && XLUnaryOps.count(nname = XLNormalize(name->value)))
    {
        prefix_fn unary = XLUnaryOps[nname];
        unary(input);
    }
    else
    {
        XL2C(input->left);
        int has_paren = input->right->Kind() == xlBLOCK;
        if (!has_paren)
            out << "(";
        XL2C(input->right);
        if (!has_paren)
            out << ")";
    }
}


bool XL2CTranslation::Infix(XLInfix *input)
// ----------------------------------------------------------------------------
//    Translate infix operators or functions
// ----------------------------------------------------------------------------
{
    text nname = XLNormalize(input->name);
    if (XLBinaryOps.count(nname))
    {
        infix_fn binary = XLBinaryOps[nname];
        binary(input);
    }
    else
    {
        out << '(';
        XL2C(input->left);
        if (XLNames.count(nname))
            out << ' ' << XLNames[nname] << ' ';
        else
            out << ' ' << nname << ' ';
        XL2C(input->right);
        out << ')';
    }
}


bool XL2CTranslation::Builtin(XLBuiltin *input)
// ----------------------------------------------------------------------------
//    This should not happen
// ----------------------------------------------------------------------------
{
    out << "*** ERROR\n";
}


void XL2C(XLTree *tree)
// ----------------------------------------------------------------------------
//   Take an XL tree as input, and emit the corresponding C code
// ----------------------------------------------------------------------------
{
    XL2CTranslation translator;
    XLDo<XL2CTranslation> run(translator);
    run (tree);
}



// ****************************************************************************
// 
//   Implementation of the various callbacks
// 
// ****************************************************************************

#define PREFIX(x)       void prefix_##x(XLPrefix *tree)
#define INFIX(x)        void infix_##x(XLInfix *tree)


// ============================================================================
// 
//   Declarations
// 
// ============================================================================

template<class T> struct ScopeSaver
// ----------------------------------------------------------------------------
//   Temporarily save the value of a variable
// ----------------------------------------------------------------------------
{
    ScopeSaver(T&what, T newVal) : saved(what), value(what) { what = newVal; }
    ~ScopeSaver() { saved = value; }
    T & saved;
    T value;
};
typedef ScopeSaver<int>  IntSaver;
typedef ScopeSaver<text> TextSaver;


PREFIX(import)
// ----------------------------------------------------------------------------
//   Process an 'import' file
// ----------------------------------------------------------------------------
{
    text imported;
    XLInfix *infix = dynamic_cast<XLInfix *> (tree->right);
    if (infix && infix->name == text("="))
    {
        text alias_name = XLModuleName(infix->left);
        text cplusified = "";
        imported = XLModuleName(infix->right);
        int i, max = imported.length();
        for (i = 0; i < max; i++)
        {
            char c = imported[i];
            if (c == '.')
                cplusified += "::";
            else
                cplusified += c;
        }
        XLModules[alias_name] = cplusified;
    }
    else
    {
        imported = XLModuleName(tree->right);
    }

    text interface = imported + ".xs";
    text body = imported + ".xl";

    FILE *iface_file = fopen(interface.c_str(), "r");
    if (iface_file)
    {
        fclose(iface_file);
        out << "\n/* " << interface << "*/\n";
        XLParser iface_parser (interface.c_str(), &gContext);
        XLTree *iface_tree = iface_parser.Parse();
        XL2C(iface_tree);
        out << ";\n";
    }

    FILE *body_file = fopen(body.c_str(), "r");
    if (body_file)
    {
        fclose(body_file);
        out << "\n/* " << body << "*/\n";
        XLParser body_parser (body.c_str(), &gContext);
        XLTree *body_tree = body_parser.Parse();
        XL2C(body_tree);
        out << ";\n";
    }

    if (!iface_file && !body_file)
        out << "??? NO FILE FOR '" << imported << "'\n";
}


void XLModule2Namespace(XLTree *tree)
// ----------------------------------------------------------------------------
//   Convert an XL module name to a sequence of namespaces
// ----------------------------------------------------------------------------
{
    XLName *name = dynamic_cast<XLName *> (tree);
    if (name)
    {
        text modname = XLNormalize(name->value);
        out << "namespace " << modname << "\n";
        if (!XLModules.count(modname))
            XLModules[modname] = modname;
    }
    else
    {
        XLInfix *infix = dynamic_cast<XLInfix *> (tree);
        if (infix && infix->name == text("."))
        {
            XLModule2Namespace(infix->left);
            out << " { ";
            in_namespace++;
            XLModule2Namespace(infix->right);
        }
    }
}


PREFIX(module)
// ----------------------------------------------------------------------------
//   If we find a module, insert a "namespace" declaration
// ----------------------------------------------------------------------------
{
    XLInfix *is_tree = dynamic_cast<XLInfix *> (tree->right);
    if (is_tree && is_tree->name == text("is"))
    {
        XLModule2Namespace(is_tree->left);
        XL2C(is_tree->right);        
    }
    else
    {
        XLPrefix *declare = dynamic_cast<XLPrefix *> (tree->right);
        if (declare)
        {
            XLModule2Namespace(declare->left);
            XL2C(declare->right);
        }
        else
        {
            XLModule2Namespace(tree->right);
            out << "{}";
        }
    }
    while (in_namespace)
    {
        out << "}\n";
        in_namespace--;
    }
}


int XLNamespaceScope(XLTree *tree)
// ----------------------------------------------------------------------------
//   Emit a tree that may correspond to a namespace reference
// ----------------------------------------------------------------------------
{
    XLName *name = dynamic_cast<XLName *> (tree);
    if (name)
    {
        text modname = XLNormalize(name->value);
        if (XLModules.count(modname))
        {
            out << XLModules[modname];
            return 1;
        }
        else
        {
            out << "XLDeref(" << modname << ")";
            return 0;
        }
    }

    XLInfix *infix = dynamic_cast<XLInfix *> (tree);
    if (infix && infix->name == text("."))
    {
        if (XLNamespaceScope(infix->left))
            out << "::";
        else
            out << ".";
        return XLNamespaceScope(infix->right);
    }
    out << "XLDeref(";
    XL2C(tree);
    out << ")";
    return 0;
}

INFIX(scope)
// ----------------------------------------------------------------------------
//   Deal with X.Y, check if X is a namespace name
// ----------------------------------------------------------------------------
{
    if (XLNamespaceScope(tree->left))
        out << "::";
    else
        out << ".";
    XL2C(tree->right);    
}


INFIX(declaration)
// ----------------------------------------------------------------------------
//   Process declarations
// ----------------------------------------------------------------------------
{
    XLInfix *init = dynamic_cast<XLInfix *> (tree->right);
    if (init && init->name == ":=")
    {
        XL2C(init->left);       // Type
        out << ' ';
        XL2C(tree->left);       // Entity name
        out << " = ";
        XL2C(init->right);      // Initializer
    }
    else
    {
        XL2C(tree->right);        // Type name
        out << ' ';
        XL2C(tree->left);         // Entity name
        if (in_procedure && !in_parameter_declaration)
        {
            out << " = XLDefaultInit< ";
            XL2C(tree->right);
            out << " > :: value()";
        }
    }
}


INFIX(sequence)
// ----------------------------------------------------------------------------
//   Process a sequence of declarations
// ----------------------------------------------------------------------------
{
    XL2C(tree->left);
    if (tree->left->Kind() == xlNAME)
        out << "()";
    if (in_parameter_declaration)
        out << ", ";
    else
        out << ";\n";
    XL2C(tree->right);
    if (tree->right->Kind() == xlNAME)
        out << "()";
#if 0
    if (in_parameter_declaration)
        out << "";
    else
        out << ";\n"; // It doesn't hurt...
#endif
}


INFIX(list)
// ----------------------------------------------------------------------------
//   Process a list of values
// ----------------------------------------------------------------------------
{
    XL2C(tree->left);
    out << ", ";
    XL2C(tree->right);
}


INFIX(range)
// ----------------------------------------------------------------------------
//    We represent a range using an std::pair
// ----------------------------------------------------------------------------
{
    out << "XLMakeRange(";
    XL2C(tree->left);
    out << ", ";
    XL2C(tree->right);
    out << ")";
}


INFIX(is)
// ----------------------------------------------------------------------------
//   Process the 'is' keyword
// ----------------------------------------------------------------------------
{
    XL2C(tree->left);
    if (in_procedure && tree->left->Kind() == xlNAME)
        out << "(void)";
    IntSaver in_parm(in_parameter_declaration, 0);
    XL2C(tree->right);
}


INFIX(return)
// ----------------------------------------------------------------------------
//   The infix return is part of function declaration
// ----------------------------------------------------------------------------
{
    XL2C(tree->right);
    out << ' ';
    XL2C(tree->left);
    if (in_procedure && tree->left->Kind() == xlNAME)
        out << "(void)";
}


PREFIX(function)
// ----------------------------------------------------------------------------
//   The prefix function shows its argument
// ----------------------------------------------------------------------------
{
    IntSaver in_parm(in_parameter_declaration, true);
    IntSaver in_proc(in_procedure, true);
    XLInfix *is_part = dynamic_cast<XLInfix *> (tree->right);
    if (is_part && is_part->name == text("is"))
    {
        XLInfix *return_part = dynamic_cast<XLInfix *> (is_part->left);
        if (return_part && return_part->name == text("return"))
        {
            XL2C(return_part);
            out << "\n{ ";
            IntSaver in_parm(in_parameter_declaration, 0);
            XL2C(return_part->right);
            out << " result = XLDefaultInit < ";
            XL2C(return_part->right);
            out << ">::value ();\n";
            XL2C(is_part->right);
            out << "return result;\n";
            out << "}\n";
            return;
        }
    }
    XL2C(tree->right);
}


PREFIX(procedure)
// ----------------------------------------------------------------------------
//   The prefix function shows its argument
// ----------------------------------------------------------------------------
{
    IntSaver in_parm(in_parameter_declaration, true);
    IntSaver in_proc(in_procedure, true);
    out << "void ";
    XL2C(tree->right);
}


PREFIX(type)
// ----------------------------------------------------------------------------
//   Typedefs
// ----------------------------------------------------------------------------
{
    XLInfix *right = dynamic_cast<XLInfix *> (tree->right);
    if (right && right->name == text("is"))
    {
        XLName *name = dynamic_cast<XLName *> (right->left);
        if (name)
        {
            XLInfix *with = dynamic_cast<XLInfix *> (right->right);
            if (with && with->name == text("with"))
            {
                out << "struct ";
                XL2C(right->left);
                out << " : ";
                XL2C(with->left);
                XL2C(with->right);
            }
            else
            {
                TextSaver type_name(in_typedef, XLNormalize(name->value));
                out << "typedef ";
                XL2C(right->right);
                out << " ";
                XL2C(right->left);
            }
        }
        else
        {
            out << "typedef ";
            XL2C(right->right);
            out << " ";
            XL2C(right->left);
        }
    }
    else if (tree->right->Kind() == xlNAME)
    {
        out << "struct ";
        XL2C(tree->right);
        out << ";\n";
    }
    else
    {
        out << "*** Bad typedef ";
        XL2C(tree->right);    
    }
}


PREFIX(record)
// ----------------------------------------------------------------------------
//   Record types are turned into structs
// ----------------------------------------------------------------------------
{
    out << "struct " << in_typedef << ' ';
    TextSaver str(in_struct, in_typedef);
    XL2C(tree->right);
}


PREFIX(enumeration)
// ----------------------------------------------------------------------------
//   Enumeration types are turned into enums
// ----------------------------------------------------------------------------
{
    out << "enum " << in_typedef << ' ';
    TextSaver str(in_struct, in_typedef);
    TextSaver en(in_enum, in_typedef);
    XL2C(tree->right);
}



// ============================================================================
// 
//   Statements
// 
// ============================================================================

INFIX(then)
// ----------------------------------------------------------------------------
//   The 'then' is the top-level of an 'if'
// ----------------------------------------------------------------------------
{
    XL2C(tree->left); // The 'if'
    XL2C(tree->right); // The block    
}


INFIX(else)
// ----------------------------------------------------------------------------
//   The 'else' is the top-level of an 'if'
// ----------------------------------------------------------------------------
{
    XL2C(tree->left); // The 'if - then'
    out << "else\n";
    XL2C(tree->right); // The block    
}


PREFIX(loop)
// ----------------------------------------------------------------------------
//    A forever loop
// ----------------------------------------------------------------------------
{
    out << "for(;;)";
    XL2C(tree->right);
}


INFIX(loop)
// ----------------------------------------------------------------------------
//   A while loop
// ----------------------------------------------------------------------------
{
    XL2C(tree->left);
    XL2C(tree->right);
}


PREFIX(for)
// ----------------------------------------------------------------------------
//   Try to mimic the semantics of the XL for loop in a very ad-hoc way
// ----------------------------------------------------------------------------
{
    static int forloop = 0;
    int loop = forloop++;
    out << "XLIterator *XLiter" << loop << " = ";
    XL2C(tree->right);
    out << ";\n";
    out << "for (XLiter" << loop << "->first(); "
        << "XLiter" << loop << "->more() || "
        << "XLDeleteIterator(XLiter" << loop << "); "
        << "XLiter" << loop << "->next())";
}


PREFIX(exit)
// ----------------------------------------------------------------------------
//   Exiting from a loop
// ----------------------------------------------------------------------------
{
    XL2C(tree->right);
    out << " break";
}


INFIX(iterator)
// ----------------------------------------------------------------------------
//    Use for "A in B" to create an iterator
// ----------------------------------------------------------------------------
{
    out << "XLMakeIterator(";
    XL2C(tree->left);
    out << ", ";
    XL2C(tree->right);
    out << ")";
}


PREFIX(in)
// ----------------------------------------------------------------------------
//   We turn in parameters into ... nothing
// ----------------------------------------------------------------------------
{
    XL2C(tree->right);
}


PREFIX(out)
// ----------------------------------------------------------------------------
//   We turn out parameters into references
// ----------------------------------------------------------------------------
{
    out << "&";
    XL2C(tree->right);
}


INFIX(of)
// ----------------------------------------------------------------------------
//   We turn A of B into A<B>, expecting a template type
// ----------------------------------------------------------------------------
{
    XL2C(tree->left);
    out << "< ";
    XL2C(tree->right);
    out << " >";
}


INFIX(to)
// ----------------------------------------------------------------------------
//   We turn pointer to X into X &, so that we can use X.Y
// ----------------------------------------------------------------------------
{
    XLName *pointer = dynamic_cast<XLName *> (tree->left);
    if (pointer && pointer->value == text("pointer"))
    {
        XL2C(tree->right);
        out << "*";
    }
    else
    {
        out << "??? unexpected 'to'";
    }
}


PREFIX(new)
// ----------------------------------------------------------------------------
//   The new operator is transformed into a reference
// ----------------------------------------------------------------------------
{
    out << "(new ";
    XL2C(tree->right);
    out << ")";
}



// ============================================================================
// 
//   Built-in functions
// 
// ============================================================================

text default_stream = "";

void DoWrite(XLTree *arg)
// ----------------------------------------------------------------------------
//   Write an argument
// ----------------------------------------------------------------------------
{
    if (XLInfix *infix = dynamic_cast<XLInfix *> (arg))
    {
        if (infix->name == text(","))
        {
            DoWrite(infix->left);
            out << ";\n";
            DoWrite(infix->right);
            return;
        }
    }
    if (XLName *name = dynamic_cast<XLName *> (arg))
    {
        if (!default_stream.length())
        {
            default_stream = XLNormalize(name->value);
            return;
        }
    }

    out << "XLWrite(";
    if (!default_stream.length())
        default_stream = "&std::cout";
    out << default_stream << ", ";
    XL2C(arg);
    out << ")";
}


PREFIX(write)
// ----------------------------------------------------------------------------
//   Implement the built-in "write" function
// ----------------------------------------------------------------------------
{
    default_stream = "";
    DoWrite(tree->right);
}


PREFIX(writeln)
// ----------------------------------------------------------------------------
//   Same thing with the built-in writeln function
// ----------------------------------------------------------------------------
{
    default_stream = "";
    DoWrite(tree->right);
    out << ";\nXLWrite(";
    if (default_stream.length())
        out << default_stream << ", ";
    else
        out << "&std::cout, ";
    out << "\"\\n\");";
}


void DoRead(XLTree *arg)
// ----------------------------------------------------------------------------
//   Read an argument
// ----------------------------------------------------------------------------
{
    if (XLInfix *infix = dynamic_cast<XLInfix *> (arg))
    {
        if (infix->name == text(","))
        {
            DoRead(infix->left);
            out << ";\n";
            DoRead(infix->right);
            return;
        }
    }
    if (XLName *name = dynamic_cast<XLName *> (arg))
    {
        if (!default_stream.length())
        {
            default_stream = XLNormalize(name->value);
            return;
        }
    }

    out << "XLRead(";
    if (!default_stream.length())
        default_stream = "&std::cin, ";
    out << default_stream << ", ";
    XL2C(arg);
    out << ")";
}


PREFIX(read)
// ----------------------------------------------------------------------------
//   Implement the built-in "read" function
// ----------------------------------------------------------------------------
{
    default_stream = "";
    DoRead(tree->right);
}


PREFIX(readln)
// ----------------------------------------------------------------------------
//   Same thing with the built-in readln function
// ----------------------------------------------------------------------------
{
    default_stream = "";
    DoRead(tree->right);
}
