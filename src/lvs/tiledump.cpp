// Dump a FASM's decoded tile configuration in a canonical, diffable form.
//   tiledump design.fasm            -- the whole configuration
//   tiledump --gaps design.fasm     -- only what the decoder does not model
#include "lvs/tileconfig.hpp"

#include <iostream>

int main(int argc, char **argv)
{
    bool gaps = false;
    std::string path;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--gaps") gaps = true;
        else path = a;
    }
    if (path.empty()) {
        std::cerr << "usage: tiledump [--gaps] <design.fasm>\n";
        return 2;
    }
    try {
        lvs::DesignConfig dc = lvs::read_fasm(path);
        if (gaps) dc.dump_gaps(std::cout);
        else dc.dump(std::cout);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
