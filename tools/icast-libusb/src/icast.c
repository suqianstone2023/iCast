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
		"  -M, --monitor        keep polling status after tuning\n"
		"  -t, --seconds N      stop after N seconds (default: run until Ctrl-C)\n"
		"  -q, --quiet          suppress status lines on stderr\n"
		"  -h, --help           this text\n"
		"\n"
		"examples:\n"
		"  icast-libusb info\n"
		"  icast-libusb tune -f 474 -M\n"
		"  icast-libusb stream -f 474 -o udp://239.1.1.1:5000\n"
		"  icast-libusb stream -f 482 -o /tmp/mux.ts -t 30\n",
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

struct options {
	int index;
	int dvbc;
	double freq_mhz;
	double symbol_rate_ksym;
	int qam;
	int bandwidth;
	const char *out;
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
	struct sink out;
	uint8_t *ts;
	double start, last_report, deadline;
	unsigned long long last_bytes = 0;
	int r;

	r = open_and_tune(&dev, o);
	if (r)
		return r;

	r = sink_open(&out, o->out ? o->out : "-");
	if (r < 0) {
		icast_close(&dev);
		return 1;
	}

	ts = malloc(TS_CHUNK);
	if (!ts) {
		sink_close(&out);
		icast_close(&dev);
		return 1;
	}

	/* Give the demodulator time to lock before pulling the stream. */
	icast_sleep_ms(900);

	start = now_seconds();
	last_report = start;
	deadline = o->seconds > 0 ? start + o->seconds : 0;

	while (!stop_requested) {
		int n = icast_read_ts(&dev, ts, TS_CHUNK, 2000);
		double t;

		if (n < 0) {
			if (n == LIBUSB_ERROR_NO_DEVICE) {
				fprintf(stderr, "icast: device disconnected\n");
				break;
			}
			continue;
		}

		if (n > 0 && sink_write(&out, ts, (size_t)n) < 0) {
			fprintf(stderr, "icast: write failed: %s\n",
				strerror(errno));
			break;
		}

		t = now_seconds();
		if (!o->quiet && t - last_report >= 1.0) {
			double dt = t - last_report;
			double rate = (double)(out.bytes - last_bytes) / dt;

			last_bytes = out.bytes;
			last_report = t;

			if (icast_read_status(&dev, &st) == 0 && st.lock)
				fprintf(stderr,
					"  %6.1fs  %.2f MB/s (%.1f Mbit/s)  snr=%u%%  level=%u%%\n",
					t - start, rate / 1e6, rate * 8 / 1e6,
					(st.snr_raw * 100u + 127u) / 255u,
					(st.strength_raw * 100u + 127u) / 255u);
			else
				fprintf(stderr,
					"  %6.1fs  %.2f MB/s (%.1f Mbit/s)  no lock\n",
					t - start, rate / 1e6, rate * 8 / 1e6);
		}

		if (deadline && t >= deadline)
			break;
	}

	fprintf(stderr, "streamed %llu bytes in %.1f s\n", out.bytes,
		now_seconds() - start);

	free(ts);
	sink_close(&out);
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
	o.out = "-";

	static const struct option long_opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "freq", required_argument, NULL, 'f' },
		{ "dvbc", no_argument, NULL, 'C' },
		{ "symbol-rate", required_argument, NULL, 'r' },
		{ "qam", required_argument, NULL, 'm' },
		{ "bandwidth", required_argument, NULL, 'b' },
		{ "out", required_argument, NULL, 'o' },
		{ "monitor", no_argument, NULL, 'M' },
		{ "seconds", required_argument, NULL, 't' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	while ((c = getopt_long(argc, argv, "d:f:Cr:m:b:o:Mt:qh", long_opts,
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
			o.out = optarg;
			break;
		case 'M':
			o.monitor = 1;
			break;
		case 't':
			o.seconds = atoi(optarg);
			break;
		case 'q':
			o.quiet = 1;
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
