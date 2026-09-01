// Entry points around the generated flex/bison parser.
#include "lvs/netlist.hpp"
#include "lvs/verilog.tab.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

extern int vlgparse(void);
extern FILE *vlgin;
struct yy_buffer_state;
extern yy_buffer_state *vlg_scan_string(const char *);
extern void vlg_delete_buffer(yy_buffer_state *);
extern void vlglex_destroy(void);

namespace lvs {

namespace {

Netlist finish(VlgParseState &st, int rc)
{
    vlglex_destroy();
    vlg_state = nullptr;
    if (rc != 0 || !st.error.empty())
        throw std::runtime_error(st.error.empty() ? (st.origin + ": parse failed") : st.error);
    return std::move(st.netlist);
}

} // namespace

Netlist parse_verilog_string(const std::string &text, const std::string &origin)
{
    VlgParseState st;
    st.origin = origin;
    vlg_state = &st;
    auto *buf = vlg_scan_string(text.c_str());
    int rc = vlgparse();
    vlg_delete_buffer(buf);
    return finish(st, rc);
}

Netlist parse_verilog_file(const std::string &path)
{
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_verilog_string(ss.str(), path);
}

} // namespace lvs
