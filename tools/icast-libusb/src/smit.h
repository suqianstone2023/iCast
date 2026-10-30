/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SMIT iCast DTMB/DVBC USB dongle - userspace driver (libusb).
 *
 * Same wire protocol as smit.c in the kernel driver; only the transport
 * changed (usb_bulk_msg -> libusb_bulk_transfer).  No kernel module, no
 * /dev/dvb, no dependency on a kernel version.
 */

#ifndef ICAST_SMIT_H
#define ICAST_SMIT_H

#include <stdint.h>
#include <libusb.h>

#define ICAST_VID 0x29df
#define ICAST_PID 0x0001

/* Bulk endpoints, as defined in smit.h of the kernel driver. */
#define SMIT_EP_CMD_SEND 0x01
#define SMIT_EP_CMD_RECV 0x82
#define SMIT_EP_STREAM   0x84

/* Payload length of the status response returned by smit_tuner_stat(). */
#define SMIT_STATUS_LEN 32

struct icast_dev {
	libusb_context *ctx;
	libusb_device_handle *h;
	int index;         /* which dongle on the bus this handle refers to */
	int iface;
	int claimed;
	int dtmb;          /* device reported DTMB support during init */
	char model[64];    /* identification string read during init */
	uint32_t frequency_hz;
};

struct icast_status {
	uint32_t frequency_hz;  /* frequency the device reports it locked on */
	uint8_t lock;
	uint8_t snr_raw;        /* 0..255, straight from the device */
	uint8_t strength_raw;   /* 0..255, straight from the device */
	uint16_t snr;           /* 0..65535, same scale as the kernel driver */
	uint16_t strength;      /* 0..65535 */
};

/* Sleep helper. usleep() is obsolescent and gone from POSIX.1-2008, and it
 * does not exist on some targets, so everything uses nanosleep internally. */
void icast_sleep_ms(int ms);

/* When non-zero, every USB transfer, command and status lookup is traced to
 * stderr. Set by -v on the command line. */
extern int icast_debug;

/* When non-zero, icast_open() issues a USB device reset before claiming the
 * interface. Needed on hosts where the dongle sat enumerated for a long time
 * without a driver ever talking to it. */
extern int icast_reset_first;

/* Human readable name for a libusb error code, with a fallback for libusb
 * builds that predate libusb_strerror(). */
const char *icast_usb_error(int r);

/* Number of iCast dongles currently attached. */
int icast_count(void);

/* Open the n-th attached dongle, claim its interface and run the init
 * sequence. Returns 0 on success, negative errno-ish value on failure. */
int icast_open(struct icast_dev *d, int index);
void icast_close(struct icast_dev *d);

/* Probe handshake + full init sequence. Must be called after icast_open(). */
int icast_init(struct icast_dev *d);

/* Put the firmware back into a state where it answers the probe handshake.
 *
 * The dongle answers exactly one init sequence and then goes quiet: a second
 * handshake, even after a replug, gets no reply until the 0xA0 vendor request
 * is issued. That request makes the chip reset itself, which also drops it off
 * the bus, so this reopens the device and reclaims the interface. */
int icast_soft_reset(struct icast_dev *d);

/* Tune. dvbc != 0 selects the DVBC path, otherwise the DTMB path.
 * freq_hz is the centre frequency in Hz, symbol_rate in Sym/s, qam is the
 * constellation (16/32/64/128/256) and bandwidth in MHz only applies to the
 * DTMB path. */
int icast_set_frontend(struct icast_dev *d, uint32_t freq_hz, int dvbc,
		       uint32_t symbol_rate, int qam, int bandwidth);

/* Query lock/signal/SNR. Returns 0 on success, -ENODATA when the device
 * returned a short response (no status available yet). */
int icast_read_status(struct icast_dev *d, struct icast_status *st);

/* Read MPEG-TS from the streaming endpoint. Returns bytes read, or a
 * negative libusb error. */
int icast_read_ts(struct icast_dev *d, uint8_t *buf, int len, int timeout_ms);

#endif /* ICAST_SMIT_H */
