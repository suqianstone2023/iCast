/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the single-program cut-out. Run with "make test".
 *
 * The packets here are hand-built: a PAT with three programs, one PMT, and a
 * handful of PES packets on the PIDs a real transponder would use. No dongle,
 * no kernel, no network.
 */

#include "../src/tsfilter.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		if (cond) {                                                    \
			printf("  ok   ");                                     \
		} else {                                                       \
			printf("  FAIL ");                                     \
			failures++;                                            \
		}                                                              \
		printf(__VA_ARGS__);                                           \
		printf("\n");                                                  \
	} while (0)

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

static unsigned pid_of(const uint8_t *p)
{
	return ((unsigned)(p[1] & 0x1f) << 8) | p[2];
}

/* A payload-unit-start packet carrying a PSI section. */
static void put_section(uint8_t *p, unsigned pid, const uint8_t *sec, size_t n)
{
	memset(p, 0xff, TSF_PACKET);
	p[0] = 0x47;
	p[1] = (uint8_t)(0x40 | ((pid >> 8) & 0x1f));
	p[2] = (uint8_t)pid;
	p[3] = 0x10; /* payload only */
	p[4] = 0x00; /* pointer_field */
	memcpy(p + 5, sec, n);
}

/* A plain payload packet on one PID, distinguishable by its first byte. */
static void put_pes(uint8_t *p, unsigned pid, uint8_t tag)
{
	memset(p, 0x00, TSF_PACKET);
	p[0] = 0x47;
	p[1] = (uint8_t)((pid >> 8) & 0x1f);
	p[2] = (uint8_t)pid;
	p[3] = 0x10;
	p[4] = tag;
}

static void build_pat(uint8_t *sec)
{
	/* three programs: 100 -> 0x1000, 220 -> 0x0898, 221 -> 0x08a2 */
	memset(sec, 0, 32);
	sec[0] = 0x00;
	sec[1] = 0xb0;
	sec[2] = 9 + 4 * 3;
	sec[3] = 0x12;
	sec[4] = 0x34;
	sec[5] = 0xc1;
	sec[6] = 0;
	sec[7] = 0;
	sec[8] = 0x00;
	sec[9] = 100;
	sec[10] = 0xf0;
	sec[11] = 0x00;
	sec[12] = 0x00;
	sec[13] = 220;
	sec[14] = 0xe8;
	sec[15] = 0x98;
	sec[16] = 0x00;
	sec[17] = 221;
	sec[18] = 0xe8;
	sec[19] = 0xa2;
	{
		uint32_t c = crc32_mpeg(sec, 20);
		sec[20] = (uint8_t)(c >> 24);
		sec[21] = (uint8_t)(c >> 16);
		sec[22] = (uint8_t)(c >> 8);
		sec[23] = (uint8_t)c;
	}
}

static void build_pmt(uint8_t *sec)
{
	/* program 220: PCR 0x089a, H.264 0x089a, AC3 0x089b */
	memset(sec, 0, 32);
	sec[0] = 0x02;
	sec[1] = 0xb0;
	sec[2] = 13 + 5 * 2;
	sec[3] = 0x00;
	sec[4] = 220;
	sec[5] = 0xc1;
	sec[6] = 0x00;
	sec[7] = 0x00;
	sec[8] = 0xe8;
	sec[9] = 0x9a;
	sec[10] = 0xf0;
	sec[11] = 0x00;
	sec[12] = 0x1b;
	sec[13] = 0xe8;
	sec[14] = 0x9a;
	sec[15] = 0xf0;
	sec[16] = 0x00;
	sec[17] = 0x81;
	sec[18] = 0xe8;
	sec[19] = 0x9b;
	sec[20] = 0xf0;
	sec[21] = 0x00;
	{
		uint32_t c = crc32_mpeg(sec, 22);
		sec[22] = (uint8_t)(c >> 24);
		sec[23] = (uint8_t)(c >> 16);
		sec[24] = (uint8_t)(c >> 8);
		sec[25] = (uint8_t)c;
	}
}

static void test_extracts_one_program(void)
{
	static struct ts_filter f;
	uint8_t buf[TSF_PACKET * 8];
	uint8_t pat[32], pmt[32];
	size_t n, i;
	int saw_pat = 0, saw_pmt = 0, saw_es = 0, saw_other = 0;
	uint32_t crc;

	printf("program 220 out of a three-program PAT:\n");

	build_pat(pat);
	build_pmt(pmt);

	put_section(buf + 0 * TSF_PACKET, 0x0000, pat, 24);
	put_pes(buf + 1 * TSF_PACKET, 0x1001, 0x11); /* program 100 video */
	put_section(buf + 2 * TSF_PACKET, 0x0898, pmt, 26);
	put_pes(buf + 3 * TSF_PACKET, 0x089a, 0x22); /* video */
	put_pes(buf + 4 * TSF_PACKET, 0x089b, 0x33); /* audio */
	put_pes(buf + 5 * TSF_PACKET, 0x1000, 0x44); /* program 100 */
	put_pes(buf + 6 * TSF_PACKET, 0x08ac, 0x55); /* program 222 pmt */
	put_pes(buf + 7 * TSF_PACKET, 0x1fff, 0x66); /* null packets */

	ts_filter_init(&f, 220);
	n = ts_filter_apply(&f, buf, sizeof(buf));

	for (i = 0; i < n; i += TSF_PACKET) {
		unsigned pid = pid_of(buf + i);

		if (pid == 0x0000)
			saw_pat = 1;
		else if (pid == 0x0898)
			saw_pmt = 1;
		else if (pid == 0x089a || pid == 0x089b)
			saw_es = 1;
		else
			saw_other = 1;
	}

	CHECK(n == 4 * TSF_PACKET, "kept 4 of 8 packets (got %zu)", n / TSF_PACKET);
	CHECK(saw_pat, "PAT is still there (players need it)");
	CHECK(saw_pmt, "our PMT survived");
	CHECK(saw_es, "video and audio PIDs survived");
	CHECK(!saw_other, "other programs and null packets were dropped");

	/* the PAT must now advertise exactly one program, with a valid CRC */
	{
		uint8_t *p = NULL;

		for (i = 0; i < n; i += TSF_PACKET)
			if (pid_of(buf + i) == 0x0000)
				p = buf + i;
		CHECK(p != NULL, "found the PAT packet");
		if (p) {
			uint8_t *sec = p + 5;

			CHECK(sec[0] == 0x00, "PAT table_id unchanged");
			CHECK(sec[2] == 13, "section_length rewritten to 13 (got %u)",
			      sec[2]);
			CHECK(((sec[8] << 8) | sec[9]) == 220,
			      "PAT lists program 220 (got %u)",
			      (sec[8] << 8) | sec[9]);
			CHECK((((sec[10] & 0x1f) << 8) | sec[11]) == 0x0898,
			      "PAT points at PMT 0x0898");
			crc = crc32_mpeg(sec, 12);
			CHECK(sec[12] == (uint8_t)(crc >> 24) &&
				      sec[13] == (uint8_t)(crc >> 16) &&
				      sec[14] == (uint8_t)(crc >> 8) &&
				      sec[15] == (uint8_t)crc,
			      "PAT CRC recomputed");
		}
	}
}

static void test_passthrough(void)
{
	static struct ts_filter f;
	uint8_t buf[TSF_PACKET * 3];

	printf("program 0 means let everything through:\n");
	put_pes(buf + 0 * TSF_PACKET, 0x0100, 1);
	put_pes(buf + 1 * TSF_PACKET, 0x0200, 2);
	put_pes(buf + 2 * TSF_PACKET, 0x0300, 3);

	ts_filter_init(&f, 0);
	CHECK(ts_filter_apply(&f, buf, sizeof(buf)) == sizeof(buf),
	      "all packets kept");
}

static void test_unknown_program(void)
{
	static struct ts_filter f;
	uint8_t buf[TSF_PACKET * 2];
	uint8_t pat[32];

	printf("a program that is not in the PAT:\n");
	build_pat(pat);
	put_section(buf + 0 * TSF_PACKET, 0x0000, pat, 24);
	put_pes(buf + 1 * TSF_PACKET, 0x089a, 0x22);

	ts_filter_init(&f, 999);
	/* PAT only, nothing else: better an empty stream than the wrong one */
	CHECK(ts_filter_apply(&f, buf, sizeof(buf)) == TSF_PACKET,
	      "only the PAT survives");
}

int main(void)
{
	test_extracts_one_program();
	test_passthrough();
	test_unknown_program();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
