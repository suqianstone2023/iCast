/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transport stream sink: file / stdout / UDP output, with MPEG-TS packet
 * alignment.
 */

#include "sink.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define SINK_CAP (256 * 1024)

int sink_open(struct sink *s, const char *out)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->sock = -1;

	s->buf = malloc(SINK_CAP);
	if (!s->buf)
		return -ENOMEM;

	if (strncmp(out, "udp://", 6) == 0) {
		char host[64];
		const char *p = out + 6;
		const char *colon;

		/* udp://@239.1.1.1:5000 is the form tvheadend prints. */
		if (*p == '@')
			p++;

		colon = strrchr(p, ':');
		if (!colon || (size_t)(colon - p) >= sizeof(host) || colon == p) {
			fprintf(stderr, "icast: bad udp address '%s'\n", out);
			return -EINVAL;
		}
		memcpy(host, p, (size_t)(colon - p));
		host[colon - p] = '\0';

		s->sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (s->sock < 0)
			return -errno;

		if (inet_pton(AF_INET, host, &s->dst_addr) != 1) {
			fprintf(stderr, "icast: bad udp host '%s'\n", host);
			return -EINVAL;
		}
		s->dst_port = (uint16_t)atoi(colon + 1);
		s->is_udp = 1;
		return 0;
	}

	if (strcmp(out, "-") == 0) {
		s->fd = STDOUT_FILENO;
		return 0;
	}

	s->fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (s->fd < 0) {
		fprintf(stderr, "icast: cannot open '%s': %s\n", out,
			strerror(errno));
		return -errno;
	}
	return 0;
}

/*
 * Find the first position where three consecutive 188 byte headers line up.
 * Dropping the leading partial packet avoids handing a receiver a stream that
 * starts mid-packet.
 */
static int sink_sync(struct sink *s)
{
	size_t i;

	for (i = 0; i + 3 * TS_PACKET <= s->len; i++) {
		if (s->buf[i] != 0x47)
			continue;
		if (s->buf[i + TS_PACKET] != 0x47)
			continue;
		if (s->buf[i + 2 * TS_PACKET] != 0x47)
			continue;

		if (i) {
			memmove(s->buf, s->buf + i, s->len - i);
			s->len -= i;
		}
		s->synced = 1;
		return 0;
	}

	/* Keep the tail: a sync may straddle two reads. */
	if (s->len > 3 * TS_PACKET) {
		memmove(s->buf, s->buf + s->len - 3 * TS_PACKET, 3 * TS_PACKET);
		s->len = 3 * TS_PACKET;
	}
	return -1;
}

int sink_flush(struct sink *s)
{
	size_t unit = s->is_udp ? TS_DATAGRAM_PACKETS * TS_PACKET : TS_PACKET;
	size_t n;

	if (!s->synced)
		return 0;

	n = s->len - (s->len % unit);
	if (!n)
		return 0;

	if (s->is_udp) {
		struct sockaddr_in dst;

		memset(&dst, 0, sizeof(dst));
		dst.sin_family = AF_INET;
		dst.sin_addr.s_addr = s->dst_addr;
		dst.sin_port = htons(s->dst_port);

		if (sendto(s->sock, s->buf, n, 0, (struct sockaddr *)&dst,
			   sizeof(dst)) < 0)
			return -errno;
		s->datagrams += n / unit;
	} else {
		size_t off = 0;

		while (off < n) {
			ssize_t w = write(s->fd, s->buf + off, n - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				return -errno;
			}
			if (w == 0)
				return -EIO;
			off += (size_t)w;
		}
	}

	s->bytes += n;
	if (n < s->len)
		memmove(s->buf, s->buf + n, s->len - n);
	s->len -= n;
	return 0;
}

int sink_write(struct sink *s, const uint8_t *p, size_t n)
{
	while (n) {
		size_t room = SINK_CAP - s->len;
		size_t take = n < room ? n : room;

		memcpy(s->buf + s->len, p, take);
		s->len += take;
		p += take;
		n -= take;

		if (!s->synced)
			sink_sync(s);

		if (sink_flush(s) < 0)
			return -EIO;

		if (!take)
			return -ENOSPC;
	}
	return 0;
}

void sink_close(struct sink *s)
{
	if (s->sock >= 0)
		close(s->sock);
	if (s->fd >= 0 && s->fd != STDOUT_FILENO)
		close(s->fd);
	free(s->buf);
	s->buf = NULL;
}
