/*
 * retunetest - pin down which retune paths lose the frequency change.
 *
 * Enigma2's cable scan records services from the WRONG transponder: while
 * tuned to 251 MHz the PAT it reads still says tsid 23 (the 259 MHz mux).
 * The blind-scan helper retunes in one session without problems, so the
 * difference must be in how the frontend and the demux are opened and
 * closed around the retune.  This tool replays each pattern separately:
 *
 *   phase 1  cold open fe, tune f1,        section filter on, read PAT tsid
 *   phase 2  same fe fd,   retune f2,      section filter on, read PAT tsid
 *   phase 3  close+reopen fe, tune f1,     section filter on, read PAT tsid
 *   phase 4  same fe fd,   retune f2,      PES filter instead, read PAT tsid
 *
 * A phase that reports the tsid of the frequency it tuned is a working
 * retune path; one that reports the previous tsid means the firmware
 * ignored the tune.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#define FE  "/dev/dvb/adapter1/frontend0"
#define DMX "/dev/dvb/adapter1/demux0"

static long long now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int set_prop(int fd, unsigned cmd, unsigned val)
{
	struct dtv_property p;
	struct dtv_properties ps;

	memset(&p, 0, sizeof(p));
	p.cmd = cmd;
	p.u.data = val;
	memset(&ps, 0, sizeof(ps));
	ps.num = 1;
	ps.props = &p;
	return ioctl(fd, FE_SET_PROPERTY, &ps);
}

static int tune(int fd, unsigned freq_khz, unsigned sr)
{
	set_prop(fd, DTV_DELIVERY_SYSTEM, SYS_DVBC_ANNEX_A);
	set_prop(fd, DTV_FREQUENCY, freq_khz * 1000);
	set_prop(fd, DTV_SYMBOL_RATE, sr);
	set_prop(fd, DTV_MODULATION, QAM_64);
	set_prop(fd, DTV_INVERSION, INVERSION_AUTO);
	return set_prop(fd, DTV_TUNE, 0);
}

static int wait_lock(int fd, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	fe_status_t st;

	while (now_ms() < deadline) {
		st = 0;
		if (ioctl(fd, FE_READ_STATUS, &st) == 0 &&
		    (st & FE_HAS_LOCK))
			return 1;
		usleep(50 * 1000);
	}
	return 0;
}

static int open_section_filter(int *dmx)
{
	struct dmx_sct_filter_params f;

	*dmx = open(DMX, O_RDWR | O_NONBLOCK);
	if (*dmx < 0) {
		printf("    open %s: %s\n", DMX, strerror(errno));
		return -1;
	}
	memset(&f, 0, sizeof(f));
	f.pid = 0x0000;			/* PAT */
	f.timeout = 0;
	f.flags = 0;
	if (ioctl(*dmx, DMX_SET_FILTER, &f) < 0) {
		printf("    DMX_SET_FILTER: %s\n", strerror(errno));
		close(*dmx);
		*dmx = -1;
		return -1;
	}
	if (ioctl(*dmx, DMX_START, 0) < 0) {
		printf("    DMX_START: %s\n", strerror(errno));
		close(*dmx);
		*dmx = -1;
		return -1;
	}
	return 0;
}

static int open_pes_filter(int *dmx)
{
	struct dmx_pes_filter_params p;

	*dmx = open(DMX, O_RDWR | O_NONBLOCK);
	if (*dmx < 0) {
		printf("    open %s: %s\n", DMX, strerror(errno));
		return -1;
	}
	memset(&p, 0, sizeof(p));
	p.pid = 0x2000;
	p.input = DMX_IN_FRONTEND;
	p.output = DMX_OUT_TS_TAP;
	p.pes_type = DMX_PES_OTHER;
	p.flags = DMX_IMMEDIATE_START;
	if (ioctl(*dmx, DMX_SET_PES_FILTER, &p) < 0) {
		printf("    DMX_SET_PES_FILTER: %s\n", strerror(errno));
		close(*dmx);
		*dmx = -1;
		return -1;
	}
	return 0;
}

/*
 * Drain the dvr side is not needed for section filters (sections arrive on
 * the demux fd itself), but the first sections after a retune may still
 * belong to the previous mux, so read for a while and report every distinct
 * tsid seen, oldest first.  A stale first burst followed by fresh PATs
 * shows up as two different tsids.
 */
static int read_tsid(int dmx, int wait_ms, int ts_mode)
{
	long long deadline = now_ms() + wait_ms;
	unsigned char buf[4096];
	int last = -1;

	while (now_ms() < deadline) {
		fd_set r;
		struct timeval tv = { 0, 100 * 1000 };
		int n;

		FD_ZERO(&r);
		FD_SET(dmx, &r);
		if (select(dmx + 1, &r, NULL, NULL, &tv) <= 0)
			continue;
		n = read(dmx, buf, sizeof(buf));
		if (n < 0)
			continue;

		if (!ts_mode) {
			/* demux fd hands out bare sections */
			if (n >= 5 && buf[0] == 0x00) {
				int tsid = (buf[3] << 8) | buf[4];
				if (tsid != last) {
					printf("    PAT tsid=%d (0x%x)\n",
					       tsid, tsid);
					last = tsid;
				}
			}
			continue;
		}

		/* dvr fd hands out TS packets */
		for (int i = 0; i + 5 < n; i++) {
			unsigned char *s;
			int p;

			if (buf[i] != 0x47)
				continue;
			/* PID 0, payload present, section starts here */
			if ((buf[i + 1] & 0x1f) != 0 || buf[i + 2] != 0x00)
				continue;
			if ((buf[i + 3] & 0x10) == 0)
				continue;
			if ((buf[i + 1] & 0x40) == 0)
				continue;
			p = buf[i + 4];		/* pointer_field */
			if (i + 5 + p + 5 > n)
				continue;
			s = buf + i + 5 + p;
			if (s[0] != 0x00)
				continue;
			int tsid = (s[3] << 8) | s[4];
			if (tsid != last) {
				printf("    PAT tsid=%d (0x%x)\n", tsid, tsid);
				last = tsid;
			}
		}
	}
	if (last < 0)
		printf("    no PAT received\n");
	return last;
}

static void phase(const char *name, int reopen_fe, unsigned freq, int pes,
		  unsigned sr)
{
	static int fe = -1;
	int dmx = -1, tsid;
	unsigned char drain_buf[8192];

	printf("%s: tune %u kHz (%s filter%s)\n", name, freq,
	       pes ? "PES" : "section", reopen_fe ? ", fe reopened" : "");

	if (reopen_fe && fe >= 0) {
		close(fe);
		fe = -1;
	}
	if (fe < 0) {
		fe = open(FE, O_RDWR | O_NONBLOCK);
		if (fe < 0) {
			printf("    open %s: %s\n", FE, strerror(errno));
			return;
		}
	}

	if (tune(fe, freq, sr) < 0) {
		printf("    tune: %s\n", strerror(errno));
		return;
	}
	if (!wait_lock(fe, 2000)) {
		printf("    no lock in 2 s\n");
		return;
	}

	if (pes) {
		if (open_pes_filter(&dmx) < 0)
			return;
		/* TS_TAP output arrives on the dvr device, not the demux fd */
		dmx = open("/dev/dvb/adapter1/dvr0", O_RDONLY | O_NONBLOCK);
		if (dmx < 0) {
			printf("    open dvr0: %s\n", strerror(errno));
			return;
		}
	} else {
		if (open_section_filter(&dmx) < 0)
			return;
	}
	if (dmx < 0)
		return;

	usleep(400 * 1000);
	/* drain whatever the FIFO kept from before */
	while (read(dmx, drain_buf, sizeof(drain_buf)) > 0)
		;
	tsid = read_tsid(dmx, 1500, pes);
	printf("    -> tsid %d (expected %u)\n", tsid,
	       freq == 251000 ? 22 : 23);

	if (dmx >= 0)
		close(dmx);
}

int main(int argc, char **argv)
{
	unsigned f1 = 251000, f2 = 259000, sr = 6875000;

	if (argc > 1)
		f1 = atoi(argv[1]);
	if (argc > 2)
		f2 = atoi(argv[2]);
	if (argc > 3)
		sr = atoi(argv[3]);

	printf("=== retune-path test %u <-> %u kHz, sr %u ===\n", f1, f2, sr);
	phase("phase1 (cold fe -> f1, section)", 0, f1, 0, sr);
	phase("phase2 (same fe -> f2, section)", 0, f2, 0, sr);
	phase("phase3 (reopen fe -> f1, section)", 1, f1, 0, sr);
	phase("phase4 (same fe -> f2, PES)", 0, f2, 1, sr);
	return 0;
}
