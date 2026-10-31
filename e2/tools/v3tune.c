/*
 * v3tune - exercise the *legacy* DVBv3 tuning path (FE_SET_FRONTEND) exactly
 * the way Enigma2 does, so we can tell whether the driver handles it.
 *
 *   v3tune [-d] [-f freq_khz] [-s symrate] [-q qam]
 *
 *   -d   arm a PAT section filter on demux0 *before* tuning (E2 does this)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

static const char *fe_type_name(fe_type_t t)
{
	switch (t) {
	case FE_QPSK: return "QPSK";
	case FE_QAM:  return "QAM";
	case FE_OFDM: return "OFDM";
	case FE_ATSC: return "ATSC";
	default:      return "?";
	}
}

static void show_status(const char *tag, enum fe_status s)
{
	printf("%-10s status=0x%02x%s%s%s%s%s\n", tag, s,
	       (s & FE_HAS_SIGNAL)      ? " SIGNAL"  : "",
	       (s & FE_HAS_CARRIER)     ? " CARRIER" : "",
	       (s & FE_HAS_VITERBI)     ? " VITERBI" : "",
	       (s & FE_HAS_SYNC)        ? " SYNC"    : "",
	       (s & FE_HAS_LOCK)        ? " LOCK"    : "");
}

int main(int argc, char **argv)
{
	int opt, arm_demux = 0, dfd = -1;
	unsigned int freq = 251000;      /* kHz, same as E2's Transponder */
	unsigned int sr = 6875000;
	fe_modulation_t qam = QAM_64;
	struct dvb_frontend_info info;
	struct dvb_frontend_parameters p;
	enum fe_status st;
	int fd, i, ret;

	while ((opt = getopt(argc, argv, "df:s:q:")) != -1) {
		switch (opt) {
		case 'd': arm_demux = 1; break;
		case 'f': freq = strtoul(optarg, NULL, 0); break;
		case 's': sr = strtoul(optarg, NULL, 0); break;
		case 'q': qam = atoi(optarg); break;
		default: fprintf(stderr, "usage: %s [-d] [-f kHz] [-s S/s] [-q qam]\n", argv[0]);
			 return 2;
		}
	}

	fd = open("/dev/dvb/adapter1/frontend0", O_RDWR | O_NONBLOCK);
	if (fd < 0) { perror("open frontend"); return 1; }

	memset(&info, 0, sizeof(info));
	if (ioctl(fd, FE_GET_INFO, &info) < 0) { perror("FE_GET_INFO"); return 1; }
	printf("frontend   : %s\n", info.name);
	printf("type       : %d (%s)\n", info.type, fe_type_name(info.type));
	printf("caps       : 0x%08x\n", info.caps);

	if (arm_demux) {
		struct dmx_pes_filter_params f;
		dfd = open("/dev/dvb/adapter1/demux0", O_RDWR | O_NONBLOCK);
		if (dfd < 0) { perror("open demux"); return 1; }
		memset(&f, 0, sizeof(f));
		f.pid = 0x0000;             /* PAT, same as E2's scan */
		f.input = DMX_IN_FRONTEND;
		f.output = DMX_OUT_TS_TAP;
		f.pes_type = DMX_PES_OTHER;
		f.flags = DMX_IMMEDIATE_START;
		if (ioctl(dfd, DMX_SET_PES_FILTER, &f) < 0) { perror("DMX_SET_PES_FILTER"); return 1; }
		printf("demux      : PAT filter armed BEFORE tuning\n");
	}

	memset(&p, 0, sizeof(p));
	p.frequency = freq;             /* legacy API: kHz */
	p.inversion = INVERSION_AUTO;
	p.u.qam.symbol_rate = sr;
	p.u.qam.fec_inner = FEC_NONE;
	p.u.qam.modulation = qam;

	printf("\ntuning (v3) : frequency=%u kHz, symbol_rate=%u, qam=%d\n", freq, sr, qam);
	fflush(stdout);

	errno = 0;
	ret = ioctl(fd, FE_SET_FRONTEND, &p);
	printf("FE_SET_FRONTEND : ret=%d errno=%d (%s)\n", ret, errno, strerror(errno));

	for (i = 0; i < 12; i++) {
		st = 0;
		errno = 0;
		if (ioctl(fd, FE_READ_STATUS, &st) < 0) {
			printf("  +%4dms  FE_READ_STATUS failed errno=%d (%s)\n",
			       i * 500, errno, strerror(errno));
		} else {
			char tag[16];
			snprintf(tag, sizeof(tag), "+%dms", i * 500);
			show_status(tag, st);
		}
		fflush(stdout);
		if (st & FE_HAS_LOCK)
			break;
		usleep(500000);
	}

	memset(&p, 0, sizeof(p));
	if (ioctl(fd, FE_GET_FRONTEND, &p) == 0)
		printf("\nreadback   : frequency=%u kHz, symbol_rate=%u, qam=%d, inversion=%d\n",
		       p.frequency, p.u.qam.symbol_rate, p.u.qam.modulation, p.inversion);
	else
		perror("FE_GET_FRONTEND");

	close(fd);
	if (dfd >= 0)
		close(dfd);
	return 0;
}
