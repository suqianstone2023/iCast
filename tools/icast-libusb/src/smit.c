/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SMIT iCast DTMB/DVBC USB dongle - userspace driver (libusb).
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>   (kernel driver)
 *
 * This is a straight port of the transport and command layer of the kernel
 * driver to libusb.  The command tables, their order and the completion
 * polling are deliberately kept byte for byte identical - the protocol was
 * reverse engineered from USB captures and there is no documentation to
 * check a "cleaner" implementation against.
 *
 * Differences from the kernel version, all of them defensive:
 *   - every receive buffer is zeroed first, so a short transfer cannot make
 *     us read stale data from the previous exchange
 *   - the declared payload length is clamped before it is used as a copy
 *     length
 *   - the completion polling loop has a hard iteration cap
 */

#include "smit.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define SMIT_PACK_MAX 1000
#define SMIT_CMD_TIMEOUT 500
#define SMIT_STAT_TIMEOUT 400

void icast_sleep_ms(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

struct smit_pack {
	uint8_t tag;
	uint8_t byte2;
	uint8_t req;
	uint8_t datalen;
	uint8_t data[SMIT_PACK_MAX];
};

static int smit_bulk(struct icast_dev *d, unsigned char ep, void *buf, int len,
		     int *actual, int timeout)
{
	int transferred = 0;
	int r;

	r = libusb_bulk_transfer(d->h, ep, buf, len, &transferred, timeout);
	if (actual)
		*actual = transferred;
	return r;
}

static int smit_control_reset(struct icast_dev *d)
{
	return libusb_control_transfer(d->h, 0x40, 0xA0, 0, 0, NULL, 0, 500);
}

/*
 * Poll until the device reports completion in data[1].  Mirrors smit_check():
 * keep sending the 0xA0/0x01 request until 0x80 comes back, or give up after
 * checkcount tries.
 */
static int smit_check(struct icast_dev *d, int checkcount)
{
	struct smit_pack smit;
	int actual = 0;
	int cnt = 0;
	int r;

	for (;;) {
		memset(&smit, 0, sizeof(smit));
		smit.tag = 0x01;
		smit.req = 0xA0;
		smit.datalen = 0x01;
		smit.data[0] = 0x01;

		r = smit_bulk(d, SMIT_EP_CMD_SEND, &smit, 5, &actual,
			      SMIT_CMD_TIMEOUT);
		if (r < 0)
			return r;

		memset(&smit, 0, sizeof(smit));
		r = smit_bulk(d, SMIT_EP_CMD_RECV, &smit, SMIT_PACK_MAX, &actual,
			      SMIT_CMD_TIMEOUT);
		if (r < 0)
			return r;

		if (smit.tag != 1)
			return -EPROTO;
		if (smit.data[1] == 0x80)
			return 0;
		if (smit.data[1])
			return -EIO;

		cnt++;
		if (checkcount <= 0)
			return 0;
		if (cnt > checkcount)
			break;
		icast_sleep_ms(cnt / 2 + 3);
	}

	return -ETIMEDOUT;
}

/*
 * Send one command: 4 byte header (tag, byte2, req, datalen) followed by the
 * payload, then read the reply back into the same buffer.  Mirrors smit_set().
 */
static int smit_set(struct icast_dev *d, uint8_t req, const uint8_t *data,
		    uint8_t len, int need_complete)
{
	struct smit_pack smit;
	int actual = 0;
	int r;

	memset(&smit, 0, sizeof(smit));
	smit.tag = 0x01;
	smit.byte2 = 0x00;
	smit.req = req;
	smit.datalen = len;
	memcpy(smit.data, data, len);

	r = smit_bulk(d, SMIT_EP_CMD_SEND, &smit, len + 4, &actual,
		      SMIT_CMD_TIMEOUT);
	if (r < 0)
		return r;

	memset(&smit, 0, sizeof(smit));
	r = smit_bulk(d, SMIT_EP_CMD_RECV, &smit, SMIT_PACK_MAX, &actual,
		      SMIT_CMD_TIMEOUT);
	if (r < 0)
		return r;

	if (smit.tag != 1)
		return -EPROTO;

	/* Some commands answer with a fixed short acknowledgement. */
	if (smit.req == 0x80 && smit.data[1] == 0x80)
		return 0;

	if ((int)smit.datalen + 4 >= actual ||
	    smit.data[smit.datalen] != 0x80 ||
	    smit.data[smit.datalen + 3] != 0x80) {
		if (need_complete) {
			icast_sleep_ms(100);
			return smit_check(d, need_complete);
		}
	}

	return 0;
}

/*
 * Ask for tuner status.  Returns the payload length, copies the payload to
 * out.  Mirrors smit_tuner_stat().
 */
static int smit_tuner_stat(struct icast_dev *d, uint8_t *out, int checkcount)
{
	struct smit_pack smit;
	int actual = 0;
	int ret;
	int r;

	memset(&smit, 0, sizeof(smit));
	smit.tag = 0x01;
	smit.req = 0x81;
	smit.datalen = 0x01;
	smit.data[0] = 0x01;

	r = smit_bulk(d, SMIT_EP_CMD_SEND, &smit, 5, &actual, SMIT_STAT_TIMEOUT);
	if (r < 0)
		return r;

	memset(&smit, 0, sizeof(smit));
	r = smit_bulk(d, SMIT_EP_CMD_RECV, &smit, 900, &actual,
		      SMIT_STAT_TIMEOUT);
	if (r < 0)
		return r;

	if (smit.tag != 1)
		return -EPROTO;

	ret = smit.datalen;
	if (ret > SMIT_PACK_MAX)
		ret = SMIT_PACK_MAX;
	memcpy(out, smit.data, (size_t)ret);

	if ((ret >= actual || smit.data[ret] != 0x80 ||
	     smit.data[ret + 3] != 0x80) && checkcount > 0)
		smit_check(d, checkcount);

	return ret;
}

/*
 * Device initialisation.  Command tables and order are copied from
 * smit_init() in the kernel driver.
 */
static int smit_init(struct icast_dev *d)
{
	static const uint8_t cmd0[1] = { 0x01 };
	static const uint8_t cmd3_init[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x01,
					       0x00, 0x41, 0x00, 0x01 };
	static const uint8_t cmd4_init[9] = { 0x01, 0x90, 0x02, 0x00, 0x01, 0x9F,
					      0x80, 0x10, 0x00 };
	static const uint8_t cmd5[37] = {
		0x01, 0x90, 0x02, 0x00, 0x01, 0x9F, 0x80, 0x11, 0x1C, 0x00, 0x01,
		0x00, 0x41, 0x00, 0x02, 0x00, 0x41, 0x00, 0x03, 0x00, 0x41, 0x00,
		0x24, 0x00, 0x41, 0x00, 0x40, 0x00, 0x41, 0x00, 0x96, 0x10, 0x01,
		0x00, 0x20, 0x00, 0x41
	};
	static const uint8_t cmd6[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96,
					  0x10, 0x01, 0x00, 0x03 };
	static const uint8_t cmd7[17] = { 0x01, 0x90, 0x02, 0x00, 0x03, 0x9F,
					  0x9A, 0x00, 0x08, 0x53, 0x4D, 0x69,
					  0x54, 0x5A, 0x42, 0x4A, 0x4C };
	static const uint8_t cmd8[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96,
					  0x10, 0x01, 0x00, 0x04 };
	static const uint8_t cmd10[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x03,
					   0x00, 0x41, 0x00, 0x04 };
	static const uint8_t cmd11[9] = { 0x01, 0x90, 0x02, 0x00, 0x04, 0x9F,
					  0x80, 0x30, 0x00 };
	static const uint8_t cmd12[10] = { 0x01, 0x92, 0x07, 0x00, 0x00, 0x96,
					   0x10, 0x01, 0x00, 0x05 };
	static const uint8_t cmd9[32] = {
		0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
		0x17, 0x00, 0x00, 0x14, 0x00, 0x03, 0x00, 0x10,
		0xB8, 0xF3, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00
	};
	static const uint8_t cmd14[20] = {
		0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07, 0x0B, 0x07,
		0x00, 0x08, 0x10, 0x07, 0x00, 0x04, 0x47, 0x44, 0x48, 0x57
	};

	uint8_t cmd3[sizeof(cmd3_init)];
	uint8_t cmd4[sizeof(cmd4_init)];
	/* Zeroed so that the strstr() below cannot walk past what we filled in. */
	uint8_t buf[1000] = { 0 };
	size_t i;
	int ret;

	memcpy(cmd3, cmd3_init, sizeof(cmd3));
	memcpy(cmd4, cmd4_init, sizeof(cmd4));

	smit_set(d, 0x82, cmd0, 1, 100);
	smit_tuner_stat(d, buf, 0);

	smit_set(d, 0xA0, cmd3, 10, 0);

	smit_set(d, 0xA0, cmd4, 9, 100);
	smit_tuner_stat(d, buf, 0);

	cmd4[7] = 0x12;
	smit_set(d, 0xA0, cmd4, 9, 100);
	smit_tuner_stat(d, buf, 0);

	smit_set(d, 0xA0, cmd5, 37, 100);
	smit_tuner_stat(d, buf, 0);

	cmd3[5] = 0x02;
	cmd3[9] = 0x02;
	smit_set(d, 0xA0, cmd3, 10, 0);

	cmd4[4] = 0x02;
	cmd4[7] = 0x20;
	smit_set(d, 0xA0, cmd4, 9, 100);
	smit_tuner_stat(d, buf, 0);
	smit_tuner_stat(d, buf, 0);

	smit_set(d, 0xA0, cmd6, 10, 1);

	smit_set(d, 0xA0, cmd7, 17, 100);
	smit_tuner_stat(d, buf, 100);
	smit_tuner_stat(d, buf, 0);

	smit_set(d, 0xA0, cmd8, 10, 300);
	smit_tuner_stat(d, buf, 0);

	smit_set(d, 0xA0, cmd14, 20, 100);
	smit_tuner_stat(d, buf, 0);

	/* The tuner status payload carries an ASCII identification block. */
	memset(d->model, 0, sizeof(d->model));
	for (i = 0; i + 1 < sizeof(d->model) && 18 + i < sizeof(buf); i++) {
		char c = (char)buf[18 + i];
		if (c == '\0')
			break;
		d->model[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}

	d->dtmb = strstr((char *)buf + 18, "DTMB:Y") != NULL;

	if (!d->dtmb) {
		smit_set(d, 0xA0, cmd10, 10, 0);

		smit_set(d, 0xA0, cmd11, 9, 0);
		smit_tuner_stat(d, buf, 0);
		smit_tuner_stat(d, buf, 0);

		smit_set(d, 0xA0, cmd12, 10, 100);
		smit_tuner_stat(d, buf, 0);
	}

	ret = smit_set(d, 0xA0, cmd9, 32, 100);
	smit_tuner_stat(d, buf, 20);

	return ret < 0 ? ret : 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFF);
	p[1] = (uint8_t)((v >> 8) & 0xFF);
	p[2] = (uint8_t)((v >> 16) & 0xFF);
	p[3] = (uint8_t)((v >> 24) & 0xFF);
}

int icast_set_frontend(struct icast_dev *d, uint32_t freq_hz, int dvbc,
		       uint32_t symbol_rate, int qam, int bandwidth)
{
	/* Tuning template, copied from icast_fe_set_frontend(). */
	uint8_t cmd9[32] = {
		0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
		0x17, 0x00, 0x00, 0x14, 0x00, 0x03, 0x00, 0x10,
		0xB8, 0xF3, 0x03, 0x00, 0xDB, 0x1A, 0x00, 0x00,
		0x40, 0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00
	};
	uint8_t buf[900];
	int r;

	put_le32(&cmd9[16], freq_hz / 1000);

	if (dvbc) {
		put_le32(&cmd9[20], symbol_rate / 1000);
		if (qam > 0)
			put_le32(&cmd9[24], (uint32_t)qam);
	} else {
		put_le32(&cmd9[20], 0);
		put_le32(&cmd9[24], 0);
		put_le32(&cmd9[28], (uint32_t)bandwidth);
	}

	r = smit_set(d, 0xA0, cmd9, 32, 100);
	if (r < 0)
		return r;

	smit_tuner_stat(d, buf, 2);

	d->frequency_hz = freq_hz;
	return 0;
}

int icast_read_status(struct icast_dev *d, struct icast_status *st)
{
	/* Status request, copied from icast_fe_read_status(). */
	static const uint8_t cmd12[20] = {
		0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
		0x0B, 0x01, 0x00, 0x08, 0x00, 0x05, 0x00, 0x04,
		0x47, 0x53, 0x54, 0x41
	};
	uint8_t buf[900];
	int r;

	memset(st, 0, sizeof(*st));
	r = smit_set(d, 0xA0, cmd12, 20, 50);
	if (r < 0)
		return r;

	r = smit_tuner_stat(d, buf, 5);
	if (r != SMIT_STATUS_LEN)
		return -ENODATA;

	st->lock = buf[29];
	if (!st->lock)
		return 0;

	st->frequency_hz = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
			   ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);
	st->snr_raw = buf[30];
	st->strength_raw = buf[31];

	/*
	 * Same fixed-point scaling the kernel driver uses: raw 0..255 maps to
	 * the 0..65535 range DVB expects.
	 */
	st->strength = (uint16_t)(((uint64_t)90070618176225ULL * st->strength_raw) >> 37);
	st->snr = (uint16_t)(((uint64_t)90070618176225ULL * st->snr_raw) >> 37);

	return 0;
}

int icast_read_ts(struct icast_dev *d, uint8_t *buf, int len, int timeout_ms)
{
	int transferred = 0;
	int r;

	r = libusb_bulk_transfer(d->h, SMIT_EP_STREAM, buf, len, &transferred,
				 timeout_ms);
	if (r < 0 && r != LIBUSB_ERROR_TIMEOUT)
		return r;

	return transferred;
}

static int find_stream_interface(libusb_device *dev)
{
	struct libusb_config_descriptor *cfg = NULL;
	int iface = -1;
	int i, j, k;

	if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
		return -1;

	for (i = 0; i < cfg->bNumInterfaces && iface < 0; i++) {
		for (j = 0; j < cfg->interface[i].num_altsetting; j++) {
			const struct libusb_interface_descriptor *alt =
				&cfg->interface[i].altsetting[j];

			for (k = 0; k < alt->bNumEndpoints; k++) {
				if (alt->endpoint[k].bEndpointAddress ==
				    SMIT_EP_STREAM) {
					iface = alt->bInterfaceNumber;
					break;
				}
			}
			if (iface >= 0)
				break;
		}
	}

	libusb_free_config_descriptor(cfg);
	return iface;
}

int icast_count(void)
{
	libusb_device **list = NULL;
	ssize_t n, i;
	int count = 0;

	if (libusb_init(NULL) != 0)
		return 0;

	n = libusb_get_device_list(NULL, &list);
	for (i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (desc.idVendor == ICAST_VID && desc.idProduct == ICAST_PID)
			count++;
	}

	libusb_free_device_list(list, 1);
	libusb_exit(NULL);
	return count;
}

int icast_open(struct icast_dev *d, int index)
{
	libusb_device **list = NULL;
	libusb_device *target = NULL;
	ssize_t n;
	int seen = 0;
	int r;

	memset(d, 0, sizeof(*d));
	d->iface = -1;

	r = libusb_init(&d->ctx);
	if (r < 0)
		return r;

	n = libusb_get_device_list(d->ctx, &list);
	if (n < 0) {
		libusb_exit(d->ctx);
		return (int)n;
	}

	for (ssize_t i = 0; i < n; i++) {
		struct libusb_device_descriptor desc;

		if (libusb_get_device_descriptor(list[i], &desc) != 0)
			continue;
		if (desc.idVendor != ICAST_VID || desc.idProduct != ICAST_PID)
			continue;
		if (seen++ == index) {
			target = list[i];
			break;
		}
	}

	if (!target) {
		libusb_free_device_list(list, 1);
		libusb_exit(d->ctx);
		return -ENODEV;
	}

	r = find_stream_interface(target);
	if (r < 0) {
		libusb_free_device_list(list, 1);
		libusb_exit(d->ctx);
		return -ENODEV;
	}
	d->iface = r;

	r = libusb_open(target, &d->h);
	libusb_free_device_list(list, 1);
	if (r < 0) {
		libusb_exit(d->ctx);
		return r;
	}

	/* Take the interface away from the kernel driver if it is bound. */
#if defined(__linux__)
#if defined(LIBUSB_API_VERSION) && LIBUSB_API_VERSION >= 0x01000103
	libusb_set_auto_detach_kernel_driver(d->h, 1);
#else
	libusb_detach_kernel_driver(d->h, d->iface);
#endif
#endif

	r = libusb_claim_interface(d->h, d->iface);
	if (r < 0) {
		libusb_close(d->h);
		libusb_exit(d->ctx);
		return r;
	}
	d->claimed = 1;

	return 0;
}

void icast_close(struct icast_dev *d)
{
	if (d->h) {
		if (d->claimed)
			libusb_release_interface(d->h, d->iface);
		libusb_close(d->h);
		d->h = NULL;
	}
	if (d->ctx) {
		libusb_exit(d->ctx);
		d->ctx = NULL;
	}
}

/*
 * Probe handshake, copied from smit_probe(): 0xFE 0x00 0x10 0x00 out, then a
 * 4 byte reply, then the full init sequence.
 */
int icast_init(struct icast_dev *d)
{
	uint8_t hs[4] = { 0xFE, 0x00, 0x10, 0x00 };
	int actual = 0;
	int r;

	r = smit_bulk(d, SMIT_EP_CMD_SEND, hs, 4, &actual, SMIT_CMD_TIMEOUT);
	if (r < 0)
		return r;

	r = smit_bulk(d, SMIT_EP_CMD_RECV, hs, 4, &actual, SMIT_CMD_TIMEOUT);
	if (r < 0)
		return r;

	r = smit_init(d);
	if (r < 0) {
		smit_control_reset(d);
		return r;
	}

	return 0;
}
