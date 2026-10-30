/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cut one program out of a multiplex.
 *
 * A DVB-C transponder carries every service at once - here about 38 Mbit/s of
 * five programs. That is fine on a file or with ffmpeg, but a settop box
 * demuxer handed the whole mux over UDP has to allocate receive buffers for
 * the peak rate and tends to stall before it ever prerolls. Handing each
 * channel its own program costs a PID mask and fixes it: the PAT is rewritten
 * to list the single program (with a fresh CRC, or nothing will parse it) and
 * everything else is dropped.
 *
 * Kept free of I/O so tests/tsfilter_test.c can exercise it on synthetic data.
 */

#ifndef ICAST_TSFILTER_H
#define ICAST_TSFILTER_H

#include <stddef.h>
#include <stdint.h>

#define TSF_PACKET 188
#define TSF_MAX_ES 32

struct ts_filter {
	int want;          /* program number to keep, 0 = pass everything */
	int have_pat;      /* learned pmt_pid */
	int ready;         /* mask is complete, PAT is being rewritten */
	uint16_t pmt_pid;
	uint16_t pcr_pid;
	int es_count;
	uint16_t es_pid[TSF_MAX_ES];
	uint8_t mask[8192 / 8];
};

void ts_filter_init(struct ts_filter *f, int program);

/*
 * Filter n bytes of transport stream in place and return the new length.
 * n must be a whole number of 188 byte packets; callers buffer the tail.
 */
size_t ts_filter_apply(struct ts_filter *f, uint8_t *buf, size_t n);

#endif /* ICAST_TSFILTER_H */
