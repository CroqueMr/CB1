/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CB1_SDR_EXPANSION_H
#define CB1_SDR_EXPANSION_H
#include "cb1_hdr10_ai.h"
#include "dvbridge_policy.h"

struct cb1_sdr_expansion;
bool cb1_sdr_expansion_supported(pl_gpu gpu, bool dv);
void cb1_sdr_expansion_destroy(struct cb1_sdr_expansion **ctx);
enum cb1_ai_status cb1_sdr_expansion_prepare(struct cb1_sdr_expansion **ctx,
    pl_gpu gpu, pl_renderer renderer, const struct dvbridge_identity *id,
    const struct pl_frame *source, double pts, bool dv, struct pl_frame *pq,
    struct cb1_ai_result *description);
bool cb1_sdr_expansion_matches_dv(const struct cb1_sdr_expansion *ctx,
    const struct cb1_ai_result *description, const struct pl_frame *source);
const struct pl_frame *cb1_sdr_expansion_hdr10(const struct cb1_sdr_expansion *ctx,
    const struct dvbridge_identity *id);
#endif
