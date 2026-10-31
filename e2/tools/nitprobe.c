/*
 * nitprobe -- listen on a DVB demux for PSI/SI tables and dump them raw.
 *
 * Used to find out whether a cable headend actually broadcasts a NIT
 * (PID 0x0010) or a BAT (PID 0x0011).  Enigma2 reports "got nit, err -1"
 * when its table filter times out, which is indistinguishable from "the
 * headend never sends one" without looking at the transport stream itself.
 *
 * Build (on the ARM build host):
 *   arm-buildroot-linux-gnueabihf-gcc -O2 -static -o nitprobe nitprobe.c
 *
 * Usage:
 *   nitprobe [adapter_dir] [pid] [seconds]
 *   nitprobe                      -> /dev/dvb/adapter0, pid 0x10, 15 s
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/dvb/dmx.h>

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static const char *table_name(unsigned char tid)
{
	switch (tid) {
	case 0x00: return "PAT";
	case 0x40: return "NIT(actual)";
	case 0x41: return "NIT(other)";
	case 0x42: return "SDT(actual)";
	case 0x46: return "SDT(other)";
	case 0x4A: return "BAT";
	case 0x4E: return "EIT";
	case 0x70: return "TDT";
	case 0x73: return "TOT";
	default:   return "?";
	}
}

/* Walk the NIT/BAT descriptor loop and print every frequency we find. */
static void dump_nit_transponders(const unsigned char *s, int len)
{
	/* s points at table_id, len counts bytes present */
	int pos;
	int network_descriptors_length, transport_stream_loop_length;

	if (len < 12)
		return;

	/* 3 header + 2 netid + 1 ver + 2 section_length */
	network_descriptors_length = ((s[8] & 0x0f) << 8) | s[9];
	pos = 10 + network_descriptors_length;
	if (pos + 2 > len)
		return;

	transport_stream_loop_length = ((s[pos] & 0x0f) << 8) | s[pos + 1];
	pos += 2;

	while (pos + 6 <= len && transport_stream_loop_length > 6) {
		int tsid = (s[pos] << 8) | s[pos + 1];
		int onid = (s[pos + 2] << 8) | s[pos + 3];
		int dlen = ((s[pos + 4] & 0x0f) << 8) | s[pos + 5];
		int dpos = pos + 6;
		int dend = dpos + dlen;

		printf("    TS tsid=%04x onid=%04x\n", tsid, onid);

		while (dpos + 2 <= dend) {
			int tag = s[dpos];
			int l = s[dpos + 1];
			const unsigned char *d = s + dpos + 2;

			if (tag == 0x44 && l >= 11) {
				/* cable_delivery_system_descriptor */
				unsigned long freq = ((unsigned long)d[0] << 24) |
						     ((unsigned long)d[1] << 16) |
						     ((unsigned long)d[2] << 8) | d[3];
				int qam = d[6] & 0x3f;
				unsigned long sr = ((unsigned long)d[7] << 16) |
						   ((unsigned long)d[8] << 8) | d[9];
				printf("      cable freq=%lu Hz (%lu kHz) qam=%d sr=%lu\n",
				       freq * 100, freq / 10, qam, sr * 100);
			} else if (tag == 0x5f && l >= 5) {
				/* terrestrial_delivery_system_descriptor */
				unsigned long centre = ((unsigned long)d[0] << 24) |
						       ((unsigned long)d[1] << 16) |
						       ((unsigned long)d[2] << 8) | d[3];
				printf("      dvbt centre=%lu Hz (%lu kHz)\n",
				       centre * 10, centre / 100);
			} else {
				printf("      descriptor tag=0x%02x len=%d\n", tag, l);
			}
			if (l + 2 <= 0)
				break;
			dpos += l + 2;
		}

		transport_stream_loop_length -= dlen + 6;
		pos = dend;
	}
}

int main(int argc, char **argv)
{
	const char *adir = argc > 1 ? argv[1] : "/dev/dvb/adapter0";
	int pid = argc > 2 ? (int)strtol(argv[2], NULL, 0) : 0x10;
	int secs = argc > 3 ? atoi(argv[3]) : 15;

	char path[128];
	struct dmx_sct_filter_params f;
	unsigned char buf[8192];
	int fd, dmx, n, total = 0;
	long long deadline;

	snprintf(path, sizeof(path), "%s/demux1", adir);
	if (strstr(adir, "demux")) {
		/* caller passed a full demux device path */
		snprintf(path, sizeof(path), "%s", adir);
	} else if (access(path, F_OK) != 0) {
		snprintf(path, sizeof(path), "%s/demux0", adir);
	}

	dmx = open(path, O_RDWR);
	if (dmx < 0) {
		printf("open %s: %s\n", path, strerror(errno));
		return 1;
	}

	memset(&f, 0, sizeof(f));
	f.pid = pid;
	f.filter.filter[0] = 0x00;
	f.filter.mask[0] = 0x00;
	f.timeout = 0;
	f.flags = DMX_IMMEDIATE_START;

	if (ioctl(dmx, DMX_SET_FILTER, &f) < 0) {
		printf("DMX_SET_FILTER pid=0x%x: %s\n", pid, strerror(errno));
		close(dmx);
		return 1;
	}

	/*
	 * A section filter delivers its data straight back through the demux
	 * file handle; only PES filters with DMX_OUT_TS_TAP feed the dvr node.
	 */
	fd = dmx;

	printf("listening on %s pid=0x%04x for %d s\n", path, pid, secs);
	fflush(stdout);

	deadline = now_ms() + secs * 1000LL;
	while (now_ms() < deadline) {
		fd_set rf;
		struct timeval tv = { 1, 0 };

		FD_ZERO(&rf);
		FD_SET(fd, &rf);
		if (select(fd + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;

		n = read(fd, buf, sizeof(buf));
		if (n <= 0)
			continue;

		total += n;
		printf("  [%d bytes] table 0x%02x section_length=%d %s\n",
		       n, buf[0],
		       ((buf[1] & 0x0f) << 8) | buf[2],
		       table_name(buf[0]));
		if (buf[0] == 0x40 || buf[0] == 0x41 || buf[0] == 0x4a)
			dump_nit_transponders(buf, n);
		if (buf[0] == 0x00) {
			/* PAT: emit the hex once, plus the network PID (program 0) */
			int p, slen = ((buf[1] & 0x0f) << 8) | buf[2];

			printf("      hex:");
			for (p = 0; p < n && p < 48; p++)
				printf(" %02x", buf[p]);
			printf("\n");
			for (p = 8; p + 4 <= 3 + slen && p + 4 <= n; p += 4) {
				int prog = (buf[p] << 8) | buf[p + 1];
				int ppid = ((buf[p + 2] & 0x1f) << 8) | buf[p + 3];

				if (prog == 0)
					printf("      PAT program 0 -> NIT PID 0x%04x\n", ppid);
			}
			fflush(stdout);
		}
		fflush(stdout);
	}

	printf("\n--- %d bytes received for pid 0x%04x in %d s ---\n",
	       total, pid, secs);
	if (!total)
		printf("=> nothing arrived: the headend does not broadcast this table\n");

	close(dmx);
	return 0;
}
