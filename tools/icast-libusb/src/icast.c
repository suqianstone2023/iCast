/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * icast-libusb - userspace driver for the SMIT iCast DTMB/DVBC USB dongle.
 *
 * Drives the stick directly over libusb, with no kernel module and no
 * /dev/dvb.  Tuning and status come from the same commands the kernel
 * driver sends; the transport stream is read from bulk endpoint 0x84 and
 * written to a file, stdout, or a UDP socket (for tvheadend IPTV input).
 */

#include "sink.h"
#include "smit.h"
#include "tsfilter.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "1.0"
#endif

/* TS_PACKET and the datagram convention live in sink.h. */
#define TS_CHUNK (128 * 1024)

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* libusb_strerror() only exists from 1.0.16 on; fall back for older builds. */
static const char *usb_error(int r)
{
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000103
	return libusb_strerror((enum libusb_error)r);
#else
	return libusb_error_name(r);
#endif
}

static double now_seconds(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------- cli */

static void usage(FILE *f)
{
	fprintf(f,
		"icast-libusb %s - userspace driver for the SMIT iCast DTMB/DVBC dongle\n"
		"\n"
		"usage: icast-libusb [options] <command>\n"
		"\n"
		"commands:\n"
		"  list                 list attached iCast dongles\n"
		"  info                 show device mode and identification string\n"
		"  tune                 tune and report lock status\n"
		"  stream               tune and stream the transport stream out\n"
		"\n"
		"options:\n"
		"  -d, --device N       n-th dongle, 0 based (default 0)\n"
		"  -f, --freq MHZ       centre frequency in MHz, e.g. 474 or 474.5\n"
		"  -C, --dvbc           DVBC instead of DTMB/DVBT\n"
		"  -r, --symbol-rate KSYM  DVBC symbol rate in kSym/s (default 6875)\n"
		"  -m, --qam N          DVBC constellation: 16/32/64/128/256 (default 64)\n"
		"  -b, --bandwidth MHZ  DTMB bandwidth (default 8)\n"
		"  -o, --out TARGET     '-' stdout, a file, or udp://239.1.1.1:5000\n"
		"                       repeatable: every -o gets the whole multiplex\n"
		"  -p, --program N=TARGET  like -o but sends only program N, with a\n"
		"                       rewritten PAT (repeatable, this is what a\n"
		"                       settop box needs: one service per port)\n"
		"  -M, --monitor        keep polling status after tuning\n"
		"  -t, --seconds N      stop after N seconds (default: run until Ctrl-C)\n"
		"  -q, --quiet          suppress status lines on stderr\n"
		"  -v, --verbose        trace every USB transfer (device bring-up)\n"
		"  -R, --reset          force a vendor reset before init\n"
		"  -h, --help           this text\n"
		"\n"
		"examples:\n"
		"  icast-libusb info\n"
		"  icast-libusb tune -f 474 -M\n"
		"  icast-libusb stream -f 474 -o udp://239.1.1.1:5000\n"
		"  icast-libusb stream -f 482 -o /tmp/mux.ts -t 30\n"
		"  icast-libusb stream -C -f 251 -p 220=udp://127.0.0.1:5001 \\\n"
		"                       -p 221=udp://127.0.0.1:5002\n",
		PACKAGE_VERSION);
}

static const char *mode_name(const struct icast_dev *d)
{
	return d->dtmb ? "DTMB/DVBC (dual mode)" : "DVBC (single mode)";
}

static void print_status(const struct icast_status *st, int have_status)
{
	if (!have_status) {
		fprintf(stderr, "  no lock (device returned no status)\n");
		return;
	}

	fprintf(stderr, "  lock=%s  freq=%.3f MHz  snr=%u%%  level=%u%%\n",
		st->lock ? "YES" : "no", st->frequency_hz / 1e6,
		(st->snr_raw * 100u + 127u) / 255u,
		(st->strength_raw * 100u + 127u) / 255u);
}

static int cmd_list(void)
{
	libusb_context *ctx = NULL;
	libusb_device **list = NULL;
	ssize_t n, i;
	int count = 0;

	if (libusb_init(&ctx) != 0) {
		fprintf(stderr, "icast: libusb init failed\n");
		return 1;
	}

	n = libusb_get_device_list(ctx, &list);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (desc.idVendor != ICAST_VID || desc.idProduct != ICAST_PID)
			continue;

		printf("bus %03u device %03u  ID %04x:%04x  (index %d)\n",
		       libusb_get_bus_number(list[i]),
		       libusb_get_device_address(list[i]), desc.idVendor,
		       desc.idProduct, count);
		count++;
	}

	libusb_free_device_list(list, 1);
	libusb_exit(ctx);

	if (!count) {
		fprintf(stderr, "no iCast dongle (%04x:%04x) found\n", ICAST_VID,
			ICAST_PID);
		return 1;
	}
	return 0;
}

#define MAX_OUT 8

struct out_spec {
	const char *target;
	int program; /* 0 = whole multiplex */
};

struct options {
	int index;
	int dvbc;
	double freq_mhz;
	double symbol_rate_ksym;
	int qam;
	int bandwidth;
	struct out_spec out[MAX_OUT];
	int n_out;
	int monitor;
	int seconds;
	int quiet;
};

static int open_and_tune(struct icast_dev *dev, const struct options *o)
{
	uint32_t freq_hz;
	int r;

	freq_hz = (uint32_t)(o->freq_mhz * 1e6 + 0.5);

	r = icast_open(dev, o->index);
	if (r < 0) {
		fprintf(stderr, "icast: cannot open dongle #%d: %s\n", o->index,
			r == -ENODEV ? "not found" : usb_error(r));
		return 1;
	}

	fprintf(stderr, "device: %s\n", mode_name(dev));

	r = icast_init(dev);
	if (r < 0) {
		fprintf(stderr, "icast: device init failed: %s\n",
			usb_error(r));
		icast_close(dev);
		return 1;
	}

	fprintf(stderr, "tuning: %.3f MHz %s\n", o->freq_mhz,
		o->dvbc ? "DVBC" : "DTMB");

	r = icast_set_frontend(dev, freq_hz, o->dvbc,
			       (uint32_t)(o->symbol_rate_ksym * 1000 + 0.5),
			       o->qam, o->bandwidth);
	if (r < 0) {
		fprintf(stderr, "icast: tuning failed: %s\n", usb_error(r));
		icast_close(dev);
		return 1;
	}

	return 0;
}

/*
 * Tuning only queues the request: the demodulator takes a second or two to
 * lock, and a tune that lands while the firmware is still settling is simply
 * ignored. Poll for a lock, and retune once if it never arrives - otherwise
 * the first read of the transport stream blocks until it times out.
 */
static int ensure_lock(struct icast_dev *dev, const struct options *o)
{
	struct icast_status st;
	uint32_t freq_hz = (uint32_t)(o->freq_mhz * 1e6 + 0.5);
	int attempt;
	int i;

	for (attempt = 0; attempt < 2; attempt++) {
		for (i = 0; i < 16; i++) {
			if (icast_read_status(dev, &st) == 0 && st.lock) {
				/*
				 * Always reported, quiet or not: this is the one
				 * line an init script can show to prove the
				 * tuner came up.
				 */
				fprintf(stderr,
					"lock: %.3f MHz  snr=%u%%  level=%u%%\n",
					st.frequency_hz / 1e6,
					(st.snr_raw * 100u + 127u) / 255u,
					(st.strength_raw * 100u + 127u) / 255u);
				return 0;
			}
			icast_sleep_ms(500);
		}
		if (attempt == 0) {
			if (!o->quiet)
				fprintf(stderr, "icast: no lock yet, retuning\n");
			if (icast_set_frontend(dev, freq_hz, o->dvbc,
					       (uint32_t)(o->symbol_rate_ksym * 1000 + 0.5),
					       o->qam, o->bandwidth) < 0)
				break;
		}
	}

	return -1;
}

static int cmd_info(const struct options *o)
{
	struct icast_dev dev;
	struct icast_status st;
	int have;
	int r;

	r = icast_open(&dev, o->index);
	if (r < 0) {
		fprintf(stderr, "icast: cannot open dongle #%d: %s\n", o->index,
			r == -ENODEV ? "not found" : usb_error(r));
		return 1;
	}

	r = icast_init(&dev);
	if (r < 0) {
		fprintf(stderr, "icast: device init failed: %s\n",
			usb_error(r));
		icast_close(&dev);
		return 1;
	}

	printf("mode : %s\n", mode_name(&dev));
	printf("id   : %s\n", dev.model[0] ? dev.model : "(no identification)");

	have = icast_read_status(&dev, &st) == 0;
	if (have)
		printf("lock : %s at %.3f MHz\n", st.lock ? "yes" : "no",
		       st.frequency_hz / 1e6);
	else
		printf("lock : unknown\n");

	icast_close(&dev);
	return 0;
}

static int cmd_tune(const struct options *o)
{
	struct icast_dev dev;
	struct icast_status st;
	double deadline;
	int r;

	r = open_and_tune(&dev, o);
	if (r)
		return r;

	/* The kernel driver uses min_delay_ms = 800 for this tuner. */
	icast_sleep_ms(900);

	deadline = o->seconds > 0 ? now_seconds() + o->seconds : 0;

	do {
		int have = icast_read_status(&dev, &st) == 0;

		if (!o->quiet) {
			printf("status:\n");
			print_status(&st, have);
			fflush(stdout);
		}

		if (!o->monitor)
			break;

		icast_sleep_ms(500);
	} while (!stop_requested &&
		 (deadline == 0 || now_seconds() < deadline));

	icast_close(&dev);
	return 0;
}

static int cmd_stream(const struct options *o)
{
	struct icast_dev dev;
	struct icast_status st;
	static struct sink out[MAX_OUT];
	static struct ts_filter filt[MAX_OUT];
	int nout = o->n_out;
	uint8_t *ts;
	double start, last_report, deadline, last_data;
	unsigned long long last_bytes = 0;
	int recovery = 0;
	int i, r;

	r = open_and_tune(&dev, o);
	if (r)
		return r;

	for (i = 0; i < nout; i++) {
		struct ts_filter *f = NULL;

		if (o->out[i].program > 0) {
			ts_filter_init(&filt[i], o->out[i].program);
			f = &filt[i];
		}
		r = sink_open(&out[i], o->out[i].target, f);
		if (r < 0) {
			while (i-- > 0)
				sink_close(&out[i]);
			icast_close(&dev);
			return 1;
		}
	}

	ts = malloc(TS_CHUNK);
	if (!ts) {
		for (i = 0; i < nout; i++)
			sink_close(&out[i]);
		icast_close(&dev);
		return 1;
	}

	/* Reading before the demodulator locks just returns timeouts. */
	if (ensure_lock(&dev, o) < 0 && !o->quiet)
		fprintf(stderr, "icast: warning: no lock, reading anyway\n");

	start = now_seconds();
	last_report = start;
	last_data = start;
	deadline = o->seconds > 0 ? start + o->seconds : 0;

	while (!stop_requested) {
		int n = icast_read_ts(&dev, ts, TS_CHUNK, 2000);
		double t = now_seconds();

		if (n > 0) {
			if (recovery) {
				fprintf(stderr, "icast: data flowing again\n");
				recovery = 0;
			}
			last_data = t;
		} else if (t - last_data >= 8.0) {
			/*
			 * The firmware sometimes accepts the tune and reports a
			 * lock but never starts delivering the transport stream,
			 * and a process that was killed mid-stream leaves it that
			 * way. Retune first; if that does not take, go through the
			 * vendor reset - it is the only thing that clears it.
			 * Keep cycling rather than giving up: a 24/7 box has to
			 * come back by itself once the signal returns.
			 */
			recovery++;
			if (!o->quiet)
				fprintf(stderr,
					"icast: no data for %.0fs, %s\n",
					t - last_data,
					recovery <= 3 ? "retuning" :
					recovery <= 9 ? "reinitialising" :
							"retuning");
			if (recovery <= 3 || recovery > 9) {
				icast_set_frontend(&dev,
					(uint32_t)(o->freq_mhz * 1e6 + 0.5),
					o->dvbc,
					(uint32_t)(o->symbol_rate_ksym * 1000 + 0.5),
					o->qam, o->bandwidth);
			} else if (icast_soft_reset(&dev) == 0) {
				icast_init(&dev);
				icast_set_frontend(&dev,
					(uint32_t)(o->freq_mhz * 1e6 + 0.5),
					o->dvbc,
					(uint32_t)(o->symbol_rate_ksym * 1000 + 0.5),
					o->qam, o->bandwidth);
			}
			last_data = now_seconds();
			continue;
		}

		if (n < 0) {
			if (n == LIBUSB_ERROR_NO_DEVICE) {
				fprintf(stderr, "icast: device disconnected\n");
				break;
			}
			continue;
		}

		if (n > 0) {
			int failed = 0;

			for (i = 0; i < nout; i++) {
				int w = sink_write(&out[i], ts, (size_t)n);

				if (w < 0) {
					fprintf(stderr,
						"icast: write failed on '%s': %s (fd=%d udp=%d dst=%08x:%u len=%zu nudp=%llu)\n",
						o->out[i].target, strerror(-w),
						out[i].sock, out[i].is_udp,
						(unsigned)out[i].dst_addr,
						(unsigned)out[i].dst_port,
						out[i].len, out[i].datagrams);
					failed = 1;
				}
			}
			if (failed)
				break;
		}

		t = now_seconds();
		/*
		 * A service log needs a heartbeat, but one line a second would
		 * fill a tmpfs in a few weeks. Quiet mode still reports, just
		 * every minute - that is what "icast-stream status" tails.
		 */
		if (t - last_report >= (o->quiet ? 60.0 : 1.0)) {
			unsigned long long sent = 0, dg = 0, dr = 0;
			double dt = t - last_report;
			double rate;

			for (i = 0; i < nout; i++) {
				sent += out[i].bytes;
				dg += out[i].datagrams;
				dr += out[i].dropped;
			}
			rate = (double)(sent - last_bytes) / dt;
			last_bytes = sent;
			last_report = t;

			if (icast_read_status(&dev, &st) == 0 && st.lock)
				fprintf(stderr,
					"%s %7.1fs %.2f Mbit/s lock=yes snr=%u%% level=%u%% udp=%llu drop=%llu\n",
					o->quiet ? "hb" : "  ", t - start,
					rate * 8 / 1e6,
					(st.snr_raw * 100u + 127u) / 255u,
					(st.strength_raw * 100u + 127u) / 255u,
					dg, dr);
			else
				fprintf(stderr,
					"%s %7.1fs %.2f Mbit/s lock=no udp=%llu drop=%llu\n",
					o->quiet ? "hb" : "  ", t - start,
					rate * 8 / 1e6, dg, dr);
		}

		if (deadline && t >= deadline)
			break;
	}

	for (i = 0, r = 0; i < nout; i++) {
		fprintf(stderr, "  '%s': %llu bytes\n", o->out[i].target,
			out[i].bytes);
		sink_close(&out[i]);
	}
	fprintf(stderr, "streamed in %.1f s\n", now_seconds() - start);

	free(ts);
	icast_close(&dev);
	return 0;
}

int main(int argc, char **argv)
{
	struct options o;
	const char *cmd;
	int c;

	memset(&o, 0, sizeof(o));
	o.symbol_rate_ksym = 6875;
	o.qam = 64;
	o.bandwidth = 8;

	static const struct option long_opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "freq", required_argument, NULL, 'f' },
		{ "dvbc", no_argument, NULL, 'C' },
		{ "symbol-rate", required_argument, NULL, 'r' },
		{ "qam", required_argument, NULL, 'm' },
		{ "bandwidth", required_argument, NULL, 'b' },
		{ "out", required_argument, NULL, 'o' },
		{ "program", required_argument, NULL, 'p' },
		{ "monitor", no_argument, NULL, 'M' },
		{ "seconds", required_argument, NULL, 't' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "reset", no_argument, NULL, 'R' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while ((c = getopt_long(argc, argv, "d:f:Cr:m:b:o:p:Mt:qvRh", long_opts,
				NULL)) != -1) {
		switch (c) {
		case 'd':
			o.index = atoi(optarg);
			break;
		case 'f':
			o.freq_mhz = atof(optarg);
			break;
		case 'C':
			o.dvbc = 1;
			break;
		case 'r':
			o.symbol_rate_ksym = atof(optarg);
			break;
		case 'm':
			o.qam = atoi(optarg);
			break;
		case 'b':
			o.bandwidth = atoi(optarg);
			break;
		case 'o':
			if (o.n_out >= MAX_OUT) {
				fprintf(stderr, "icast: too many outputs (max %d)\n",
					MAX_OUT);
				return 2;
			}
			o.out[o.n_out].target = optarg;
			o.out[o.n_out].program = 0;
			o.n_out++;
			break;
		case 'p': {
			/*
			 * N=TARGET. The program number has to be resolved from the
			 * PAT at run time, so it is carried as an int here and the
			 * filter learns the PMT and elementary PIDs itself.
			 */
			const char *eq = strchr(optarg, '=');
			int prog;

			if (!eq || eq == optarg) {
				fprintf(stderr,
					"icast: --program needs PROGRAM=TARGET, got '%s'\n",
					optarg);
				return 2;
			}
			prog = atoi(optarg);
			if (prog <= 0 || prog > 0xffff) {
				fprintf(stderr, "icast: bad program number in '%s'\n",
					optarg);
				return 2;
			}
			if (o.n_out >= MAX_OUT) {
				fprintf(stderr, "icast: too many outputs (max %d)\n",
					MAX_OUT);
				return 2;
			}
			o.out[o.n_out].target = eq + 1;
			o.out[o.n_out].program = prog;
			o.n_out++;
			break;
		}
		case 'M':
			o.monitor = 1;
			break;
		case 't':
			o.seconds = atoi(optarg);
			break;
		case 'q':
			o.quiet = 1;
			break;
		case 'v':
			icast_debug = 1;
			break;
		case 'R':
			icast_reset_first = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	if (optind >= argc) {
		usage(stderr);
		return 2;
	}
	cmd = argv[optind];

	if (o.n_out == 0) {
		o.out[0].target = "-";
		o.out[0].program = 0;
		o.n_out = 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (strcmp(cmd, "list") == 0)
		return cmd_list();
	if (strcmp(cmd, "info") == 0)
		return cmd_info(&o);

	if (strcmp(cmd, "tune") == 0 || strcmp(cmd, "stream") == 0) {
		if (o.freq_mhz <= 0) {
			fprintf(stderr, "icast: '%s' needs -f/--freq\n", cmd);
			return 2;
		}
		return strcmp(cmd, "tune") == 0 ? cmd_tune(&o)
						: cmd_stream(&o);
	}

	fprintf(stderr, "icast: unknown command '%s'\n", cmd);
	usage(stderr);
	return 2;
}
