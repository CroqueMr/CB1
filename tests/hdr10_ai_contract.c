/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <string.h>
#include "cb1_circuit.h"
#if defined(CB1_EXPECT_HDR10_AI) && !defined(CB1_HAS_HDR10_AI)
#error "AI-enabled consumers require the matched native API"
#endif
#if !defined(CB1_EXPECT_HDR10_AI) && defined(CB1_HAS_HDR10_AI)
#error "Native consumers must not enable AI implicitly"
#endif

int main(void)
{
    const char *expected[] = {
        "CB1-DVDVR.1.0", "CB1-DVHDR10B.1.0", "CB1-DVHDR10X.1.0",
        "CB1-DVDVA.1.0", NULL, NULL, NULL,
        "CB1-HDR10DVR.1.0", "CB1-HDR10DVA.1.0", "CB1-DVDVMR.1.0", "CB1-DVDVAB.1.0",
        "CB1-DVDVMRI.1.0", "CB1-DVHDR10P.1.0", "CB1-HDR10PDVR.1.0",
        "CB1-DVSDR.1.0", "CB1-HDR10SDR.1.0", "CB1-HLGDVR.1.0", "CB1-DVLLDV.1.0",
        "CB1-HLGSDR.1.0", "CB1-SDRHDR10.1.0", "CB1-SDRDVR.1.0",
    };
    assert(strcmp(cb1_engine_version(), "CB1 1.1") == 0);
    assert(CB1_CIRCUIT_COUNT == (int)(sizeof(expected)/sizeof(*expected)));
    for (int i = 0; i < CB1_CIRCUIT_COUNT; ++i)
        if (expected[i]) assert(strcmp(cb1_circuit_version((enum cb1_circuit)i), expected[i]) == 0);
        else assert(cb1_circuit_version((enum cb1_circuit)i) == NULL);
    assert(cb1_circuit_version((enum cb1_circuit)-1) == NULL);
    assert(cb1_circuit_version(CB1_CIRCUIT_COUNT) == NULL);
    return 0;
}
