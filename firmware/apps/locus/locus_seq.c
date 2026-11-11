/* SPDX-License-Identifier: GPL-3.0-only */
/* LOCUS's sequencer and chance triggers, once a tick (48 frames) from the engine, in the audio interrupt. */
#include "locus.h"

uint32_t lseq_tick(int32_t* note)
{
    (void)note;
    return 0;
}
