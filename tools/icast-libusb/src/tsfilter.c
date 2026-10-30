/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Cut one program out of a multiplex - see tsfilter.h for why.
 */

#include "tsfilter.h"

#include <string.h>

#define PID_PAT 0x0000
#define PID_SDT 0x0011

static int bit_get(const uint8_t *m, unsigned pid)
{
	return m[pid >> 3] & (1u << (pid & 7));
}

static void allow(struct ts_filter *f, unsigned pid)
{
	if (pid < 0x2000)
		f->mask[pid >> 3] |= (uint8_t)(1u << (pid & 7));
}

/* MPEG-2 CRC32: no reflection, no final xor. */
static uint32_t crc32_mpeg(const uint8_t *d, size_t n)
{
	uint32_t crc = 0xffffffffu;
	size_t i;
	int b;

	for (i = 0; i < n; i++) {
		crc ^= (uint32_t)d[i] << 24;
		for (b = 0; b < 8; b++)
			crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u
						  : crc << 1;
	}
	return crc;
}

/* Locate the PSI section that starts in this packet, if it starts here. */
static const uint8_t *section_of(const uint8_t *p, size_t *len)
{
	int afc = (p[3] >> 4) & 3;
	size_t off = 4;
	size_t ptr;

	if (!(p[1] & 0x40)) /* payload_unit_start_indicator */
		return NULL;
	if (afc & 2)
		off += 1u + p[4];
	if (!(afc & 1) || off + 1 >= TSF_PACKET)
		return NULL;

	ptr = p[off];
	if (off + 1 + ptr >= TSF_PACKET)
		return NULL;

	*len = TSF_PACKET - (off + 1 + ptr);
	return p + off + 1 + ptr;
}

static void parse_pat(struct ts_filter *f, const uint8_t *sec, size_t len)
{
	size_t seclen, end, i;

	if (len < 12 || sec[0] != 0x00)
		return;

	seclen = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];
	if (3 + seclen > len || seclen < 9)
		return;

	end = 3 + seclen - 4; /* exclude CRC */
	for (i = 8; i + 4 <= end; i += 4) {
		unsigned prog = ((unsigned)sec[i] << 8) | sec[i + 1];
		unsigned pid = ((unsigned)(sec[i + 2] & 0x1f) << 8) | sec[i + 3];

		if (prog && (int)prog == f->want) {
			f->pmt_pid = (uint16_t)pid;
			f->have_pat = 1;
			allow(f, pid);
			return;
		}
	}
}

static void parse_pmt(struct ts_filter *f, const uint8_t *sec, size_t len)
{
	size_t seclen, end, i, ilen;

	if (len < 16 || sec[0] != 0x02)
		return;

	seclen = ((size_t)(sec[1] & 0x0f) << 8) | sec[2];
	if (3 + seclen > len || seclen < 13)
		return;

	end = 3 + seclen - 4;
	f->pcr_pid = (uint16_t)(((unsigned)(sec[8] & 0x1f) << 8) | sec[9]);
	allow(f, f->pcr_pid);

	ilen = ((size_t)(sec[10] & 0x0f) << 8) | sec[11];
	i = 12 + ilen;

	while (i + 5 <= end) {
		unsigned pid = ((unsigned)(sec[i + 1] & 0x1f) << 8) | sec[i + 2];
		size_t elen = ((size_t)(sec[i + 3] & 0x0f) << 8) | sec[i + 4];

		allow(f, pid);
		if (f->es_count < TSF_MAX_ES)
			f->es_pid[f->es_count++] = (uint16_t)pid;
		i += 5 + elen;
	}

	f->ready = 1;
}

/*
 * Replace the program list with just ours. A PAT still advertising five
 * programs whose PMTs never arrive keeps a demuxer waiting, and the CRC has
 * to be recomputed or the section is thrown away as corrupt.
 */
static void rewrite_pat(struct ts_filter *f, uint8_t *p)
{
	int afc = (p[3] >> 4) & 3;
	size_t off = 4;
	size_t ptr;
	uint8_t *sec;
	uint32_t crc;
	size_t i;

	if (afc & 2)
		off += 1u + p[4];
	if (!(afc & 1) || off + 1 >= TSF_PACKET)
		return;

	ptr = p[off];
	sec = p + off + 1 + ptr;
	if (sec + 16 > p + TSF_PACKET || sec[0] != 0x00)
		return;

	sec[1] = 0xb0;
	sec[2] = 13; /* tsid(2) ver(1) sec(1) last(1) entry(4) crc(4) */
	sec[5] = 0xc1;
	sec[6] = 0;
	sec[7] = 0;
	sec[8] = (uint8_t)(f->want >> 8);
	sec[9] = (uint8_t)(f->want & 0xff);
	sec[10] = (uint8_t)(0xe0 | (f->pmt_pid >> 8));
	sec[11] = (uint8_t)(f->pmt_pid & 0xff);

	crc = crc32_mpeg(sec, 12);
	sec[12] = (uint8_t)(crc >> 24);
	sec[13] = (uint8_t)(crc >> 16);
	sec[14] = (uint8_t)(crc >> 8);
	sec[15] = (uint8_t)crc;

	/* Stuffing: 0xff, never 0x47, so sync stays unambiguous. */
	for (i = 16; sec + i < p + TSF_PACKET; i++)
		sec[i] = 0xff;
}

void ts_filter_init(struct ts_filter *f, int program)
{
	memset(f, 0, sizeof(*f));
	f->want = program;
	allow(f, PID_PAT);
	allow(f, PID_SDT);
}

size_t ts_filter_apply(struct ts_filter *f, uint8_t *buf, size_t n)
{
	size_t r = 0, w = 0;

	if (!f || f->want <= 0)
		return n;

	/*
	 * Learn from this buffer before filtering it. The PAT arrives ahead of
	 * the PMT, so a single pass would let the first PAT through unchanged
	 * and a player that latches onto it would wait for four PMTs that never
	 * come.
	 */
	if (!f->ready) {
		for (r = 0; r + TSF_PACKET <= n; r += TSF_PACKET) {
			uint8_t *p = buf + r;
			const uint8_t *sec;
			size_t len;
			unsigned pid;

			if (p[0] != 0x47)
				continue;

			pid = ((unsigned)(p[1] & 0x1f) << 8) | p[2];
			sec = section_of(p, &len);
			if (!sec)
				continue;

			if (pid == PID_PAT)
				parse_pat(f, sec, len);
			else if (f->have_pat && pid == f->pmt_pid)
				parse_pmt(f, sec, len);
		}
		r = 0;
	}

	while (r + TSF_PACKET <= n) {
		uint8_t *p = buf + r;
		unsigned pid;
		int keep;

		if (p[0] != 0x47) { /* lost sync: drop it rather than pass junk */
			r += TSF_PACKET;
			continue;
		}

		pid = ((unsigned)(p[1] & 0x1f) << 8) | p[2];

		if (f->ready) {
			if (pid == PID_PAT)
				rewrite_pat(f, p);
			keep = bit_get(f->mask, pid);
		} else {
			/* still learning: carry the tables, drop everything else */
			keep = pid == PID_PAT || pid == PID_SDT ||
			       (f->have_pat && pid == f->pmt_pid);
		}

		if (keep) {
			if (w != r)
				memmove(buf + w, p, TSF_PACKET);
			w += TSF_PACKET;
		}
		r += TSF_PACKET;
	}

	return w;
}
