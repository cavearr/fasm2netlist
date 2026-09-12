// The DSP48E1's own interface, and how each port reaches the fabric.
//
// Kept beside src/bram_ports.hpp and for the same reason: the extractor, which
// turns a bitstream into DSP48E1 instances, and the tile model, which cuts one
// at its boundary, must agree exactly about what its ports are.  A second copy
// would be a second opinion, and the failure it produced -- two netlists
// disagreeing about one wire of one operand -- would look like a bug in the
// proof rather than in the table.
//
// Taken from the site pins prjxray records for the DSP48E1 site: 417 of them,
// which group into these ports.  Note CEA1/CEA2 and CEB1/CEB2, which are
// discrete scalar ports and not a bus -- there is no CEA0 -- so reading the
// trailing digit as an index would invent one and shift the other two.
#ifndef F2N_DSP_PORTS_HPP
#define F2N_DSP_PORTS_HPP

namespace dsp {

struct Port {
	const char* name;
	int width;  // 0 = scalar
	bool out;
};

inline constexpr Port kDsp48e1[] = {
    // operands and cascades in
    {"A", 30, false},      {"ACIN", 30, false},   {"B", 18, false},
    {"BCIN", 18, false},   {"C", 48, false},      {"D", 25, false},
    {"PCIN", 48, false},   {"CARRYCASCIN", 0, false}, {"CARRYIN", 0, false},
    {"MULTSIGNIN", 0, false},
    // control
    {"ALUMODE", 4, false}, {"CARRYINSEL", 3, false}, {"INMODE", 5, false},
    {"OPMODE", 7, false},  {"CLK", 0, false},
    // clock enables -- CEA1/CEA2 and CEB1/CEB2 are separate ports
    {"CEA1", 0, false},    {"CEA2", 0, false},    {"CEAD", 0, false},
    {"CEALUMODE", 0, false}, {"CEB1", 0, false},  {"CEB2", 0, false},
    {"CEC", 0, false},     {"CECARRYIN", 0, false}, {"CECTRL", 0, false},
    {"CED", 0, false},     {"CEINMODE", 0, false}, {"CEM", 0, false},
    {"CEP", 0, false},
    // resets
    {"RSTA", 0, false},    {"RSTALLCARRYIN", 0, false}, {"RSTALUMODE", 0, false},
    {"RSTB", 0, false},    {"RSTC", 0, false},    {"RSTCTRL", 0, false},
    {"RSTD", 0, false},    {"RSTINMODE", 0, false}, {"RSTM", 0, false},
    {"RSTP", 0, false},
    // results and cascades out -- these are what a boundary cut frees
    {"P", 48, true},       {"PCOUT", 48, true},   {"ACOUT", 30, true},
    {"BCOUT", 18, true},   {"CARRYOUT", 4, true}, {"CARRYCASCOUT", 0, true},
    {"MULTSIGNOUT", 0, true}, {"OVERFLOW", 0, true}, {"UNDERFLOW", 0, true},
    {"PATTERNDETECT", 0, true}, {"PATTERNBDETECT", 0, true},
};

// The data outputs: the pins a cut turns into free variables.  Everything else
// the block drives is a cascade or a flag, and is cut for the same reason --
// nothing here models what a DSP computes.
inline bool is_data_out(const char* pin)
{
	for (const auto& p : kDsp48e1)
		if (p.out && __builtin_strcmp(p.name, pin) == 0) return true;
	return false;
}

} // namespace dsp

#endif
