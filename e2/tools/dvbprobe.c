// SPDX-License-Identifier: GPL-2.0
/*
 * dvbprobe - minimal DVB-C tuning + TS capture probe.
 *
 * Built static so it can be dropped onto an Enigma2 image that has no
 * toolchain and no dvb-apps.  Uses the DTV property API rather than the
 * legacy FE_SET_FRONTEND ioctl on purpose: the legacy call derives the
 * delivery system from fe_info.type, and our dongle currently reports
 * FE_OFDM, which would be interpreted as DVB-T.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

static const char *fe_type_name(fe_type_t t)
{
	switch (t) {
	case FE_QPSK: return "FE_QPSK";
	case FE_QAM:  return "FE_QAM";
	case FE_OFDM: return "FE_OFDM";
	case FE_ATSC: return "FE_ATSC";
	default:      return "unknown";
	}
}

static long ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000L +
	       (t.tv_nsec - t0->tv_nsec) / 1000000L;
}

int main(int argc, char **argv)
{
	const char *adir = argc > 1 ? argv[1] : "/dev/dvb/adapter1";
	unsigned int freq = argc > 2 ? (unsigned)atoi(argv[2]) : 251000000;
	unsigned int sr   = argc > 3 ? (unsigned)atoi(argv[3]) : 6875000;
	int capture       = argc > 4 ? atoi(argv[4]) : 4;   /* seconds */
	const char *outfile = argc > 5 ? argv[5] : NULL;
	char path[128];
	struct dvb_frontend_info info;
	struct dtv_property props[8];
	struct dtv_properties cmdseq;
	struct timespec t0;
	int fd, dfd, vfd, i, ret;
	enum fe_status status = 0;
	unsigned char *buf;
	size_t got = 0, want, off;
	long elapsed;

	snprintf(path, sizeof(path), "%s/frontend0", adir);
	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0) { perror(path); return 1; }

	if (ioctl(fd, FE_GET_INFO, &info) < 0) { perror("FE_GET_INFO"); return 1; }
	printf("frontend   : %s\n", info.name);
	printf("type       : %d (%s)\n", info.type, fe_type_name(info.type));
	printf("caps       : 0x%08x\n", info.caps);
	printf("freq range : %u .. %u step %u\n",
	       info.frequency_min, info.frequency_max, info.frequency_stepsize);
	printf("symbolrate : %u .. %u\n", info.symbol_rate_min, info.symbol_rate_max);

	i = 0;
	props[i].cmd = DTV_CLEAR;              i++;
	props[i].cmd = DTV_DELIVERY_SYSTEM;    props[i].u.data = SYS_DVBC_ANNEX_A; i++;
	props[i].cmd = DTV_FREQUENCY;          props[i].u.data = freq;             i++;
	props[i].cmd = DTV_MODULATION;         props[i].u.data = QAM_64;           i++;
	props[i].cmd = DTV_SYMBOL_RATE;        props[i].u.data = sr;               i++;
	props[i].cmd = DTV_INVERSION;          props[i].u.data = INVERSION_AUTO;   i++;
	props[i].cmd = DTV_TUNE;               props[i].u.data = 0;                i++;
	cmdseq.num = i;
	cmdseq.props = props;

	printf("\ntuning     : %u Hz, QAM_64, %u S/s\n", freq, sr);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ioctl(fd, FE_SET_PROPERTY, &cmdseq) < 0) {
		printf("FE_SET_PROPERTY failed: %s\n", strerror(errno));
		return 1;
	}
	printf("tune ioctl : returned in %ld ms\n", ms_since(&t0));

	for (i = 0; i < 20; i++) {
		usleep(500000);
		status = 0;
		if (ioctl(fd, FE_READ_STATUS, &status) < 0) {
			printf("FE_READ_STATUS failed: %s\n", strerror(errno));
			break;
		}
		printf("  +%4ld ms  status=0x%02x%s%s%s%s%s\n", ms_since(&t0), status,
		       (status & FE_HAS_SIGNAL)  ? " SIGNAL"  : "",
		       (status & FE_HAS_CARRIER) ? " CARRIER" : "",
		       (status & FE_HAS_VITERBI) ? " VITERBI" : "",
		       (status & FE_HAS_SYNC)    ? " SYNC"    : "",
		       (status & FE_HAS_LOCK)    ? " LOCK"    : "");
		if (status & FE_HAS_LOCK)
			break;
	}

	{
		unsigned int strength = 0, snr = 0, ber = 0, ucb = 0;
		if (ioctl(fd, FE_READ_SIGNAL_STRENGTH, &strength) == 0)
			printf("strength   : %u\n", strength);
		if (ioctl(fd, FE_READ_SNR, &snr) == 0)
			printf("snr        : %u\n", snr);
		if (ioctl(fd, FE_READ_BER, &ber) == 0)
			printf("ber        : %u\n", ber);
		if (ioctl(fd, FE_READ_UNCORRECTED_BLOCKS, &ucb) == 0)
			printf("ucblocks   : %u\n", ucb);
	}

	if (capture <= 0)
		return 0;

	/* Full transport stream: PID 0x2000 with DMX_OUT_TS_TAP. */
	snprintf(path, sizeof(path), "%s/demux0", adir);
	dfd = open(path, O_RDWR);
	if (dfd < 0) { perror(path); return 1; }
	{
		struct dmx_pes_filter_params p;
		memset(&p, 0, sizeof(p));
		p.pid = 0x2000;
		p.input = DMX_IN_FRONTEND;
		p.output = DMX_OUT_TS_TAP;
		p.pes_type = DMX_PES_OTHER;
		p.flags = 0;
		if (ioctl(dfd, DMX_SET_PES_FILTER, &p) < 0) {
			perror("DMX_SET_PES_FILTER");
			return 1;
		}
		if (ioctl(dfd, DMX_START, 0) < 0) { perror("DMX_START"); return 1; }
	}
	printf("demux      : full TS filter armed\n");

	snprintf(path, sizeof(path), "%s/dvr0", adir);
	vfd = open(path, O_RDONLY | O_NONBLOCK);
	if (vfd < 0) { perror(path); return 1; }

	want = (size_t)capture * 1024 * 1024;
	buf = malloc(want);
	if (!buf) return 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (got < want) {
		fd_set rfds;
		struct timeval tv = { 0, 500000 };
		ssize_t n;

		FD_ZERO(&rfds);
		FD_SET(vfd, &rfds);
		ret = select(vfd + 1, &rfds, NULL, NULL, &tv);
		if (ret == 0) {
			if (ms_since(&t0) > (capture + 3) * 1000L) break;
			continue;
		}
		if (ret < 0) { if (errno == EINTR) continue; perror("select"); break; }
		n = read(vfd, buf + got, want - got);
		if (n > 0) got += n;
		else if (n < 0 && errno != EAGAIN) break;
	}

	elapsed = ms_since(&t0);
	printf("captured   : %zu bytes in %ld ms (%.2f Mbit/s)\n", got, elapsed,
	       elapsed > 0 ? (double)got * 8.0 / (double)elapsed / 1000.0 : 0.0);

	if (outfile && got) {
		FILE *fp = fopen(outfile, "wb");
		if (fp) {
			size_t wr = fwrite(buf, 1, got, fp);
			fclose(fp);
			printf("written    : %s (%zu bytes)\n", outfile, wr);
		} else {
			printf("written    : %s failed: %s\n", outfile, strerror(errno));
		}
	}

	if (got >= 188) {
		unsigned long pkts = 0, bad_sync = 0, tei = 0, scrambled = 0;
		unsigned long *pids = calloc(8192, sizeof(*pids));
		size_t pid_top_i;
		int top, k;

		for (off = 0; off + 188 <= got; off += 188) {
			unsigned char *p = buf + off;
			unsigned int pid, sc;

			if (p[0] != 0x47) { bad_sync++; continue; }
			pkts++;
			if (p[1] & 0x80) tei++;
			pid = ((p[1] & 0x1f) << 8) | p[2];
			sc = (p[3] >> 6) & 0x03;
			if (sc && pids) scrambled++;
			if (pids && pid < 8192) pids[pid]++;
		}
		printf("packets    : %lu  bad_sync=%lu  tei=%lu  scrambled=%lu (%.2f%%)\n",
		       pkts, bad_sync, tei, scrambled,
		       pkts ? (double)scrambled * 100.0 / (double)pkts : 0.0);

		if (pids) {
			printf("top PIDs   :");
			for (k = 0; k < 8; k++) {
				unsigned long best = 0;
				pid_top_i = 0;
				for (top = 0; top < 8192; top++)
					if (pids[top] > best) { best = pids[top]; pid_top_i = top; }
				if (!best) break;
				printf(" %#06zx=%lu", pid_top_i, best);
				pids[pid_top_i] = 0;
			}
			printf("\n");
			free(pids);
		}
	}

	close(vfd);
	ioctl(dfd, DMX_STOP, 0);
	close(dfd);
	close(fd);
	free(buf);
	return 0;
}
