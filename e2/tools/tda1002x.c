/*
 * tda1002x -- cable transponder search utility, iCast edition.
 *
 * Enigma2's "Automatic Scan" on a cable tuner does not walk a transponder
 * list itself.  ScanSetup.py shells out to a vendor binary (tda1002x,
 * CXD1981, atbm781x, ...) that hunts for live transponders and prints them
 * on stdout; E2 then scans only the frequencies that came back.
 *
 * The vendors ship that binary for their own demodulator.  For the iCast
 * USB stick there is none, so E2 aborts with
 *   "Cable scan executable utility not found '/usr/bin/tda1002x'!"
 *
 * This program implements the same command line and the same stdout
 * protocol on top of the plain DVB frontend API, so the stock UI works:
 *
 *   tda1002x --init --scan --verbose --wakeup --inv 2 --bus 2 \
 *            --scan-stepsize 1000 --mod 64 --sr 6875000
 *
 * Output per hit (parsed by Screens/ScanSetup.py::getCableTransponderData):
 *
 *   OK <frequency_khz> <symbol_rate_hz> FEC_AUTO QAM64 INVERSION_AUTO
 *
 *
 * Two device-level facts shape the design, both measured on OpenBH 5.6 /
 * kernel 4.4.35 with the iCast firmware:
 *
 *  1. The demodulator has no working AFC.  A multiplex centred on 251.000
 *     MHz locks at 251 and nowhere else: 249, 250, 252, 253 all fail even
 *     after five seconds of settling.  Detection therefore has to walk a
 *     fine raster, and a frequency is only reported when the demodulator
 *     itself says FE_HAS_LOCK.
 *
 *  2. Opening and closing the DVB devices per frequency breaks the stick.
 *     Every close makes dvb-usb kill its bulk URBs without telling the
 *     firmware to stop pushing, the device-side FIFO fills, and the bulk IN
 *     endpoint halts.  dvb-usb resubmits the URB on the next open but never
 *     calls usb_clear_halt, so the second session reads a dead pipe -- or
 *     worse, the tail of the previous multiplex, which is what made an
 *     earlier version "find" a signal on almost every frequency it tried.
 *
 * So a sweep opens frontend + demux + dvr exactly once and keeps them open,
 * retuning with plain FE_SET_FRONTEND for each step -- the same sequence
 * Enigma2 itself uses when the user zaps from one channel to the next.
 *
 * Build:
 *   arm-buildroot-linux-gnueabihf-gcc -O2 -static -o tda1002x tda1002x.c
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#define MAX_SR 8
#define MAX_MOD 4

/*
 * Default cable raster.  Chinese cable headends here put their multiplexes
 * on 115 MHz + n*8 MHz: 251.000 and 259.000 both land on it, and the two
 * working symbols are eight apart.  Enigma2 passes --scan-stepsize 1000 by
 * default (config.Nims[x].dvbc.scan_frequency_steps), which over 115..862
 * MHz is 748 probes and roughly six minutes of USB traffic; 8 MHz probes
 * the same spectrum in under a minute.  The value from E2 is still honoured
 * whenever it is coarser, and --step-exact forces it verbatim.
 */
#define DEFAULT_FIRST_KHZ  115000
#define DEFAULT_LAST_KHZ   862000
#define DEFAULT_STEP_KHZ    8000

static long long now_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

struct opts {
	unsigned step_khz;
	unsigned first_khz;
	unsigned last_khz;
	unsigned srs[MAX_SR];
	int nsr;
	int mods[MAX_MOD];
	int nmod;
	int wait_ms;		/* how long to watch the dvr for a PAT */
	int lock_ms;		/* how long to wait for FE_HAS_LOCK */
	int settle_ms;		/* pause after lock before trusting the dvr */
	int gap_ms;		/* pause between steps */
	int open_ms;		/* how long to wait for a busy frontend */
	const char *fe;
	const char *dmx;
	const char *dvr;
	const char *logpath;
	int verbose;
	int single;
	int step_exact;
	int tap_probe;		/* seconds to watch the dvr without touching the frontend */
	int sec_probe;		/* seconds to watch a PID-0 section filter on the demux */
};

/* ------------------------------------------------------------------ */

/*
 * Enigma2 keeps adapter1/frontend0 open for as long as it is running, so a
 * helper process cannot take the frontend while E2 is still playing.  Its
 * scan setup calls stopService() first, which tears the channel down and
 * closes the frontend -- but not necessarily by the time the helper starts.
 * Waiting for EBUSY to clear covers that gap; any other errno is reported
 * immediately because retrying will not fix it.
 *
 * There is no silent fallback to adapter0: that is the receiver's built-in
 * cable frontend, and a scan that quietly ran on it describes completely
 * different hardware while looking perfectly normal in the hit list.
 */
static int open_fe_wait(const struct opts *o, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	int warned = 0;

	for (;;) {
		int fd = open(o->fe, O_RDWR | O_NONBLOCK);

		if (fd >= 0)
			return fd;
		if (errno != EBUSY || now_ms() >= deadline) {
			fprintf(stderr, "tda1002x(icast): cannot open %s: %s\n",
				o->fe, strerror(errno));
			return -1;
		}
		if (!warned) {
			fprintf(stderr,
				"tda1002x(icast): %s is busy, waiting up to %d ms for Enigma2 to release it\n",
				o->fe, timeout_ms);
			warned = 1;
		}
		usleep(300 * 1000);
	}
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

/*
 * Every step reports its own errno.  "tune error" on its own was useless
 * during bring-up: -ENODEV means the frontend was put to sleep by Enigma2,
 * -EBUSY means someone else is driving it, -EINVAL means the property set
 * is not supported -- three completely different problems that used to
 * look identical from the outside.
 */
static int tune(int fd, unsigned freq_khz, unsigned sr, enum fe_modulation mod)
{
	static const char *const names[] = {
		"DTV_DELIVERY_SYSTEM", "DTV_FREQUENCY", "DTV_SYMBOL_RATE",
		"DTV_MODULATION", "DTV_INVERSION", "DTV_TUNE"
	};
	unsigned cmd[6];
	unsigned val[6];
	int i;

	cmd[0] = DTV_DELIVERY_SYSTEM;	val[0] = SYS_DVBC_ANNEX_A;
	cmd[1] = DTV_FREQUENCY;		val[1] = freq_khz * 1000;
	cmd[2] = DTV_SYMBOL_RATE;	val[2] = sr;
	cmd[3] = DTV_MODULATION;	val[3] = mod;
	cmd[4] = DTV_INVERSION;		val[4] = INVERSION_AUTO;
	cmd[5] = DTV_TUNE;		val[5] = 0;

	for (i = 0; i < 6; i++) {
		if (set_prop(fd, cmd[i], val[i]) < 0) {
			fprintf(stderr,
				"tda1002x(icast): %u kHz: %s failed: %s\n",
				freq_khz, names[i], strerror(errno));
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/*
 * One device session: frontend, demux and dvr opened once and kept open
 * for the whole sweep.  See the file header for why this matters.
 */
struct session {
	int fe;
	int dmx;
	int dvr;
};

static int sess_open(const struct opts *o, struct session *s)
{
	struct dmx_pes_filter_params p;

	s->fe = s->dmx = s->dvr = -1;

	/*
	 * Report what was actually opened.  adapter0 on this receiver is the
	 * built-in cable frontend, adapter1 is the stick, and a scan that
	 * silently ran on the wrong one is indistinguishable from a working
	 * one when only the hit list is printed.
	 */
	if (o->verbose)
		fprintf(stderr, "tda1002x(icast): session on %s\n", o->fe);

	s->fe = open_fe_wait(o, o->open_ms);
	if (s->fe < 0)
		return -1;

	s->dmx = open(o->dmx, O_RDWR | O_NONBLOCK);
	if (s->dmx < 0) {
		fprintf(stderr, "tda1002x(icast): cannot open %s: %s\n",
			o->dmx, strerror(errno));
		goto fail;
	}

	/*
	 * Section filters do not deliver anything on this adapter; a PES
	 * filter on a fixed PID with DMX_OUT_TS_TAP does, and the resulting
	 * transport stream is read from the dvr.  PID 0x2000 is deliberately
	 * not a broadcast PID, so the demux copies the whole mux through
	 * instead of selecting one elementary stream.
	 */
	memset(&p, 0, sizeof(p));
	p.pid = 0x2000;
	p.input = DMX_IN_FRONTEND;
	p.output = DMX_OUT_TS_TAP;
	p.pes_type = DMX_PES_OTHER;
	if (ioctl(s->dmx, DMX_SET_PES_FILTER, &p) < 0) {
		fprintf(stderr, "tda1002x(icast): DMX_SET_PES_FILTER on %s failed: %s\n",
			o->dmx, strerror(errno));
		goto fail;
	}

	s->dvr = open(o->dvr, O_RDONLY | O_NONBLOCK);
	if (s->dvr < 0) {
		fprintf(stderr, "tda1002x(icast): cannot open %s: %s\n",
			o->dvr, strerror(errno));
		goto fail;
	}

	if (ioctl(s->dmx, DMX_START, 0) < 0) {
		fprintf(stderr, "tda1002x(icast): DMX_START on %s failed: %s\n",
			o->dmx, strerror(errno));
		goto fail;
	}

	return 0;

fail:
	if (s->dvr >= 0)
		close(s->dvr);
	if (s->dmx >= 0)
		close(s->dmx);
	if (s->fe >= 0)
		close(s->fe);
	s->fe = s->dmx = s->dvr = -1;
	return -1;
}

static void sess_close(struct session *s)
{
	if (s->dvr >= 0)
		close(s->dvr);
	if (s->dmx >= 0) {
		ioctl(s->dmx, DMX_STOP, 0);
		close(s->dmx);
	}
	if (s->fe >= 0)
		close(s->fe);
	s->fe = s->dmx = s->dvr = -1;
}

/*
 * Throw away everything the dvr already queued.
 *
 * This is what makes a retune trustworthy.  The dvr ring is refilled by the
 * URBs continuously, so after a retune it still holds up to a few hundred
 * milliseconds of the *previous* multiplex; reading that as the new
 * frequency is exactly the false positive an earlier version produced on
 * nearly every step.  Reading until two consecutive empty polls means the
 * ring is genuinely drained at this instant.
 */
static int drain(int dvr)
{
	unsigned char b[65536];
	int total = 0, idle = 0;

	while (idle < 2) {
		ssize_t n = read(dvr, b, sizeof(b));

		if (n > 0) {
			total += (int)n;
			idle = 0;
			continue;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
			break;
		if (++idle >= 2)
			break;
		usleep(5000);
	}
	return total;
}

/*
 * Look for a PID 0x0000 packet in a chunk of transport stream.  A demodulated
 * stream on a live cable channel always carries one within a few hundred ms;
 * noise produces no 188-byte aligned 0x47 packets at all.
 */
static int count_pat(const unsigned char *buf, int len)
{
	int i, n = 0;

	for (i = 0; i + 188 <= len; i++) {
		int pid;

		if (buf[i] != 0x47)
			continue;

		pid = ((buf[i + 1] & 0x1f) << 8) | buf[i + 2];
		if (pid != 0x0000)
			continue;

		/* confirm the packet grid before trusting it */
		if (i + 2 * 188 <= len && buf[i + 188] != 0x47)
			continue;
		n++;
		i += 187;
	}
	return n;
}

/*
 * Ask the demodulator whether it is locked.
 *
 * This is the only trustworthy hit criterion on this stick.  A transport
 * stream byte count is not: when the firmware ignores a retune it keeps
 * pushing the previous multiplex and a PAT-counting probe then "finds" a
 * signal everywhere.  The lock bit comes from the demodulator itself, and
 * the driver clears it at the start of every set_frontend, so it cannot
 * carry over from the previous step either.
 */
static int wait_lock(int fe, int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;

	for (;;) {
		fe_status_t status = 0;

		if (ioctl(fe, FE_READ_STATUS, &status) == 0 &&
		    (status & FE_HAS_LOCK))
			return 1;
		if (now_ms() >= deadline)
			return 0;
		usleep(40 * 1000);
	}
}

/*
 * transport_stream_id of the first PAT in a chunk, or -1.  Only used for
 * diagnostics: a sweep that reports the same tsid on consecutive
 * frequencies is reading a stream the stick never actually retuned away
 * from.
 */
static int pat_tsid(const unsigned char *buf, int len)
{
	int i;

	for (i = 0; i + 188 <= len; i++) {
		const unsigned char *p;
		int pid, off;

		if (buf[i] != 0x47)
			continue;
		if (!(buf[i + 1] & 0x40))	/* payload_unit_start_indicator */
			continue;
		pid = ((buf[i + 1] & 0x1f) << 8) | buf[i + 2];
		if (pid != 0x0000)
			continue;

		off = i + 4;
		if (buf[i + 3] & 0x20)		/* adaptation field present */
			off += 1 + buf[i + 4];
		if (off + 8 > len)
			continue;
		p = buf + off;
		if (*p != 0x00)			/* pointer_field must be 0 here */
			continue;
		p++;
		if (p[0] != 0x00)		/* table_id PAT */
			continue;
		return (p[3] << 8) | p[4];
	}
	return -1;
}

/*
 * Read from the dvr for a while, counting PAT packets.
 *
 * Two consecutive PATs are required.  A single one can be leftover data
 * copied before the retune -- on an empty frequency that produces a false
 * hit.  A channel that is really there repeats the PAT about ten times a
 * second, so requiring two costs nothing and removes the false positives.
 */
static int probe_ts(int dvr, int wait_ms, int *tsid_out)
{
	long long deadline = now_ms() + wait_ms;
	unsigned char buf[65536];
	int pats = 0;

	while (now_ms() < deadline) {
		fd_set rf;
		struct timeval tv = { 0, 100000 };
		int n, tsid;

		FD_ZERO(&rf);
		FD_SET(dvr, &rf);
		if (select(dvr + 1, &rf, NULL, NULL, &tv) <= 0)
			continue;

		n = read(dvr, buf, sizeof(buf));
		if (n <= 0)
			continue;
		pats += count_pat(buf, n);
		tsid = pat_tsid(buf, n);
		if (tsid >= 0 && tsid_out && *tsid_out < 0)
			*tsid_out = tsid;
		if (pats >= 2)
			return 1;
	}
	return 0;
}

/*
 * Probe one frequency inside an already-open session.
 *
 * Order matters: lock first, then let the old multiplex run out of the dvr,
 * then look for a PAT.  Waiting for the lock before reading the stream is
 * what keeps an empty frequency from being judged on the previous
 * frequency's bytes.
 *
 * Returns 1 on a hit, 0 when the frequency is empty, -1 when tuning failed.
 */
static int sess_probe(struct session *s, const struct opts *o,
		      unsigned freq_khz, unsigned sr, enum fe_modulation mod,
		      int *tsid_out)
{
	if (tune(s->fe, freq_khz, sr, mod) < 0)
		return -1;

	if (!wait_lock(s->fe, o->lock_ms))
		return 0;

	/*
	 * The demodulator needs a moment to settle after a retune, and the
	 * dvr still holds the tail of the previous multiplex.  Waiting here
	 * costs 0.6 s only on frequencies that actually locked, which is a
	 * handful out of the whole raster.
	 */
	usleep(o->settle_ms * 1000);
	drain(s->dvr);

	return probe_ts(s->dvr, o->wait_ms, tsid_out);
}

/* ------------------------------------------------------------------ */

static int mod_num(enum fe_modulation m)
{
	switch (m) {
	case QAM_16: return 16;
	case QAM_32: return 32;
	case QAM_128: return 128;
	case QAM_256: return 256;
	default: return 64;
	}
}

static const char *mod_name(enum fe_modulation m)
{
	switch (m) {
	case QAM_16: return "QAM16";
	case QAM_32: return "QAM32";
	case QAM_128: return "QAM128";
	case QAM_256: return "QAM256";
	default: return "QAM64";
	}
}

static void logline(const struct opts *o, const char *fmt, ...)
{
	va_list ap;
	FILE *f;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	if (!o->logpath)
		return;
	f = fopen(o->logpath, "a");
	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static void usage(const char *a)
{
	fprintf(stderr,
		"usage: %s [--init] [--scan] [--verbose] [--wakeup] [--inv N] [--bus N]\n"
		"          [--scan-stepsize KHZ] [--scan-bands MASK] [--mod 16|32|64|128|256]\n"
		"          [--sr HZ] [--wait MS] [--from KHZ] [--to KHZ] [--step-exact]\n"
		"          [--fe PATH] [--dmx PATH] [--dvr PATH] [--log PATH]\n"
		"          [--single] [--lock-ms MS] [--settle MS] [--gap MS]\n", a);
}

int main(int argc, char **argv)
{
	struct opts o;
	struct session s;
	unsigned step;
	int i, j, found = 0;

	memset(&o, 0, sizeof(o));
	o.step_khz = 0;			/* 0 = take E2's value, see below */
	o.first_khz = DEFAULT_FIRST_KHZ;
	o.last_khz = DEFAULT_LAST_KHZ;
	o.wait_ms = 400;
	o.lock_ms = 400;
	o.settle_ms = 600;
	o.gap_ms = 30;
	o.open_ms = 30000;
	o.fe = "/dev/dvb/adapter1/frontend0";
	o.dmx = "/dev/dvb/adapter1/demux0";
	o.dvr = "/dev/dvb/adapter1/dvr0";
	o.logpath = "/var/log/tda1002x.log";

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (!strcmp(a, "--verbose")) {
			o.verbose = 1;
		} else if (!strcmp(a, "--single")) {
			o.single = 1;
		} else if (!strcmp(a, "--step-exact")) {
			o.step_exact = 1;
		} else if (!strcmp(a, "--lock-ms") && v) {
			o.lock_ms = atoi(v);
			i++;
		} else if (!strcmp(a, "--settle") && v) {
			o.settle_ms = atoi(v);
			i++;
		} else if (!strcmp(a, "--gap") && v) {
			o.gap_ms = atoi(v);
			i++;
		} else if (!strcmp(a, "--open-timeout") && v) {
			o.open_ms = atoi(v);
			i++;
		} else if (!strcmp(a, "--tap-probe") && v) {
			o.tap_probe = atoi(v);
			i++;
		} else if (!strcmp(a, "--sec-probe") && v) {
			o.sec_probe = atoi(v);
			i++;
		} else if (!strcmp(a, "--log") && v) {
			o.logpath = v;
			i++;
		} else if (!strcmp(a, "--init") || !strcmp(a, "--scan") ||
			   !strcmp(a, "--wakeup")) {
			/* accepted for compatibility with the vendor CLI */
		} else if (!strcmp(a, "--scan-stepsize") && v) {
			o.step_khz = (unsigned)strtoul(v, NULL, 10);
			i++;
		} else if (!strcmp(a, "--scan-bands") && v) {
			/* band mask: we always cover the whole cable spectrum */
			i++;
		} else if (!strcmp(a, "--mod") && v) {
			if (o.nmod < MAX_MOD) {
				int m = atoi(v);
				if (m == 16) o.mods[o.nmod++] = QAM_16;
				else if (m == 32) o.mods[o.nmod++] = QAM_32;
				else if (m == 64) o.mods[o.nmod++] = QAM_64;
				else if (m == 128) o.mods[o.nmod++] = QAM_128;
				else if (m == 256) o.mods[o.nmod++] = QAM_256;
			}
			i++;
		} else if (!strcmp(a, "--sr") && v) {
			if (o.nsr < MAX_SR)
				o.srs[o.nsr++] = (unsigned)strtoul(v, NULL, 10);
			i++;
		} else if (!strcmp(a, "--from") && v) {
			o.first_khz = (unsigned)strtoul(v, NULL, 10);
			i++;
		} else if (!strcmp(a, "--to") && v) {
			o.last_khz = (unsigned)strtoul(v, NULL, 10);
			i++;
		} else if (!strcmp(a, "--wait") && v) {
			o.wait_ms = atoi(v);
			i++;
		} else if (!strcmp(a, "--fe") && v) {
			o.fe = v;
			i++;
		} else if (!strcmp(a, "--dmx") && v) {
			o.dmx = v;
			i++;
		} else if (!strcmp(a, "--dvr") && v) {
			o.dvr = v;
			i++;
		} else if (!strcmp(a, "--bus") && v) {
			/* the iCast stick always lives on adapter1; ignore the I2C bus */
			i++;
		} else if (!strcmp(a, "--inv") && v) {
			i++;
		} else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage(argv[0]);
			return 0;
		}
	}

	if (!o.nsr) {
		o.srs[0] = 6875000;
		o.nsr = 1;
	}
	if (!o.nmod) {
		o.mods[0] = QAM_64;
		o.nmod = 1;
	}

	/*
	 * Raster.  E2 passes its configured step (1000 kHz by default), and
	 * a 1 MHz walk from 115 to 862 MHz is 748 probes: minutes of USB
	 * traffic for a spectrum that is planned on an 8 MHz grid.  Take the
	 * caller's value when it is coarser than the grid, otherwise use the
	 * grid -- unless --step-exact says the caller really meant it.
	 */
	step = o.step_khz;
	if (step == 0)
		step = DEFAULT_STEP_KHZ;
	else if (step < DEFAULT_STEP_KHZ && !o.step_exact)
		step = DEFAULT_STEP_KHZ;
	if (step < 1000)
		step = 1000;
	if (step > 10000)
		step = 10000;

	/*
	 * Diagnostic: watch the transport stream without opening the frontend.
	 *
	 * Enigma2 holds adapter1/frontend0 for itself, so during an E2-driven
	 * scan no helper can look at the tuner.  The demux has no such
	 * restriction, and a PES tap on it says straight away whether the
	 * stick is actually pushing a stream: bytes arriving means the URB
	 * pump is running, silence means the frontend may well be locked but
	 * nothing is being transferred.  Those two failures look identical
	 * from Enigma2's side ("0 channels found").
	 */
	if (o.tap_probe > 0) {
		struct dmx_pes_filter_params p;
		unsigned char buf[65536];
		long long deadline;
		long total = 0;
		int pats = 0, dmx, dvr;

		dmx = open(o.dmx, O_RDWR | O_NONBLOCK);
		if (dmx < 0) {
			fprintf(stderr, "tda1002x(icast): tap: cannot open %s: %s\n",
				o.dmx, strerror(errno));
			return 1;
		}
		memset(&p, 0, sizeof(p));
		p.pid = 0x2000;
		p.input = DMX_IN_FRONTEND;
		p.output = DMX_OUT_TS_TAP;
		p.pes_type = DMX_PES_OTHER;
		if (ioctl(dmx, DMX_SET_PES_FILTER, &p) < 0) {
			fprintf(stderr, "tda1002x(icast): tap: PES filter failed: %s\n",
				strerror(errno));
			close(dmx);
			return 1;
		}
		dvr = open(o.dvr, O_RDONLY | O_NONBLOCK);
		if (dvr < 0) {
			fprintf(stderr, "tda1002x(icast): tap: cannot open %s: %s\n",
				o.dvr, strerror(errno));
			close(dmx);
			return 1;
		}
		ioctl(dmx, DMX_START, 0);

		deadline = now_ms() + o.tap_probe * 1000;
		while (now_ms() < deadline) {
			fd_set rf;
			struct timeval tv = { 0, 100000 };
			int n;

			FD_ZERO(&rf);
			FD_SET(dvr, &rf);
			if (select(dvr + 1, &rf, NULL, NULL, &tv) <= 0)
				continue;
			n = read(dvr, buf, sizeof(buf));
			if (n > 0) {
				total += n;
				pats += count_pat(buf, n);
			}
		}

		printf("tap-probe: %ld bytes, %d PAT in %d s -> %s\n",
		       total, pats, o.tap_probe, total > 0 ? "STREAMING" : "NO DATA");

		ioctl(dmx, DMX_STOP, 0);
		close(dvr);
		close(dmx);
		return total > 0 ? 0 : 1;
	}

	/*
	 * Diagnostic: can a section filter on this adapter deliver anything?
	 *
	 * This is the difference that decides whether Enigma2's own scan can
	 * work.  Enigma2 reads PAT/PMT/SDT through section filters, so if a
	 * PID-0 filter sits silent while the PES tap above is showing 38 Mbit
	 * of transport stream, then no amount of retuning will ever make E2
	 * find a channel -- the stream simply never reaches its readers.
	 * Section payloads are read from the demux fd itself; only a
	 * DMX_OUT_TS_TAP filter feeds the dvr.
	 */
	if (o.sec_probe > 0) {
		struct dmx_sct_filter_params sf;
		unsigned char buf[65536];
		long long deadline;
		long total = 0;
		int sections = 0, dmx;

		dmx = open(o.dmx, O_RDWR | O_NONBLOCK);
		if (dmx < 0) {
			fprintf(stderr, "tda1002x(icast): sec: cannot open %s: %s\n",
				o.dmx, strerror(errno));
			return 1;
		}
		memset(&sf, 0, sizeof(sf));
		sf.pid = 0x0000;
		sf.filter.filter[0] = 0x00;
		sf.filter.mask[0] = 0xff;
		sf.timeout = 0;
		sf.flags = 0;
		if (ioctl(dmx, DMX_SET_FILTER, &sf) < 0) {
			fprintf(stderr, "tda1002x(icast): sec: DMX_SET_FILTER failed: %s\n",
				strerror(errno));
			close(dmx);
			return 1;
		}
		ioctl(dmx, DMX_START, 0);

		deadline = now_ms() + o.sec_probe * 1000;
		while (now_ms() < deadline) {
			fd_set rf;
			struct timeval tv = { 0, 100000 };
			int n;

			FD_ZERO(&rf);
			FD_SET(dmx, &rf);
			if (select(dmx + 1, &rf, NULL, NULL, &tv) <= 0)
				continue;
			n = read(dmx, buf, sizeof(buf));
			if (n > 0) {
				int i;

				total += n;
				for (i = 0; i < n; i++)
					if (buf[i] == 0x00 && i + 3 < n &&
					    buf[i + 1] == 0xb0)
						sections++;
			}
		}

		printf("sec-probe: %ld bytes, ~%d PAT sections in %d s -> %s\n",
		       total, sections, o.sec_probe, total > 0 ? "OK" : "NO SECTION DATA");

		ioctl(dmx, DMX_STOP, 0);
		close(dmx);
		return total > 0 ? 0 : 1;
	}

	/* ---------------- single-shot mode -------------------------------- */
	if (o.single) {
		int hit;
		int tsid = -1;

		if (sess_open(&o, &s) < 0)
			return -1;
		hit = sess_probe(&s, &o, o.first_khz, o.srs[0],
				 (enum fe_modulation)o.mods[0], &tsid);
		sess_close(&s);

		if (hit > 0) {
			printf("OK %u %u FEC_AUTO %s INVERSION_AUTO\n",
			       o.first_khz, o.srs[0], mod_name(o.mods[0]));
			fflush(stdout);
			return 0;
		}
		return 1;
	}

	logline(&o, "tda1002x(icast): %u..%u kHz step %u, %d sr, %d qam, one session\n",
		o.first_khz, o.last_khz, step, o.nsr, o.nmod);

	/*
	 * Fail loudly rather than exiting 0 with no hits: E2 treats a silent
	 * empty result as "nothing on the cable", and that hides a device
	 * that could not even be opened.
	 */
	if (sess_open(&o, &s) < 0) {
		logline(&o, "tda1002x(icast): cannot open %s / %s / %s: %s\n",
			o.fe, o.dmx, o.dvr, strerror(errno));
		return 1;
	}

	for (j = 0; j < o.nsr; j++) {
		unsigned freq;

		for (freq = o.first_khz; freq <= o.last_khz; freq += step) {
			int m, tsid = -1, hit = 0;

			for (m = 0; m < o.nmod; m++) {
				hit = sess_probe(&s, &o, freq, o.srs[j],
						 (enum fe_modulation)o.mods[m], &tsid);
				if (hit < 0)
					break;
				if (hit > 0)
					break;	/* first working modulation wins */
			}

			if (hit > 0) {
				found++;
				printf("OK %u %u FEC_AUTO %s INVERSION_AUTO\n",
				       freq, o.srs[j], mod_name(o.mods[m]));
				fflush(stdout);
				logline(&o, "tda1002x(icast): hit %u kHz / %u S/s tsid=%d\n",
					freq, o.srs[j], tsid);
			} else if (o.verbose) {
				logline(&o, "tda1002x(icast): %u kHz %s\n", freq,
					hit < 0 ? "tune error" : "no lock");
			}

			if (o.gap_ms > 0)
				usleep(o.gap_ms * 1000);
		}
	}

	sess_close(&s);

	logline(&o, "tda1002x(icast): done, %d transponder(s) of %u..%u kHz\n",
		found, o.first_khz, o.last_khz);
	return 0;
}
