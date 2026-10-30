/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Transport stream sink: file / stdout / UDP output, with MPEG-TS packet
 * alignment.
 */

#include "sink.h"

#include "tsfilter.h"

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

/* Set by -v in the CLI; reports what the sink is actually pointed at. */
extern int icast_debug;

#define SINK_CAP (256 * 1024)

int sink_open(struct sink *s, const char *out, struct ts_filter *filter)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->sock = -1;
	s->filter = filter;

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
		s->exp_addr = s->dst_addr;
		s->exp_port = s->dst_port;
		s->is_udp = 1;

		/*
		 * Connect the socket so the kernel resolves the route once and
		 * validates the destination, then plain send() sends datagrams to
		 * it. Rebuilding a sockaddr for every sendto() is easy to get
		 * subtly wrong, and a wrong destination silently drops every
		 * packet without an error.
		 */
		{
			struct sockaddr_in dst;

			memset(&dst, 0, sizeof(dst));
			dst.sin_family = AF_INET;
			dst.sin_addr.s_addr = s->dst_addr;
			dst.sin_port = htons(s->dst_port);
			if (connect(s->sock, (struct sockaddr *)&dst,
				    sizeof(dst)) < 0)
				return -errno;
			if (icast_debug) {
				struct sockaddr_in peer, me;
				socklen_t pl = sizeof(peer), ml = sizeof(me);

				memset(&peer, 0, sizeof(peer));
				memset(&me, 0, sizeof(me));
				if (getpeername(s->sock, (struct sockaddr *)&peer, &pl) == 0)
					fprintf(stderr, "  [sink] peer %08x:%u\n",
						(unsigned)peer.sin_addr.s_addr,
						(unsigned)ntohs(peer.sin_port));
				else
					fprintf(stderr, "  [sink] getpeername failed: %s\n",
						strerror(errno));
				if (getsockname(s->sock, (struct sockaddr *)&me, &ml) == 0)
					fprintf(stderr, "  [sink] local %08x:%u\n",
						(unsigned)me.sin_addr.s_addr,
						(unsigned)ntohs(me.sin_port));
			}
		}
		if (icast_debug)
			fprintf(stderr, "  [sink] udp host=%s port=%u fd=%d\n",
				host, (unsigned)s->dst_port, s->sock);
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

	/*
	 * Filtering happens after sync so packets are aligned, and before the
	 * datagram split so a filtered datagram is still a whole number of
	 * packets. The tail is whatever did not make up a full packet yet.
	 */
	if (s->filter) {
		size_t whole = s->len - (s->len % TSF_PACKET);
		size_t kept = ts_filter_apply(s->filter, s->buf, whole);

		if (kept != whole) {
			size_t tail = s->len - whole;

			memmove(s->buf + kept, s->buf + whole, tail);
			s->len = kept + tail;
		}
	}

	n = s->len - (s->len % unit);
	if (!n)
		return 0;

	if (s->is_udp) {
		size_t off = 0;

		/*
		 * One datagram per 1316 byte group. Sending the whole buffer in a
		 * single sendto() fails with EMSGSIZE as soon as it exceeds the
		 * MTU, which it always does - a 128 KB read is 97 groups.
		 */
		if (s->dst_port != s->exp_port || s->dst_addr != s->exp_addr) {
			fprintf(stderr,
				"  [sink] TARGET CHANGED at #%llu: %08x:%u -> %08x:%u\n",
				s->datagrams, (unsigned)s->exp_addr,
				(unsigned)s->exp_port, (unsigned)s->dst_addr,
				(unsigned)s->dst_port);
			s->exp_addr = s->dst_addr;
			s->exp_port = s->dst_port;
		}
		while (off < n) {
			size_t chunk = n - off < unit ? n - off : unit;

			if (send(s->sock, s->buf + off, chunk, 0) < 0) {
				/*
				 * A live stream must survive having no listener: the
				 * kernel reports ICMP port unreachable as ECONNREFUSED
				 * on the next send. That is not a stream failure, it
				 * just means nobody is watching right now - a player
				 * that tunes in later gets data from then on.
				 */
				if (errno == ECONNREFUSED || errno == ENOBUFS ||
				    errno == EAGAIN || errno == EINTR) {
					s->dropped++;
					off += chunk;
					continue;
				}
				return -errno;
			}
			off += chunk;
			s->datagrams++;
		}
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

		{
			int fr = sink_flush(s);

			if (fr < 0)
				return fr;
		}

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
