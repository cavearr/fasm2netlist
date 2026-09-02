/* Structural Verilog lexer.
 *
 * Token names and the escaped-identifier rule follow ver_front/vlexer.mll.
 * The escaped form matters here rather than being a nicety: fasm2netlist
 * names every cell after its physical site, so every instance in its own
 * output arrives as \ff_CLBLM_R_X31Y134_SLICE_X46Y134_A5 with a terminating
 * space, and a lexer that does not strip that pair loses the name.
 */
%option prefix="vlg"
%option noyywrap nounput noinput never-interactive
%option yylineno

%{
#include "lvs/verilog.tab.hpp"
#include <string>

#define YY_USER_ACTION vlglloc.first_line = vlglloc.last_line = yylineno;
%}

ident       [a-zA-Z_][a-zA-Z0-9_$]*
escident    \\[^ \t\r\n]+[ \t\r\n]
sized       [0-9]*'[sS]?[bBoOdDhH][0-9a-fA-FxXzZ_?]+
decimal     [0-9][0-9_]*

%x COMMENT

%%

`[a-zA-Z_]+[^\n]*        { /* compiler directive: not configuration */ }
"//"[^\n]*              { /* line comment */ }
"/*"                    { BEGIN(COMMENT); }
<COMMENT>"*/"           { BEGIN(INITIAL); }
<COMMENT>.|\n           { }

[ \t\r\n]+              { }

"module"                { return MODULE; }
"endmodule"             { return ENDMODULE; }
"input"                 { return INPUT; }
"output"                { return OUTPUT; }
"inout"                 { return INOUT; }
"wire"                  { return WIRE; }
"reg"                   { return REG; }
"assign"                { return ASSIGN; }
"supply0"               { return SUPPLY0; }
"supply1"               { return SUPPLY1; }

{escident}              { std::string s(yytext + 1, yyleng - 2);
                          vlglval.str = new std::string(s); return IDSTR; }
{ident}                 { vlglval.str = new std::string(yytext, yyleng); return IDSTR; }
\"[^\"\n]*\"          { vlglval.str = new std::string(yytext, yyleng); return STRING; }
{sized}                 { vlglval.str = new std::string(yytext, yyleng); return NUMBER; }
{decimal}               { vlglval.str = new std::string(yytext, yyleng); return NUMBER; }

"("                     { return LPAREN; }
")"                     { return RPAREN; }
"["                     { return LBRACK; }
"]"                     { return RBRACK; }
"{"                     { return LCURLY; }
"}"                     { return RCURLY; }
";"                     { return SEMICOLON; }
","                     { return COMMA; }
":"                     { return COLON; }
"."                     { return DOT; }
"="                     { return EQUALS; }
"#"                     { return HASH; }

.                       { return yytext[0]; /* let the parser report it */ }

%%
