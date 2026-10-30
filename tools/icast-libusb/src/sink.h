/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transport stream sink: file / stdout / UDP output, with MPEG-TS packet
 * alignment.
 *
 * Keeping this separate from the CLI makes it testable without the dongle -
 * see tests/sink_test.c.
 */

#ifndef ICAST_SINK_H
#define ICAST_SINK_H

#include <stddef.h>
#include <stdint.h>

struct ts_filter;

#define TS_PACKET 188
#define TS_DATAGRAM_PACKETS 7 /* 1316 byte datagrams, the usual convention */

struct sink {
	int is_udp;
	int fd;
	int sock;
	uint32_t dst_addr;
	uint16_t dst_port;
	uint8_t *buf;
	size_t len;
	int synced;
	int dbg_dst;
	struct ts_filter *filter; /* optional: keep a single program only */
	uint32_t exp_addr;   /* what sink_open parsed, to catch corruption */
	uint16_t exp_port;
	unsigned long long bytes;
	unsigned long long datagrams;
	unsigned long long dropped;   /* datagrams the kernel refused, no listener */
};

/* out is "-" for stdout, "udp://host:port" or a file path. filter may be NULL. */
int sink_open(struct sink *s, const char *out, struct ts_filter *filter);

/* Feed raw bytes read from the device. */
int sink_write(struct sink *s, const uint8_t *p, size_t n);

/* Emit whatever whole packets are buffered. */
int sink_flush(struct sink *s);

void sink_close(struct sink *s);

#endif /* ICAST_SINK_H */
