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
};
inline constexpr Port36 kRamb36[] = {
    {"ADDRARDADDR", "ADDRARDADDRL", "ADDRARDADDRU", 16, false},
    {"ADDRBWRADDR", "ADDRBWRADDRL", "ADDRBWRADDRU", 16, false},
    {"CASCADEINA", "CASCADEINA", nullptr, 0, false},
    {"CASCADEINB", "CASCADEINB", nullptr, 0, false},
    {"CASCADEOUTA", "CASCADEOUTA", nullptr, 0, true},
    {"CASCADEOUTB", "CASCADEOUTB", nullptr, 0, true},
    {"CLKARDCLK", "CLKARDCLKL", "CLKARDCLKU", 0, false},
    {"CLKBWRCLK", "CLKBWRCLKL", "CLKBWRCLKU", 0, false},
    {"DBITERR", "DBITERR", nullptr, 0, true},
    {"DIADI", "DIADI", nullptr, 32, false},
    {"DIBDI", "DIBDI", nullptr, 32, false},
    {"DIPADIP", "DIPADIP", nullptr, 4, false},
    {"DIPBDIP", "DIPBDIP", nullptr, 4, false},
    {"DOADO", "DOADO", nullptr, 32, true},
    {"DOBDO", "DOBDO", nullptr, 32, true},
    {"DOPADOP", "DOPADOP", nullptr, 4, true},
    {"DOPBDOP", "DOPBDOP", nullptr, 4, true},
    {"ECCPARITY", "ECCPARITY", nullptr, 8, true},
    {"ENARDEN", "ENARDENL", "ENARDENU", 0, false},
    {"ENBWREN", "ENBWRENL", "ENBWRENU", 0, false},
    {"INJECTDBITERR", "INJECTDBITERR", nullptr, 0, false},
    {"INJECTSBITERR", "INJECTSBITERR", nullptr, 0, false},
    {"REGCEAREGCE", "REGCEAREGCEL", "REGCEAREGCEU", 0, false},
    {"REGCEB", "REGCEBL", "REGCEBU", 0, false},
    {"REGCLKARDRCLK", "REGCLKARDRCLKL", "REGCLKARDRCLKU", 0, false},
    {"REGCLKB", "REGCLKBL", "REGCLKBU", 0, false},
    // prjxray spells the lower RSTRAMARSTRAM pin with a trailing "RST"
    {"RSTRAMARSTRAM", "RSTRAMARSTRAMLRST", "RSTRAMARSTRAMU", 0, false},
    {"RSTRAMB", "RSTRAMBL", "RSTRAMBU", 0, false},
    {"RSTREGARSTREG", "RSTREGARSTREGL", "RSTREGARSTREGU", 0, false},
    {"RSTREGB", "RSTREGBL", "RSTREGBU", 0, false},
    {"SBITERR", "SBITERR", nullptr, 0, true},
    {"WEA", "WEAL", "WEAU", 4, false},
    {"WEBWE", "WEBWEL", "WEBWEU", 8, false},
};

} // namespace bram

#endif
