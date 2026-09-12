// The block RAM primitives' own interfaces, and how each port reaches the
// fabric.
//
// Two tools need this and they need exactly the same answer: the extractor,
// which turns a bitstream into RAMB18E1/RAMB36E1 instances, and the tile model,
// which has to instantiate the same primitives over the same site pins for the
// two to be comparable at all.  A second copy of these tables would be a second
// opinion about what a block RAM's ports are, and the failure it produced --
// two netlists that disagree about one wire of one address -- would look like a
// bug in the proof rather than in the table.
#ifndef F2N_BRAM_PORTS_HPP
#define F2N_BRAM_PORTS_HPP

#include <string>
#include <utility>

namespace bram {

struct Port {
	const char* name;
	int width;  // 0 = scalar
	bool out;
};

// RAMB18E1's own interface. The site carries wider WEA/WEBWE and a set of
// FIFO-only pins; only these are ports of the primitive.
inline constexpr Port kRamb18[] = {
    {"ADDRARDADDR", 14, false}, {"ADDRBWRADDR", 14, false}, {"CLKARDCLK", 0, false},
    {"CLKBWRCLK", 0, false},    {"DIADI", 16, false},       {"DIBDI", 16, false},
    {"DIPADIP", 2, false},      {"DIPBDIP", 2, false},      {"DOADO", 16, true},
    {"DOBDO", 16, true},        {"DOPADOP", 2, true},       {"DOPBDOP", 2, true},
    {"ENARDEN", 0, false},      {"ENBWREN", 0, false},      {"REGCEAREGCE", 0, false},
    {"REGCEB", 0, false},       {"REGCLKARDRCLK", 0, false}, {"REGCLKB", 0, false},
    {"RSTRAMARSTRAM", 0, false}, {"RSTRAMB", 0, false},     {"RSTREGARSTREG", 0, false},
    {"RSTREGB", 0, false},      {"WEA", 2, false},          {"WEBWE", 4, false},
};

// RAMB36E1's interface, paired with the RAMBFIFO36E1 site pin it comes from.
// Signals that exist twice on the site (once per 18Kb half, suffixed L and U)
// are one port on the primitive: in 36Kb mode the halves are driven together,
// so the L copy is taken and the U copy is checked against it.
struct Port36 {
	const char* port;
	const char* pin;  // site pin base name (L variant where the pair exists)
	const char* pinU; // the U variant to cross-check, or nullptr
	int width;
	bool out;
	// A 36Kb DATA port is not one signal reaching both halves, it is one
	// signal SPLIT between them: even bits leave on the lower half's pins and
	// odd bits on the upper half's, so bit i sits on <pin>L<i/2> or
	// <pin>U<i/2>.  Control and address are the other case -- both halves see
	// the same value, which is why they carry a cross-check instead.
	bool interleaved;
};
inline constexpr Port36 kRamb36[] = {
    {"ADDRARDADDR", "ADDRARDADDRL", "ADDRARDADDRU", 16, false, false},
    {"ADDRBWRADDR", "ADDRBWRADDRL", "ADDRBWRADDRU", 16, false, false},
    {"CASCADEINA", "CASCADEINA", nullptr, 0, false, false},
    {"CASCADEINB", "CASCADEINB", nullptr, 0, false, false},
    {"CASCADEOUTA", "CASCADEOUTA", nullptr, 0, true, false},
    {"CASCADEOUTB", "CASCADEOUTB", nullptr, 0, true, false},
    {"CLKARDCLK", "CLKARDCLKL", "CLKARDCLKU", 0, false, false},
    {"CLKBWRCLK", "CLKBWRCLKL", "CLKBWRCLKU", 0, false, false},
    {"DBITERR", "DBITERR", nullptr, 0, true, false},
    {"DIADI", "DIADI", nullptr, 32, false, true},
    {"DIBDI", "DIBDI", nullptr, 32, false, true},
    {"DIPADIP", "DIPADIP", nullptr, 4, false, true},
    {"DIPBDIP", "DIPBDIP", nullptr, 4, false, true},
    {"DOADO", "DOADO", nullptr, 32, true, true},
    {"DOBDO", "DOBDO", nullptr, 32, true, true},
    {"DOPADOP", "DOPADOP", nullptr, 4, true, true},
    {"DOPBDOP", "DOPBDOP", nullptr, 4, true, true},
    {"ECCPARITY", "ECCPARITY", nullptr, 8, true, false},
    {"ENARDEN", "ENARDENL", "ENARDENU", 0, false, false},
    {"ENBWREN", "ENBWRENL", "ENBWRENU", 0, false, false},
    {"INJECTDBITERR", "INJECTDBITERR", nullptr, 0, false, false},
    {"INJECTSBITERR", "INJECTSBITERR", nullptr, 0, false, false},
    {"REGCEAREGCE", "REGCEAREGCEL", "REGCEAREGCEU", 0, false, false},
    {"REGCEB", "REGCEBL", "REGCEBU", 0, false, false},
    {"REGCLKARDRCLK", "REGCLKARDRCLKL", "REGCLKARDRCLKU", 0, false, false},
    {"REGCLKB", "REGCLKBL", "REGCLKBU", 0, false, false},
    // prjxray spells the lower RSTRAMARSTRAM pin with a trailing "RST"
    {"RSTRAMARSTRAM", "RSTRAMARSTRAMLRST", "RSTRAMARSTRAMU", 0, false, false},
    {"RSTRAMB", "RSTRAMBL", "RSTRAMBU", 0, false, false},
    {"RSTREGARSTREG", "RSTREGARSTREGL", "RSTREGARSTREGU", 0, false, false},
    {"RSTREGB", "RSTREGBL", "RSTREGBU", 0, false, false},
    {"SBITERR", "SBITERR", nullptr, 0, true, false},
    {"WEA", "WEAL", "WEAU", 4, false, false},
    {"WEBWE", "WEBWEL", "WEBWEU", 8, false, false},
};

// The control inputs that have an inversion bit of their own.  prjxray stores
// the attribute complemented, so an ABSENT "ZINV_<pin>" tag means the pin IS
// inverted -- which is what an unused control pin looks like: nothing routes
// to it, the interconnect holds it high, and the inversion makes the site see
// the nought the design asked for.  Reading this list two different ways in
// two different tools would put a reset on a memory in one netlist and not the
// other, which is exactly the kind of difference a comparison exists to find.
inline constexpr const char* kInvertible[] = {
    "CLKARDCLK", "CLKBWRCLK", "ENARDEN",       "ENBWREN", "REGCLKARDRCLK",
    "REGCLKB",   "RSTRAMB",   "RSTRAMARSTRAM", "RSTREGB", "RSTREGARSTREG",
};

// The site pin(s) carrying bit `i` of a 36Kb port: the one it lives on, and
// where the two 18Kb halves are driven together, the second copy to check it
// against.  Getting this wrong does not produce a wrong connection, it
// produces NO connection -- the site has no pin of that name -- so a data port
// asked for by the plain name simply vanishes from the netlist, which is
// exactly what happened to every RAMB36's data until the equivalence check on
// the LiteX SoC noticed the fabric reading a ROM's address where the synthesis
// read its data.
inline std::pair<std::string, std::string> pins36(const Port36& p, int i) {
	if (p.interleaved)
		return {std::string(p.pin) + (i % 2 ? "U" : "L") + std::to_string(i / 2), std::string()};
	std::string suffix = p.width == 0 ? std::string() : std::to_string(i);
	return {p.pin + suffix, p.pinU ? p.pinU + suffix : std::string()};
}

} // namespace bram

#endif
