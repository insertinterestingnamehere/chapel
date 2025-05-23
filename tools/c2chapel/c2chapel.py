#!/usr/bin/env python3

#
# Copyright 2020-2025 Hewlett Packard Enterprise Development LP
# Copyright 2004-2019 Cray Inc.
# Other additional copyright holders may be indicated within.
#
# The entirety of this work is licensed under the Apache License,
# Version 2.0 (the "License"); you may not use this file except
# in compliance with the License.
#
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

from __future__ import print_function

""" A C to Chapel parser to automatically generate extern bindings.

This tool is built on top of pycparser :
  https://github.com/eliben/pycparser

See README.rst for more information.
"""

__version__ = "0.1.0"

# TODO: look at .type.type and make sure we're not relying on a specific
# AST depth (e.g., watch out for pointers-to-pointers-to-pointers-to-...
#
# TODO: enums?

import sys

try:
    from pycparser import c_parser, c_ast, parse_file
    from pycparserext import ext_c_parser
except ImportError as e:
    sys.exit("Unable to import pycparser: " + str(e))

import argparse
import os.path
try:
    from StringIO import StringIO
except ImportError as e:
    from io import StringIO
import re

noComments = False
DEBUG      = False

VARARGS_STR = "c__varargs ..."

## Global dictionary of C->Chapel mappings. New types can
## be registered here.
c2chapel = {}

c2chapel["double"] = "c_double"
c2chapel["float"]  = "c_float"
c2chapel["char"]   = "c_char"
c2chapel["void"]   = ""

# Based on ChapelSysCTypes.chpl
c2chapel["int"]                = "c_int"
c2chapel["unsigned"]           = "c_uint"
c2chapel["unsigned int"]       = "c_uint"
c2chapel["long"]               = "c_long"
c2chapel["unsigned long"]      = "c_ulong"
c2chapel["long long"]          = "c_longlong"
c2chapel["unsigned long long"] = "c_ulonglong"
c2chapel["char"]               = "c_char"
c2chapel["signed char"]        = "c_schar"
c2chapel["unsigned char"]      = "c_uchar"
c2chapel["short"]              = "c_short"
c2chapel["unsigned short"]     = "c_ushort"
c2chapel["intptr_t"]           = "c_intptr"
c2chapel["uintptr_t"]          = "c_uintptr"
c2chapel["ptrdiff_t"]          = "c_ptrdiff"
c2chapel["ssize_t"]            = "c_ssize_t"
c2chapel["size_t"]             = "c_size_t"
c2chapel["wchar_t"]            = "c_wchar_t"
c2chapel["long double"]        = "c_longlong"
c2chapel["signed short"]       = "c_short"
c2chapel["signed int"]         = "c_int"
c2chapel["signed long long"]   = "c_longlong"
c2chapel["signed long"]        = "c_long"

# Note: this mapping is defined by CTypes, not the ChapelSysCTypes file
c2chapel["FILE"] = "c_FILE"

__temp = [k for k in c2chapel.keys()]
for key in __temp:
    if key.endswith("long"):
        c2chapel[key + " int"] = c2chapel[key]

for i in range(3, 7):
    width = 2**i
    s  = "int" + str(width) + "_t"
    ch = "int(" + str(width) + ")"

    c2chapel[s] = ch
    c2chapel["u" + s] = "u" + ch

# Map of strings to c_ast.TypeDef
typeDefs = {}

# set of types c2chapel is aware of
foundTypes = set()

# set of Chapel keywords, used to identify if a C identifier needs to be
# replaced.
chapelKeywords = set(["align","as","atomic","begin","break","by","class",
    "cobegin","coforall","config","const","continue","delete","dmapped","do",
    "domain","else","enum","except","export","extern","for","forall","if",
    "in","index","inline","inout","iter","label","lambda","let","local","module","new",
    "nil","noinit","on","only","otherwise","out","param","private","proc",
    "public","record","reduce","ref","require","return","scan","select",
    "serial","sparse","subdomain","sync","then","type","union","use",
    "var","when","where","while","with","yield","zip", "string", "bytes", "locale"])


def getArgs():
    parser = argparse.ArgumentParser(description="Generate C bindings for Chapel", prog="c2chapel")
    parser.add_argument("file", help="C99 file for which to generate bindings. Additional arguments are forwarded to the C preprocessor (invoked with 'cc -E')")
    parser.add_argument("--no-typedefs",
                        help="do not generate extern types for C typedefs",
                        action="store_true")
    parser.add_argument("--debug",
                        help="enable debugging output",
                        action="store_true")
    parser.add_argument("--no-fake-headers",
                        help="do not use fake headers included with c2chapel",
                        action="store_true")
    parser.add_argument("--no-comments",
                        help="instruct c2chapel to not generate comments",
                        action="store_true")
    parser.add_argument("-V", "--version", action="version", version="%(prog)s " + __version__)
    parser.add_argument("--gnu-extensions",
                        help="allow GNU extensions in C99 files",
                        action="store_true")

    usage = parser.format_usage()
    parser.usage = usage.rstrip().removeprefix("usage: ") + " ..."

    return parser.parse_known_args()

# s - string to print out.
# each line will be prepended with '// '
def genComment(s):
    if noComments:
        return
    for line in s.split("\n"):
        print("// " + line)

def commentNode(node):
    buf = StringIO()
    node.show(buf=buf)
    genComment(buf.getvalue())

def getDeclName(decl):
    inner = decl
    if type(inner) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
        inner = inner.type

    name = ""
    if type(inner) == c_ast.IdentifierType:
        name = " ".join(inner.names)
    elif type(inner) == c_ast.Struct or type(inner) == c_ast.Union:
        # Because Unions and Structs should be handled in the same way
        name = inner.name
    elif type(inner) == c_ast.Decl:
        name = inner.name
    elif type(inner) == c_ast.Enum:
        name = "c_int"
    else:
        raise Exception("Unhandled node type: " + str(type(inner)))
    return name

def computeArgName(decl):
    if type(decl) == c_ast.Typename:
        return ""
    elif type(decl) == c_ast.Decl:
        if decl.name in chapelKeywords:
            return decl.name + "_arg"
        else:
            return decl.name
    else:
        decl.show()
        raise c_parser.ParseError("Unhandled Node type")

def isStructOrUnionType(ast):
    inner = ast

    if type(inner) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
        inner = inner.type

    if type(inner) == c_ast.IdentifierType:
        name = " ".join(inner.names)
        if name in typeDefs:
            return isStructOrUnionType(typeDefs[name].type)

    if type(inner) == c_ast.Struct or type(inner) == c_ast.Union:
        return True

    return False

def getIntentInfo(ty):
    refIntent = ""
    retType   = ""
    curType   = ty
    ptrType = ""

    if type(curType) == c_ast.PtrDecl:
        ptrType = toChapelType(curType)

    if type(curType) == c_ast.PtrDecl and not (isPointerTo(curType, "unsigned char") or isPointerTo(curType, "char") or isPointerTo(curType, "void") or toChapelType(curType) == "c_fn_ptr"):
        if ptrType and "const" in curType.type.quals:
            refIntent = "const "
        refIntent += "ref"
        curType = curType.type
    else:
        refIntent = ""

    retType = toChapelType(curType)

    return (refIntent, retType, ptrType)

# pl - a c_ast.ParamList
def computeArgs(pl):
    formals = []
    ptrFormals = []

    if pl is None:
        return ("", "")

    for (i, arg) in enumerate(pl.params):
        if type(arg) == c_ast.EllipsisParam:
            formals.append(VARARGS_STR)
            ptrFormals.append(VARARGS_STR)
        else:
            (intent, typeName, ptrTypeName) = getIntentInfo(arg.type)
            argName = computeArgName(arg)
            if typeName != "":
                if intent == "":
                    if isStructOrUnionType(arg.type):
                        intent = "in "
                else:
                    intent += " "
                if argName == "":
                    argName = "arg" + str(i)
                formals.append(intent + argName + " : " + typeName)

                if ptrTypeName != "":
                    ptrFormals.append(argName + " : " + ptrTypeName)
                else:
                    ptrFormals.append(intent + argName + " : " + typeName)

    return (", ".join(formals), ", ".join(ptrFormals))

def isPointerTo(ty, text):
    if type(ty) == c_ast.PtrDecl:
        if type(ty.type) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
            name = getDeclName(ty.type)
            if name == text:
                return True
    return False

def toChapelType(ty):
    if isPointerTo(ty, "char"):
        if "const" in ty.type.quals:
            return "c_ptrConst(" + "c_char" + ")"
        else:
            return "c_ptr(" + "c_char" + ")"
    elif type(ty) in (c_ast.ArrayDecl, ext_c_parser.ArrayDeclExt):
        eltType = toChapelType(ty.type)
        if eltType is not None:
            if type(ty.type) in (c_ast.ArrayDecl, ext_c_parser.ArrayDeclExt):
                return eltType
            elif "const" in ty.type.quals:
                return "c_ptrConst(" + eltType + ")"
            else:
                return "c_ptr(" + eltType + ")"
        else:
            return None
    elif type(ty) == c_ast.PtrDecl:
        if type(ty.type) in (c_ast.FuncDecl, ext_c_parser.FuncDeclExt):
            return "c_fn_ptr"
        else:
            eltType = ("void" if isPointerTo(ty, "void")
                       else toChapelType(ty.type))
            if "const" in ty.type.quals:
                return "c_ptrConst(" + eltType + ")"
            else:
                return "c_ptr(" + eltType + ")"
    elif type(ty) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
        inner = ty.type
        name = ""
        try:
            name = getDeclName(ty)
        except Exception as e:
            raise Exception("toChapelType: " + str(e))

        if name in c2chapel:
            return c2chapel[name]

        return name
    elif type(ty) in (c_ast.FuncDecl, ext_c_parser.FuncDeclExt):
        return "c_fn_ptr"
    else:
        ty.show()
        raise c_parser.ParseError("Unable to translate to Chapel type")


def getFunctionName(ty):
    if type(ty) == c_ast.PtrDecl:
        return getFunctionName(ty.type)
    elif type(ty) in (c_ast.FuncDecl, ext_c_parser.FuncDeclExt):
        return getFunctionName(ty.type)
    else:
        if type(ty) not in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
            ty.show()
            raise c_parser.ParseError("Expecting TypeDecl...")
        return ty.declname


# Convert function declarations into Chapel
def genFuncDecl(fn):
    retType = toChapelType(fn.type)
    fnName  = getFunctionName(fn.type)
    (args, ptrArgs) = computeArgs(fn.args)

    if fnName in chapelKeywords:
        genComment("Unable to generate function '" + fnName + "' because its name is a Chapel keyword")
        print()
        return

    if re.match(r'.*: \bva_list\b', args):
        genComment("Unable to generate function '" + fnName + "' due to va_list argument")
        print()
        return

    if retType == "":
        retType = "void"

    print("extern proc " + fnName + "(" + args + ") : " + retType + ";\n")
    if ptrArgs != args:
        print("extern proc " + fnName + "(" + ptrArgs + ") : " + retType + ";\n")

    listArgs = args.split(", ")
    if listArgs[-1] == VARARGS_STR:
        newArgs = ",".join(listArgs[:-1])
        genComment("Overload for empty varargs")
        print("extern proc " + fnName + "(" + newArgs + ") : " + retType + ";\n")

def getStructOrUnionDef(decl):
    if type(decl) == c_ast.Struct or type(decl) == c_ast.Union:
        if decl.decls is not None:
            return decl
        else:
            return None
    elif type(decl) == c_ast.Decl or type(decl) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt) or type(decl) == c_ast.PtrDecl:
        return getStructOrUnionDef(decl.type)
    else:
        return None


def isStructOrUnionForwardDeclared(node):
    return isStructOrUnionType(node) and not node.decls

def genStructOrUnion(structOrUnion, name="", isAnon=False):
    if name == "":
        if structOrUnion.name is not None:
            name = structOrUnion.name
        else:
            return

    innerDef = getStructOrUnionDef(structOrUnion)
    isUnion = isinstance(innerDef, c_ast.Union)

    if name in chapelKeywords:
        structOrUnion = "union" if isUnion else "struct"
        genComment("Unable to generate " + structOrUnion + " '" + name + "' " + "because its name is a Chapel keyword")
        print()
        return

    ret = ""
    if isAnon:
        ret = "extern union " if isUnion else "extern record "
    else:
        ret = "extern \"union " + name + "\" union " if isUnion else "extern \"struct " + name + "\" record "
    ret += name + " {"
    foundTypes.add(name)

    # Forward Declaration
    if isStructOrUnionForwardDeclared(structOrUnion):
        print()
        return

    members = ""
    warnKeyword = False
    warnSkippingAnonymousType = False
    for decl in structOrUnion.decls:
        innerStructOrUnion = getStructOrUnionDef(decl)
        if innerStructOrUnion is not None:
            genStructOrUnion(innerStructOrUnion, isAnon=False)

        if type(decl) == c_ast.Pragma:
            break
        fieldName = getDeclName(decl)
        if fieldName in chapelKeywords:
            members = ""
            warnKeyword = True
            break
        else:
            chapelType = toChapelType(decl.type)
            if chapelType is None:
                warnSkippingAnonymousType = True
                break
            members += "  var " + fieldName + " : " + chapelType + ";\n"

    if members != "":
        members = "\n" + members
    ret += members + "}\n"
    if warnKeyword:
        genComment("Fields omitted because one or more of the identifiers is a Chapel keyword")
    if warnSkippingAnonymousType:
        genComment("Anonymous union or struct was encountered within and skipped.")
    print(ret)

def genVar(decl):
    name = decl.name
    if name in chapelKeywords:
        name = 'c2chapel_' + name

    ty   = toChapelType(decl.type)
    print("extern var " + name + " : " + ty + ";")
    print()


def genEnum(decl):
    if type(decl) == c_ast.Enum:
        if decl.name:
            genComment("Enum: " + decl.name)
        else:
            genComment("Enum: anonymous")
        for val in decl.values.enumerators:
            print("extern const " + val.name + " :c_int;")
        print("\n")

def genTypeEnum(decl):
    if type(decl.type) == c_ast.Enum:
        for child in decl.children():
            for val in child[1].children():
                for value in val[1].enumerators:
                    print("extern const " + value.name + " :" + decl.declname + ";")
        print("\n")

# Simple visitor to all function declarations
class ChapelVisitor(c_ast.NodeVisitor):
    def visit_StructOrUnion(self, node):
        genStructOrUnion(node, isAnon=False)
        # If this is not a forward declaration, then preemptively prune this
        # struct or union from the typedef map (since this is the type's
        # definition). However, if this _was_ a forward declaration, then we
        # want to handle the possibility of an embedded definition later.
        # E.g., 'typedef struct foo { int x; } foo;'
        if not isStructOrUnionForwardDeclared(node):
            typeDefs[node.name] = None

    def visit_Typedef(self, node):
        if node.name not in typeDefs:
            typeDefs[node.name] = node

    def visit_FuncDecl(self, node):
        genFuncDecl(node)

    def visit_FuncDef(self, node):
        if DEBUG:
            genComment("[debug]: Skipping FuncDef body")
            commentNode(node)
        self.visit_Decl(node.decl)

    def visit_Decl(self, node):
        if DEBUG:
            genComment("Evaluating node:")
            commentNode(node)
        for c_name, c in node.children():
            if type(c) == c_ast.Struct or type(c) == c_ast.Union:
                self.visit_StructOrUnion(c)
            elif type(c) in (c_ast.FuncDecl, ext_c_parser.FuncDeclExt):
                self.visit_FuncDecl(c)
            elif type(c) == c_ast.Enum:
                genEnum(c)
            elif type(c) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt) or type(c) == c_ast.PtrDecl or type(c) in (c_ast.ArrayDecl, ext_c_parser.ArrayDeclExt):
                genVar(node)
            else:
                node.show()
                raise Exception("Unhandled declaration")



def genTypeAlias(node):
    alias = node.name
    if type(node.type) == c_ast.PtrDecl or type(node.type) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt):
        typeName = toChapelType(node.type)
        # let compiler handle void typedefs
        if typeName == '':
            print("extern type " + alias + ";")
        else:
            print("extern type " + alias + " = " + typeName + ";")
        foundTypes.add(alias)
        print()

def isPointerToStruct(node):
    if type(node) == c_ast.PtrDecl:
        if type(node.type) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt) and type(node.type.type) == c_ast.Struct:
            return node.type.type
        else:
            return isPointerToStruct(node.type)
    return None

def isPointerToUnion(node):
    if type(node) == c_ast.PtrDecl:
        if type(node.type) in (c_ast.TypeDecl, ext_c_parser.TypeDeclExt) and type(node.type.type) == c_ast.Union:
            return node.type.type
        else:
            return isPointerToUnion(node.type)
    return None

def genTypedefs(defs):
    for name in sorted(defs):
        node = defs[name]
        if node is not None:
            if type(node.type.type) == c_ast.Struct or type(node.type.type) == c_ast.Union:
                sn = node.type.type
                if sn.decls is not None:
                    genStructOrUnion(sn, name=node.name, isAnon=True)
                elif sn.name not in foundTypes:
                    isUnion = isinstance(node.type.type, c_ast.Union)
                    structOrUnion = "union" if isUnion else "struct"
                    genComment("Opaque " + structOrUnion + "?")
                    gen = "extern union " if isUnion else "extern record "
                    gen += name + " {};\n"
                    print(gen)
                else:
                    genTypeAlias(node)
            elif isPointerToStruct(node.type):
                genComment("Typedef'd pointer to struct")
                print("extern type " + node.name + ";\n")
            elif isPointerToUnion(node.type):
                genComment("Typedef'd pointer to union")
                print("extern type " + node.name + ";\n")
            elif type(node.type.type) == c_ast.Enum:
                genComment(node.name + " enum")
                print("extern type " + node.name + " = c_int;")
                genTypeEnum(node.type)
            else:
                genTypeAlias(node)

def handleTypedefs(defs, ignores):
    ignoreDefs = {}
    for i in ignores:
        if i in defs:
            ignoreDefs[i] = defs[i]
            defs[i] = None

    numDefs = sum([1 for x in defs if defs[x] is not None])
    if numDefs != 0:
        genComment("==== c2chapel typedefs ====")
        print()
        genTypedefs(defs)

    if len(ignoreDefs) != 0:
        genComment("c2chapel thinks these typedefs are from the fake headers:")
        print("/*")
        genTypedefs(ignoreDefs)
        print("*/")

# TODO: ignore those defined within an ifdef
# There's probably some macro magic we can play with such that we only generate
# the macro name if it is defined.
#
# TODO: handle other kinds of literals:
# - strings
# - hex, octal ints
# - floats/doubles?
# - bitshift constants (e.g. 1<<3)
def emit_defines(fname):
    with open(fname, "r") as f:
        pat = re.compile("^\\s*#define\\s+([_a-zA-Z0-9]+)\\s+[0-9]+$")
        first = True
        for line in f:
            res = pat.match(line)
            if res is not None:
                if first:
                    genComment("#define'd integer literals:")
                    genComment("Note: some of these may have been defined with an ifdef")
                    first = False

                print("extern const " + res.group(1) + " : int;")

        if not first:
            print()
            genComment("End of #define'd integer literals")
            print()

def getFakeHeaderPath():
    script = os.path.realpath(__file__)
    ret    = ""
    if os.path.isfile(script):
        parent = os.path.dirname(script)
        fakes = parent + "/install/fakeHeaders/"
        if os.path.isdir(fakes):
            ret = fakes

    return ret

def findIgnores():
    path = getFakeHeaderPath()
    ret = set()
    if path:
        defs = path + "_fake_typedefs.h"
        if os.path.isfile(defs):
            with open(defs, "r") as fi:
                for line in fi:
                    if line.startswith("typedef"):
                        rhs = line.replace(";", "")
                        rhs = rhs.replace("typedef int ", "")
                        rhs = rhs.replace("typedef uint32_t ", "")
                        rhs = rhs.replace("typedef _Bool ", "")
                        rhs = rhs.replace("typedef void* ", "")
                        if "typedef struct" in rhs:
                            rhs = rhs.split()[-1]

                        ret.add(rhs.strip())

    return ret

# Return a list of strings to be passed to the C preprocessor
def getFakeHeaders():
    ret  = []
    path = getFakeHeaderPath()
    if path:
        ret = ["-I", path]
    return ret

def preamble(args, fakes):
    genComment("Generated with c2chapel version " + __version__)
    print()

    fname = args.file
    if fname.endswith(".h"):
        genComment("Header given to c2chapel:")
        print("require \"" + fname + "\";\n")

    if len(fakes) != 0:
        genComment("Note: Generated with fake std headers")
        print()

    # Needed for C types
    # Arguably we can tighten the use of this module based on what we actually
    # generate, but for now this is good enough
    print("use CTypes;")

# TODO: accept file from stdin?
if __name__=="__main__":
    args, unknown = getArgs()
    fname      = args.file
    noComments = args.no_comments
    DEBUG      = args.debug
    useGnu     = args.gnu_extensions

    if not os.path.isfile(fname):
        sys.exit("No such file: '" + fname + "'")

    fakes = []
    ignores = set()
    if args.no_fake_headers is not True:
        fakes = getFakeHeaders()
        ignores = findIgnores()

    try:
        cpp_args = ["-E"] + fakes + unknown
        if useGnu:
            ast = parse_file(fname, use_cpp=True, cpp_path="cc", cpp_args=cpp_args, parser=ext_c_parser.GnuCParser())
        else:
            ast = parse_file(fname, use_cpp=True, cpp_path="cc", cpp_args=cpp_args)
    except c_parser.ParseError as e:
        sys.exit("Unable to parse file: " + str(e))

    preamble(args, fakes)

    emit_defines(fname)

    v = ChapelVisitor()
    v.visit(ast)

    if args.no_typedefs is False:
        handleTypedefs(typeDefs, ignores)

