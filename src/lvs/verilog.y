/* Structural Verilog netlist grammar.
 *
 * A subset of ver_front/grammar.mly, keeping that description's rule names so
 * the two stay comparable: moduleDecl, modPortsE/PortList, varDecl, instDecl,
 * instnameParen, cellpinItem, AssignOne.  What is left out is everything a
 * gate-level netlist never contains -- behavioural statements, generate,
 * specify, tasks/functions, PSL -- and each omission is a production to add
 * from that file rather than a redesign here.
 */
%define api.prefix {vlg}
%define parse.error verbose
%locations

%code requires {
#include "lvs/netlist.hpp"
#include <string>
#include <vector>

struct VlgParseState {
    lvs::Netlist netlist;
    lvs::Module module;
    std::string origin;
    std::string error;
};
}

%code provides {
extern VlgParseState *vlg_state;
int vlglex(void);
void vlgerror(const char *msg);
}

%code {
#include <sstream>
#include <stdexcept>

VlgParseState *vlg_state = nullptr;

/* Scratch for cellpinItem: see the comment on that rule. */
static lvs::Pin pin_tmp;

/* Ports are named in the header and given a direction by a later
 * declaration; 2001-style headers give both at once.  Merge rather than
 * duplicate, so the module's port list keeps header order either way. */
static void merge_port(lvs::Module &m, const lvs::PortDecl &d)
{
    for (auto &p : m.ports)
        if (p.name == d.name) {
            if (d.dir != lvs::PortDecl::Dir::None)
                p.dir = d.dir;
            if (!d.range.scalar)
                p.range = d.range;
            return;
        }
    m.ports.push_back(d);
}
}

%union {
    std::string *str;
    lvs::Expr *expr;
    std::vector<lvs::Expr> *exprs;
    std::vector<lvs::Pin> *pins;
    std::vector<lvs::Param> *params;
    std::vector<std::string> *names;
    lvs::Range *range;
    int dir;
}

%token MODULE ENDMODULE INPUT OUTPUT INOUT WIRE REG ASSIGN SUPPLY0 SUPPLY1
%token <str> IDSTR NUMBER STRING
%token LPAREN RPAREN LBRACK RBRACK LCURLY RCURLY SEMICOLON COMMA COLON DOT EQUALS HASH

%type <str> identifier
%type <range> Anyrange PortRangeE
%type <dir> PortDirection
%type <expr> expr varRefDotBit
%type <exprs> concIdList
%type <pins> cellpinList cellpinItList
%type <params> paramValueE cellparamList
%type <names> netSigList

%destructor { delete $$; } /* safe: actions null what they free */ <str> <expr> <exprs> <pins> <params> <names> <range>

%start start

%%

start
    : /* empty */
    | start modprimDecl
    ;

modprimDecl
    : moduleDecl
    ;

moduleDecl
    : MODULE identifier { vlg_state->module = lvs::Module(); vlg_state->module.name = *$2; delete $2; $2 = nullptr; }
      modPortsE SEMICOLON modItemListE ENDMODULE
      { vlg_state->netlist.modules.push_back(vlg_state->module); }
    ;

modPortsE
    : /* empty */
    | LPAREN RPAREN
    | LPAREN PortList RPAREN
    ;

PortList
    : Port
    | PortList COMMA Port
    ;

/* 1995 style names a port here and declares it below; 2001 style declares it
 * inline.  Both reach merge_port(). */
Port
    : identifier
      { lvs::PortDecl d; d.name = *$1; delete $1; $1 = nullptr; merge_port(vlg_state->module, d); }
    | PortDirection PortRangeE identifier
      { lvs::PortDecl d; d.dir = lvs::PortDecl::Dir($1); d.range = *$2; d.name = *$3;
        delete $2; $2 = nullptr; delete $3; $3 = nullptr; merge_port(vlg_state->module, d); }
    | PortDirection WIRE PortRangeE identifier
      { lvs::PortDecl d; d.dir = lvs::PortDecl::Dir($1); d.range = *$3; d.name = *$4;
        delete $3; $3 = nullptr; delete $4; $4 = nullptr; merge_port(vlg_state->module, d); }
    | PortDirection REG PortRangeE identifier
      { lvs::PortDecl d; d.dir = lvs::PortDecl::Dir($1); d.range = *$3; d.name = *$4;
        delete $3; $3 = nullptr; delete $4; $4 = nullptr; merge_port(vlg_state->module, d); }
    ;

PortDirection
    : INPUT  { $$ = int(lvs::PortDecl::Dir::Input); }
    | OUTPUT { $$ = int(lvs::PortDecl::Dir::Output); }
    | INOUT  { $$ = int(lvs::PortDecl::Dir::Inout); }
    ;

modItemListE
    : /* empty */
    | modItemListE modItem
    ;

modItem
    : varDecl
    | instDecl
    | ASSIGN AssignList SEMICOLON
    ;

varDecl
    : PortDirection PortRangeE netSigList SEMICOLON
      { for (auto &n : *$3) { lvs::PortDecl d; d.dir = lvs::PortDecl::Dir($1); d.range = *$2; d.name = n;
                              merge_port(vlg_state->module, d); }
        delete $2; $2 = nullptr; delete $3; $3 = nullptr; }
    | netType PortRangeE netSigList SEMICOLON
      { for (auto &n : *$3) { lvs::PortDecl d; d.range = *$2; d.name = n;
                              vlg_state->module.nets.push_back(d); }
        delete $2; $2 = nullptr; delete $3; $3 = nullptr; }
    | netType PortRangeE identifier EQUALS expr SEMICOLON
      { lvs::PortDecl d; d.range = *$2; d.name = *$3;
        vlg_state->module.nets.push_back(d);
        lvs::Expr lhs; lhs.kind = lvs::Expr::Kind::Id; lhs.name = *$3;
        vlg_state->module.assigns.push_back({lhs, *$5});
        delete $2; $2 = nullptr; delete $3; $3 = nullptr; delete $5; $5 = nullptr; }
    | PortDirection netType PortRangeE netSigList SEMICOLON
      { for (auto &n : *$4) { lvs::PortDecl d; d.dir = lvs::PortDecl::Dir($1); d.range = *$3; d.name = n;
                              merge_port(vlg_state->module, d);
                              lvs::PortDecl w = d; w.dir = lvs::PortDecl::Dir::None;
                              vlg_state->module.nets.push_back(w); }
        delete $3; $3 = nullptr; delete $4; $4 = nullptr; }
    ;

netType
    : WIRE
    | REG
    | SUPPLY0
    | SUPPLY1
    ;

netSigList
    : identifier                    { $$ = new std::vector<std::string>{*$1}; delete $1; $1 = nullptr; }
    | netSigList COMMA identifier   { $$ = $1; $$->push_back(*$3); delete $3; $3 = nullptr; }
    ;

PortRangeE
    : /* empty */ { $$ = new lvs::Range(); }
    | Anyrange    { $$ = $1; }
    ;

Anyrange
    : LBRACK NUMBER COLON NUMBER RBRACK
      { $$ = new lvs::Range(); $$->scalar = false;
        $$->msb = std::atoi($2->c_str()); $$->lsb = std::atoi($4->c_str());
        delete $2; $2 = nullptr; delete $4; $4 = nullptr; }
    ;

/* instDecl: type [#(params)] name ( pins ) ; */
instDecl
    : identifier paramValueE identifier LPAREN cellpinList RPAREN SEMICOLON
      { lvs::Instance inst;
        inst.type = *$1; inst.name = *$3;
        inst.params = *$2; inst.pins = *$5;
        delete $1; $1 = nullptr; delete $2; $2 = nullptr; delete $3; $3 = nullptr; delete $5; $5 = nullptr;
        vlg_state->module.instances.push_back(inst); }
    ;

paramValueE
    : /* empty */                        { $$ = new std::vector<lvs::Param>(); }
    | HASH LPAREN cellparamList RPAREN   { $$ = $3; }
    ;

cellparamList
    : /* empty */                          { $$ = new std::vector<lvs::Param>(); }
    | DOT identifier LPAREN NUMBER RPAREN
      { $$ = new std::vector<lvs::Param>(); $$->push_back({*$2, *$4}); delete $2; $2 = nullptr; delete $4; $4 = nullptr; }
    | DOT identifier LPAREN STRING RPAREN
      { $$ = new std::vector<lvs::Param>(); $$->push_back({*$2, *$4}); delete $2; $2 = nullptr; delete $4; $4 = nullptr; }
    | NUMBER
      { $$ = new std::vector<lvs::Param>(); $$->push_back({"", *$1}); delete $1; $1 = nullptr; }
    | cellparamList COMMA DOT identifier LPAREN NUMBER RPAREN
      { $$ = $1; $$->push_back({*$4, *$6}); delete $4; $4 = nullptr; delete $6; $6 = nullptr; }
    | cellparamList COMMA DOT identifier LPAREN STRING RPAREN
      { $$ = $1; $$->push_back({*$4, *$6}); delete $4; $4 = nullptr; delete $6; $6 = nullptr; }
    | cellparamList COMMA NUMBER
      { $$ = $1; $$->push_back({"", *$3}); delete $3; $3 = nullptr; }
    ;

cellpinList
    : cellpinItList { $$ = $1; }
    ;

cellpinItList
    : /* empty */                     { $$ = new std::vector<lvs::Pin>(); }
    | cellpinItem                     { $$ = new std::vector<lvs::Pin>(); $$->push_back(pin_tmp); }
    | cellpinItList COMMA cellpinItem { $$ = $1; $$->push_back(pin_tmp); }
    ;

/* cellpinItem stashes into a scratch Pin because a mid-rule value would need
 * a %union member for it; the grammar is LALR(1) and single-threaded, so the
 * scratch is safe and keeps the union small. */
cellpinItem
    : DOT identifier LPAREN RPAREN         { pin_tmp = lvs::Pin{*$2, lvs::Expr()}; delete $2; $2 = nullptr; }
    | DOT identifier LPAREN expr RPAREN    { pin_tmp = lvs::Pin{*$2, *$4}; delete $2; $2 = nullptr; delete $4; $4 = nullptr; }
    | expr                                 { pin_tmp = lvs::Pin{"", *$1}; delete $1; $1 = nullptr; }
    ;

AssignList
    : AssignOne
    | AssignList COMMA AssignOne
    ;

AssignOne
    : varRefDotBit EQUALS expr
      { vlg_state->module.assigns.push_back({*$1, *$3}); delete $1; $1 = nullptr; delete $3; $3 = nullptr; }
    ;

varRefDotBit
    : identifier                          { $$ = new lvs::Expr(); $$->kind = lvs::Expr::Kind::Id; $$->name = *$1; delete $1; $1 = nullptr; }
    | identifier LBRACK NUMBER RBRACK
      { $$ = new lvs::Expr(); $$->kind = lvs::Expr::Kind::BitSel; $$->name = *$1;
        $$->index = std::atoi($3->c_str()); delete $1; $1 = nullptr; delete $3; $3 = nullptr; }
    | identifier LBRACK NUMBER COLON NUMBER RBRACK
      { $$ = new lvs::Expr(); $$->kind = lvs::Expr::Kind::PartSel; $$->name = *$1;
        $$->range.scalar = false; $$->range.msb = std::atoi($3->c_str());
        $$->range.lsb = std::atoi($5->c_str()); delete $1; $1 = nullptr; delete $3; $3 = nullptr; delete $5; $5 = nullptr; }
    ;

expr
    : varRefDotBit           { $$ = $1; }
    | NUMBER                 { $$ = new lvs::Expr(); $$->kind = lvs::Expr::Kind::Const; $$->const_text = *$1; delete $1; $1 = nullptr; }
    | LCURLY concIdList RCURLY
      { $$ = new lvs::Expr(); $$->kind = lvs::Expr::Kind::Concat; $$->parts = *$2; delete $2; $2 = nullptr; }
    ;

concIdList
    : expr                    { $$ = new std::vector<lvs::Expr>{*$1}; delete $1; $1 = nullptr; }
    | concIdList COMMA expr   { $$ = $1; $$->push_back(*$3); delete $3; $3 = nullptr; }
    ;

identifier
    : IDSTR { $$ = $1; }
    ;

%%

void vlgerror(const char *msg)
{
    std::ostringstream os;
    os << vlg_state->origin << ':' << vlglloc.first_line << ": " << msg;
    vlg_state->error = os.str();
}
