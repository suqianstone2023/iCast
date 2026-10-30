/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tests for the transport stream sink, runnable without the dongle:
 *
 *   make test
 *
 * Covers the two things that silently break a stream downstream:
 *   - packet alignment when the first bytes read are garbage, or when a
 *     packet straddles two USB reads
 *   - UDP datagram sizing (must be a whole number of 188 byte packets,
 *     otherwise tvheadend drops the tail of every datagram)
 */

#include "../src/sink.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

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

/* Build a synthetic stream: `garbage` junk bytes, then `packets` TS packets. */
static size_t build_stream(uint8_t *out, size_t garbage, size_t packets)
{
	size_t i, p;

	for (i = 0; i < garbage; i++)
		out[i] = 0x11;

	for (p = 0; p < packets; p++) {
		uint8_t *pkt = out + garbage + p * TS_PACKET;

		pkt[0] = 0x47;
		pkt[1] = (uint8_t)(p & 0xFF);
		pkt[2] = (uint8_t)((p >> 8) & 0xFF);
		for (i = 3; i < TS_PACKET; i++)
			pkt[i] = (uint8_t)(i + p);
	}

	return garbage + packets * TS_PACKET;
}

static int all_packets(const uint8_t *p, size_t len)
{
	size_t i;

	if (len % TS_PACKET)
		return 0;
	for (i = 0; i < len; i += TS_PACKET)
		if (p[i] != 0x47)
			return 0;
	return 1;
}

static void test_file_alignment(void)
{
	const size_t garbage = 37, packets = 20;
	uint8_t in[8192], out[8192];
	char path[] = "/tmp/icast-sink-XXXXXX";
	size_t total, got, off, i;
	struct sink s;
	FILE *f;
	int fd;
	int rc;

	printf("file output, 37 leading junk bytes, 97 byte reads:\n");

	total = build_stream(in, garbage, packets);
	fd = mkstemp(path);
	if (fd < 0) {
		printf("  FAIL mkstemp\n");
		failures++;
		return;
	}

	rc = sink_open(&s, path, NULL);
	CHECK(rc == 0, "sink_open(%s)", path);

	for (off = 0; off < total; off += 97) {
		size_t n = total - off < 97 ? total - off : 97;

		if (sink_write(&s, in + off, n) < 0) {
			printf("  FAIL sink_write\n");
			failures++;
			break;
		}
	}
	sink_flush(&s);
	sink_close(&s);
	close(fd);

	f = fopen(path, "rb");
	got = f ? fread(out, 1, sizeof(out), f) : 0;
	if (f)
		fclose(f);
	unlink(path);

	CHECK(got == packets * TS_PACKET, "dropped the junk prefix: %zu of %zu bytes",
	      got, (size_t)(packets * TS_PACKET));
	CHECK(all_packets(out, got), "every 188 byte slot starts with 0x47");

	for (i = 0; i + TS_PACKET <= got; i += TS_PACKET) {
		if (out[i + 1] != (uint8_t)(i / TS_PACKET)) {
			CHECK(0, "packet %zu out of order", i / TS_PACKET);
			return;
		}
	}
	CHECK(1, "packet contents survived the re-chunking intact");
}

static void test_udp_datagram_size(void)
{
	const size_t packets = 20;
	uint8_t in[8192];
	uint8_t rx[4096];
	char addr[64];
	struct sockaddr_in me;
	socklen_t mlen = sizeof(me);
	int sock;
	int port;
	unsigned long long bytes = 0;
	int datagrams = 0;
	int bad = 0;
	struct sink s;
	size_t total, off;

	printf("udp output, 20 packets in 53 byte reads:\n");

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		printf("  FAIL socket\n");
		failures++;
		return;
	}

	memset(&me, 0, sizeof(me));
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0 ||
	    getsockname(sock, (struct sockaddr *)&me, &mlen) < 0) {
		printf("  FAIL bind\n");
		failures++;
		close(sock);
		return;
	}
	port = ntohs(me.sin_port);

	/* A short receive timeout keeps the test from hanging if nothing lands. */
	{
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	snprintf(addr, sizeof(addr), "udp://127.0.0.1:%d", port);
	CHECK(sink_open(&s, addr, NULL) == 0, "sink_open(%s)", addr);

	total = build_stream(in, 0, packets);
	for (off = 0; off < total; off += 53) {
		size_t n = total - off < 53 ? total - off : 53;

		if (sink_write(&s, in + off, n) < 0) {
			printf("  FAIL sink_write\n");
			failures++;
			break;
		}
	}

	for (;;) {
		ssize_t r = recv(sock, rx, sizeof(rx), 0);

		if (r <= 0)
			break;
		datagrams++;
		bytes += (unsigned long long)r;
		if ((size_t)r % (TS_DATAGRAM_PACKETS * TS_PACKET) != 0)
			bad++;
		if (rx[0] != 0x47)
			bad++;
	}

	sink_close(&s);
	close(sock);

	CHECK(datagrams == 2, "20 packets -> 2 datagrams (got %d)", datagrams);
	CHECK(bad == 0, "every datagram is a whole number of 1316 byte groups");
	CHECK(bytes == 2ull * TS_DATAGRAM_PACKETS * TS_PACKET,
	      "bytes on the wire: %llu", bytes);
}

/*
 * The device hands over up to 128 KB per bulk read, so a single sink_write()
 * call can hold a whole buffer of packets. Sending that as one datagram fails
 * with EMSGSIZE - which is exactly what happened on the first real capture.
 */
static void test_udp_bulk_write(void)
{
	const size_t packets = 100;
	uint8_t in[100 * TS_PACKET];
	uint8_t rx[4096];
	char addr[64];
	struct sockaddr_in me;
	socklen_t mlen = sizeof(me);
	int sock, port, rc;
	unsigned long long bytes = 0;
	int datagrams = 0;
	int too_big = 0;
	struct sink s;
	size_t total;

	printf("udp output, %zu packets in one write:\n", packets);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		printf("  FAIL socket\n");
		failures++;
		return;
	}

	memset(&me, 0, sizeof(me));
	me.sin_family = AF_INET;
	me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0 ||
	    getsockname(sock, (struct sockaddr *)&me, &mlen) < 0) {
		printf("  FAIL bind\n");
		failures++;
		close(sock);
		return;
	}
	port = ntohs(me.sin_port);

	{
		struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}

	snprintf(addr, sizeof(addr), "udp://127.0.0.1:%d", port);
	CHECK(sink_open(&s, addr, NULL) == 0, "sink_open(%s)", addr);

	total = build_stream(in, 0, packets);
	rc = sink_write(&s, in, total);
	CHECK(rc == 0, "one %zu byte write succeeds (got %d)", total, rc);

	for (;;) {
		ssize_t r = recv(sock, rx, sizeof(rx), 0);

		if (r <= 0)
			break;
		datagrams++;
		bytes += (unsigned long long)r;
		if ((size_t)r > TS_DATAGRAM_PACKETS * TS_PACKET)
			too_big++;
		if (rx[0] != 0x47)
			too_big++;
	}

	sink_close(&s);
	close(sock);

	/* 100 packets = 14 whole 1316 byte groups, 376 bytes left buffered. */
	CHECK(datagrams == 14, "split into 14 datagrams (got %d)", datagrams);
	CHECK(too_big == 0, "no datagram exceeds 1316 bytes and all start with 0x47");
	CHECK(bytes == 14ull * TS_DATAGRAM_PACKETS * TS_PACKET,
	      "bytes on the wire: %llu", bytes);
}

int main(void)
{
	test_file_alignment();
	test_udp_datagram_size();
	test_udp_bulk_write();

	if (failures) {
		printf("\n%d check(s) failed\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}

/* sink.c reports its target when this is set; the tests do not need it. */
int icast_debug;
