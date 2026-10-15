// The output pins of a GTXE2_CHANNEL transceiver.  The transceiver is not modelled -- what comes out of it is a serial link's worth of
// timing -- but the parallel side is Boolean as far as the fabric is concerned: RXDATA, RXCHARISK, RXRESETDONE and the rest are signals a
// design registers and branches on.  Left unmodelled they read as the interconnect pull-up on the extraction side and as free variables
// on the synthesis side, so every register that samples received data "differs".  Both sides therefore cut the block at these pins and
// the placement pairs the two cuts, exactly as for a DSP.  Shared by the extractor (which emits a stub with these pins), the equivalence
// checker (which cuts them) and the register map (which pairs them).
#pragma once
#include <set>
#include <string>

namespace lvs {

inline bool gtx_channel_out(const std::string &pin)
{
    static const std::set<std::string> outs = {
        "CPLLFBCLKLOST", "CPLLLOCK", "CPLLREFCLKLOST", "DMONITOROUT", "DRPDO", "DRPRDY", "EYESCANDATAERROR", "GTREFCLKMONITOR",
        "PCSRSVDOUT", "PHYSTATUS", "RXBUFSTATUS", "RXBYTEISALIGNED", "RXBYTEREALIGN", "RXCDRLOCK", "RXCHANBONDSEQ", "RXCHANISALIGNED",
        "RXCHANREALIGN", "RXCHARISCOMMA", "RXCHARISK", "RXCOMINITDET", "RXCOMMADET", "RXCOMSASDET", "RXCOMWAKEDET", "RXDATA",
        "RXDATAVALID", "RXDISPERR", "RXDLYSRESETDONE", "RXELECIDLE", "RXHEADER", "RXHEADERVALID", "RXMONITOROUT", "RXNOTINTABLE",
        "RXPHALIGNDONE", "RXPMARESETDONE", "RXPRBSERR", "RXQPISENN", "RXQPISENP", "RXRATEDONE", "RXRESETDONE", "RXSTARTOFSEQ",
        "RXSTATUS", "RXVALID", "TSTOUT", "TXBUFSTATUS", "TXCOMFINISH", "TXDLYSRESETDONE", "TXGEARBOXREADY", "TXPHALIGNDONE",
        "TXPHINITDONE", "TXPMARESETDONE", "TXQPISENN", "TXQPISENP", "TXRATEDONE", "TXRESETDONE"};
    return outs.count(pin) != 0;
}

}  // namespace lvs
