// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SMIT iCast DTMB/DVBC driver
 *
 * Copyright (C) 2026 Xiaodong Ni <nxiaodong520@gmail.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/delay.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
#include <dvb_frontend.h>
#else
#include <media/dvb_frontend.h>
#endif

#include "dvb-usb.h"
#include "smit.h"
#include "icast-fe.h"

//#define CONFIG_DYNAMIC_DEBUG

/*
 * Runtime diagnostics.  The frontend side had no logging at all, which made
 * "Enigma2 tuned but never saw a lock" impossible to tell apart from
 * "the tune never reached the dongle".  dbg=1 turns the trace on at runtime:
 *
 *   echo 1 > /sys/module/dvb_usb_icast_fe/parameters/dbg
 */
static int dbg;
module_param(dbg, int, 0644);
MODULE_PARM_DESC(dbg, "log frontend ioctl parameters and results (0/1)");

#define DLOG(fmt, ...) \
	do { if (dbg) printk(KERN_INFO "icast-fe: " fmt, ##__VA_ARGS__); } while (0)

struct icast_state {
	struct dvb_usb_device *dev;
  struct dvb_frontend frontend;
  u32 frequency;
  u32 symbol_rate;
  enum fe_modulation	modulation;
  u8 lock;
  u8 strength;
  u16 snr;
  u32 dbg_calls;
  u8  dbg_last_lock;
  int dbg_stat_len;
  /*
   * The kernel's frontend thread calls read_status() on its own schedule,
   * while Enigma2 calls FE_READ_STATUS / FE_SET_FRONTEND from the ioctl
   * path.  Both walk the same pair of bulk endpoints, and the dongle has a
   * single command channel: without this lock the request and the reply of
   * two concurrent sequences interleave and the status block comes back
   * truncated, which reads as "no lock" even though the tuner is locked.
   */
  struct mutex io_lock;
};

/*
 * Un-stall the transport-stream endpoint before anyone submits an URB to it.
 *
 * dvb-usb stops the stream with usb_urb_kill() and never clears a halt it
 * leaves behind; the iCast firmware keeps pushing data until the host drains
 * the pipe, so killing the URBs stalls the bulk IN endpoint rather than
 * stopping the device.  The next dvb_usb_ctrl_feed() then calls
 * usb_urb_submit() -- and ignores its return value -- so it counts a feed
 * that is not actually running.  The tuner still reports FE_HAS_LOCK, because
 * that comes from the control endpoint, but not a single transport packet
 * arrives: from Enigma2's side the scan says "0 channels found" on a
 * transponder that is demonstrably live.
 *
 * This matters most right after the blind-scan helper has run: it opens the
 * adapter, tunes across the band and closes again, so the endpoint is left
 * stalled exactly when Enigma2 reopens it to scan the frequencies the helper
 * just reported.
 *
 * -EPIPE / -ENOENT here just mean the endpoint was not halted, which is the
 * normal case on a receiver that has only been playing.
 */
static void icast_clear_stream_halt(struct icast_state *priv, const char *where)
{
	int ret;

	ret = usb_clear_halt(priv->dev->udev,
			     usb_rcvbulkpipe(priv->dev->udev,
					     SMIT_BULK_ENDPOINT_STREAM));
	DLOG("%s: clear halt on stream endpoint -> %d\n", where, ret);
}

static int icast_fe_init(struct dvb_frontend *fe)
{
	struct icast_state *priv = fe->demodulator_priv;

	if (!priv->dev)
		return -ENODEV;

	/*
	 * Drop the cached signal state so the first read_status() after a
	 * tune does not report the strength of the previous transponder.
	 * The tuned frequency is kept: E2 reads it back while building its
	 * transponder list, and zeroing it there only makes it guess.
	 */
	priv->lock = 0;
	priv->strength = 0;
	priv->snr = 0;

	icast_clear_stream_halt(priv, "init");

	return 0;
}

static void icast_fe_release(struct dvb_frontend *fe)
{
	struct icast_state *state = fe->demodulator_priv;

	kfree(state);
}

/*
 * get_frontend changed signature in v4.7: up to v4.6 it took only the
 * frontend and was expected to fill fe->dtv_property_cache; from v4.7 on it
 * is handed the properties blob to fill.
 *
 * Getting this wrong is nearly silent at build time -- a single
 * "incompatible pointer type" warning -- but at run time the kernel calls a
 * two-argument function through a one-argument pointer, so the callback
 * dereferences whatever happened to be in the second register.  On 4.4.35
 * that is exactly the kind of thing that turns a frontend query into an
 * oops inside the caller's context.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
static int icast_fe_get_frontend(struct dvb_frontend *fe)
{
  struct icast_state *priv = fe->demodulator_priv;

  fe->dtv_property_cache.frequency = priv->frequency;
  fe->dtv_property_cache.symbol_rate = priv->symbol_rate;
  fe->dtv_property_cache.modulation = priv->modulation;

  return 0;
}
#else
static int icast_fe_get_frontend(struct dvb_frontend *fe, struct dtv_frontend_properties *props)
{
  struct icast_state *priv = fe->demodulator_priv;

  props->frequency = priv->frequency;
  props->symbol_rate = priv->symbol_rate;
  props->modulation = priv->modulation;

  return 0;
}
#endif

#define DWORD_TO_BYTES(dword, bytes, position) \
  bytes[position] = (u8)(dword & 0xFF); \
  bytes[position+1] = (u8)((dword>>8) & 0xFF); \
  bytes[position+2] = (u8)((dword>>16) & 0xFF); \
  bytes[position+3] = (u8)((dword>>24) & 0xFF);

/*
 * The tuner command block.  Offsets that matter:
 *   16..19  frequency in kHz, little endian
 *   20..23  symbol rate in kS/s, little endian
 *   24..27  QAM order (16/32/64/128/256) for DVB-C
 *   28..31  bandwidth, DTMB only
 */
/*
 * Throw away whatever transport packets are still sitting in the dongle's
 * bulk-in FIFO after a retune.
 *
 * The firmware never flushes that FIFO when the host stops the stream
 * (usb_urb_kill), and it keeps outputting the previous mux into it until
 * the host drains the pipe.  A fresh tune therefore starts with a tail of
 * the OLD frequency, and the first PAT a reader sees belongs to the
 * previous transponder.  The blind-scan helper drains explicitly before it
 * judges a frequency, but Enigma2 starts its section filters right after
 * the tune and trusts the first PAT -- so its scan records the previous
 * transponder's services under the new frequency.  Draining here (only
 * while dvb-usb is not streaming, since then the URBs own the endpoint)
 * makes the first PAT after a tune a genuine one.
 */
static void icast_drain_stale_ts(struct icast_state *priv)
{
	u8 *buf;
	int total = 0, iter;

	if (priv->dev->adapter[0].feedcount > 0)
		return;

	buf = kmalloc(4096, GFP_KERNEL);
	if (!buf)
		return;

	for (iter = 0; iter < 128 && total < 512 * 1024; iter++) {
		int len = 4096;
		int ret = usb_bulk_msg(priv->dev->udev,
				       usb_rcvbulkpipe(priv->dev->udev,
						       SMIT_BULK_ENDPOINT_STREAM),
				       buf, len, &len, 20);

		if (ret < 0 || len == 0)
			break;
		total += len;
	}
	if (total)
		DLOG("drained %d bytes of stale TS after tune\n", total);
	kfree(buf);
}

static int icast_fe_set_frontend(struct dvb_frontend *fe)
{
  u8 cmd9[32] = {
  	0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
  	0x17,	0x00, 0x00, 0x14, 0x00, 0x03, 0x00, 0x10,
  	0xB8,	0xF3, 0x03, 0x00, 0xDB, 0x1A, 0x00, 0x00,
  	0x40,	0x00, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00
  };
  u8 buf[300];
  u32 frequency, symbol_rate;
  struct icast_state *priv = fe->demodulator_priv;
  int ret;

  if (!priv->dev)
    return -ENODEV;

  DLOG("set_frontend: ds=%d freq=%u sr=%u mod=%d\n",
       (int)fe->dtv_property_cache.delivery_system,
       (unsigned)fe->dtv_property_cache.frequency,
       (unsigned)fe->dtv_property_cache.symbol_rate,
       (int)fe->dtv_property_cache.modulation);

  /*
   * Re-arm the transport-stream endpoint here, not only in init().
   *
   * Enigma2 keeps the frontend open for its whole lifetime, so init() runs
   * once at boot and never again -- a stall picked up later stays until the
   * next reboot.  set_frontend() is the one hook that runs for every
   * transponder, and it runs before Enigma2 starts its section filters, so
   * un-stalling here is what makes the feed below actually deliver packets.
   * Without it the first transponder after the blind-scan helper always
   * scans empty; the following ones only work because closing and reopening
   * the demux happens to clear the condition as a side effect.
   */
  icast_clear_stream_halt(priv, "set_frontend");

  mutex_lock(&priv->io_lock);

	frequency = (u32)(fe->dtv_property_cache.frequency / 1000);
  DWORD_TO_BYTES(frequency, cmd9, 16);

  if ( fe->dtv_property_cache.delivery_system == SYS_DVBC_ANNEX_A)
  {
		symbol_rate = (u32)(fe->dtv_property_cache.symbol_rate / 1000);
    DWORD_TO_BYTES(symbol_rate, cmd9, 20);

    switch ( fe->dtv_property_cache.modulation )
    {
      case QAM_16:
      	DWORD_TO_BYTES(16, cmd9, 24);
        break;
      case QAM_32:
      	DWORD_TO_BYTES(32, cmd9, 24);
        break;
      case QAM_64:
      	DWORD_TO_BYTES(64, cmd9, 24);
        break;
      case QAM_128:
      	DWORD_TO_BYTES(128, cmd9, 24);
        break;
      case QAM_256:
      	DWORD_TO_BYTES(256, cmd9, 24);
        break;
      default:
      	/*
      	 * QAM_AUTO and friends: keep the value the init sequence left
      	 * in the block (64) instead of writing zero, which the firmware
      	 * reads as "no demodulation configured".
      	 */
        break;
    }
  }
  else if ( fe->dtv_property_cache.delivery_system == SYS_DVBT )
  {
  	DWORD_TO_BYTES(0, cmd9, 20);
    DWORD_TO_BYTES(0, cmd9, 24);
    DWORD_TO_BYTES(8, cmd9, 28);
  }
  else
  {
    DLOG("set_frontend: unsupported delivery system %d\n",
         (int)fe->dtv_property_cache.delivery_system);
    mutex_unlock(&priv->io_lock);
    return -EINVAL;
  }

  memset(buf, 0, sizeof(buf));

  ret = smit_set(priv->dev->udev, 0xA0u, cmd9, 32, 100);
  if (ret) {
    DLOG("set_frontend: smit_set failed ret=%d\n", ret);
    mutex_unlock(&priv->io_lock);
    return ret;
  }

  /*
   * One status read right after the tune: it drains whatever the firmware
   * still had queued for the previous transponder, and it makes the lock
   * flag meaningful for callers that poll status before the first timeout.
   * Start from "not locked": the previous transponder's state must not leak
   * into the new one, and a short reply here means we simply don't know yet.
   */
  priv->lock = 0;
  if (smit_tuner_stat(priv->dev->udev, buf, 2) >= 32)
    priv->lock = buf[29] ? 1 : 0;

  /*
   * The status read above gave the firmware a moment to settle; whatever
   * the bulk-in FIFO still holds now belongs to the previous transponder.
   */
  icast_drain_stale_ts(priv);

  DLOG("set_frontend: accepted, freq_khz=%u sr_ksps=%u post_tune_lock=%d\n",
       (unsigned)(frequency), (unsigned)(fe->dtv_property_cache.symbol_rate / 1000),
       (int)priv->lock);

  priv->frequency = fe->dtv_property_cache.frequency;
  priv->symbol_rate = fe->dtv_property_cache.symbol_rate;
  priv->modulation = fe->dtv_property_cache.modulation;

  mutex_unlock(&priv->io_lock);

  return 0;
}

static int icast_fe_get_tune_settings(struct dvb_frontend *fe,
			      struct dvb_frontend_tune_settings *fesettings)
{
	fesettings->min_delay_ms = 800;
	fesettings->step_size = 0;
	fesettings->max_drift = 0;
	return 0;
}

static int icast_fe_read_status_raw(struct dvb_frontend *fe, enum fe_status *status)
{
  struct icast_state *priv = fe->demodulator_priv;
  u8 cmd12[20] = {
  	0x01, 0x90, 0x02, 0x00, 0x03, 0x9F, 0x9A, 0x07,
  	0x0B, 0x01, 0x00, 0x08, 0x00, 0x05, 0x00, 0x04,
  	0x47, 0x53, 0x54, 0x41
  };
  u8 buf[300];
  int ret, i;

  *status = 0;

  if (!priv->dev)
    return -ENODEV;

  mutex_lock(&priv->io_lock);

  memset(buf, 0, sizeof(buf));

  ret = smit_set(priv->dev->udev, 0xA0u, cmd12, 20, 50);
  if (ret) {
    priv->dbg_stat_len = -1000 + ret;
    mutex_unlock(&priv->io_lock);
    return ret;
  }

  ret = smit_tuner_stat(priv->dev->udev, buf, 5);
  if (ret < 0) {
    priv->dbg_stat_len = -2000 + ret;
    mutex_unlock(&priv->io_lock);
    return ret;
  }

  /*
   * A short answer is the firmware saying "no new status yet", not "unlocked".
   * It shows up right after a tune and while the TS pump is starting, and
   * treating it as "lost lock" makes Enigma2 abort the scan: it retunes, the
   * retune restarts the settling window, and every attempt dies the same way.
   * Give the MCU a couple of chances, then keep the last known state -- the
   * dongle reports a real unlock as a full 32 byte block with bit 29 clear.
   */
  for (i = 0; ret < 32 && i < 3; i++) {
    msleep(10);
    ret = smit_tuner_stat(priv->dev->udev, buf, 5);
    if (ret < 0) {
      priv->dbg_stat_len = -2000 + ret;
      mutex_unlock(&priv->io_lock);
      return ret;
    }
  }

  priv->dbg_stat_len = ret;

  if (ret < 32) {
    mutex_unlock(&priv->io_lock);
    return 0;
  }

  priv->lock = buf[29] ? 1 : 0;

  if (!priv->lock) {
    mutex_unlock(&priv->io_lock);
    return 0;
  }

  /*
   * The dongle reports a single lock bit.  Everything below it in the
   * status hierarchy is implied once it is set -- without carrier and
   * synchroniser there would be no lock at all -- and E2's tuner screens
   * use the whole set to decide whether an SNR/strength reading is worth
   * showing.
   */
  *status = FE_HAS_SIGNAL | FE_HAS_CARRIER | FE_HAS_VITERBI |
            FE_HAS_SYNC | FE_HAS_LOCK;

  /* buf[16..19] is the locked frequency in kHz; the cache is in Hz. */
  priv->frequency = ((u32)buf[19] << 24 | (u32)buf[18] << 16 |
                     (u32)buf[17] << 8 | (u32)buf[16]) * 1000;
  priv->snr = buf[30];
  priv->strength = buf[31];

  mutex_unlock(&priv->io_lock);

  return 0;
}

/*
 * Logging wrapper.  Enigma2 polls status once per tune cycle and the kernel's
 * frontend thread polls it too, so an unconditional printk would flood the
 * log; every eighth call plus every lock transition is enough to follow what
 * the tuner is doing.
 */
static int icast_fe_read_status(struct dvb_frontend *fe, enum fe_status *status)
{
  struct icast_state *priv = fe->demodulator_priv;
  int ret = icast_fe_read_status_raw(fe, status);

  if (dbg) {
    priv->dbg_calls++;
    if (ret < 0 || priv->lock != priv->dbg_last_lock ||
        !(priv->dbg_calls & 7)) {
      printk(KERN_INFO "icast-fe: read_status #%u ret=%d lock=%d status=0x%x stat_len=%d\n",
             (unsigned)priv->dbg_calls, ret, (int)priv->lock,
             (unsigned)*status, priv->dbg_stat_len);
      priv->dbg_last_lock = priv->lock;
    }
  }

  return ret;
}

/*
 * The firmware hands out a 0..100 percentage; the DVB API wants 0..0xffff.
 * The original expression open-coded the same scaling as a magic multiply.
 */
static u16 icast_scale_percent(u8 raw)
{
  return (u16)(((u32)raw * 0xffffu) / 100u);
}

static int icast_fe_read_signal_strength(struct dvb_frontend *fe, u16 *strength)
{
  struct icast_state *priv = fe->demodulator_priv;

  *strength = priv->lock ? icast_scale_percent(priv->strength) : 0;
  return 0;
}

static int icast_fe_read_snr(struct dvb_frontend *fe, u16 *snr)
{
  struct icast_state *priv = fe->demodulator_priv;

  *snr = priv->lock ? icast_scale_percent(priv->snr) : 0;
  return 0;
}

/*
 * The dongle does not expose pre/post RS error counts over this protocol.
 * Providing the callbacks anyway keeps the frontend from answering -ENOSYS,
 * which some E2 signal screens treat as "frontend gone".
 */
static int icast_fe_read_ber(struct dvb_frontend *fe, u32 *ber)
{
  *ber = 0;
  return 0;
}

static int icast_fe_read_ucblocks(struct dvb_frontend *fe, u32 *ucblocks)
{
  *ucblocks = 0;
  return 0;
}

/*
 * .info.type is not filled in by dvb-core: FE_GET_INFO returns whatever the
 * driver left there, and the enum starts at 0, so a driver that never sets
 * it reports FE_QPSK.
 *
 * That is not cosmetic.  E2's eDVBUsbAdapter switches on fe_info.type to
 * label the tuner (FE_QAM -> "DVB-C", FE_QPSK -> "DVB-S2") and bails out on
 * anything it does not know, so an unset type makes a cable stick show up
 * as a satellite tuner the user cannot use.
 */
static const struct dvb_frontend_ops icast_dual_mode_fe_ops = {
	.delsys = { SYS_DVBT, SYS_DVBC_ANNEX_A },
	.info = {
		.name = "iCast DTMB/DVBC USB demodulator",
		.type = FE_OFDM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
		.frequency_min = 47000000,
		.frequency_max = 862000000,
		.frequency_stepsize = 10000,
		.symbol_rate_min = 1000000,
		.symbol_rate_max = 10000000,
#else
		.frequency_min_hz	= 47 * MHz,
		.frequency_max_hz	= 862 * MHz,
		.frequency_stepsize_hz	= 10 * kHz,
		.symbol_rate_min = 1000000,
		.symbol_rate_max = 10000000,
#endif
		.caps =
			FE_CAN_FEC_AUTO |
			FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO
	},

	.release = icast_fe_release,
	.init = icast_fe_init,
	.set_frontend = icast_fe_set_frontend,
	.get_tune_settings = icast_fe_get_tune_settings,
	.get_frontend = icast_fe_get_frontend,
	.read_status = icast_fe_read_status,
	.read_signal_strength = icast_fe_read_signal_strength,
	.read_snr = icast_fe_read_snr,
	.read_ber = icast_fe_read_ber,
	/*
	 * read_ucblocks was renamed read_uncorrected_blocks in v4.11.  The
	 * callback signature is identical, only the member name moved.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	.read_ucblocks = icast_fe_read_ucblocks,
#else
	.read_uncorrected_blocks = icast_fe_read_ucblocks,
#endif
};

static const struct dvb_frontend_ops icast_single_mode_fe_ops = {
	.delsys = { SYS_DVBC_ANNEX_A },
	.info = {
		.name = "iCast DVBC USB demodulator",
		.type = FE_QAM,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
		.frequency_min = 47000000,
		.frequency_max = 862000000,
		.frequency_stepsize = 10000,
		.symbol_rate_min = 1000000,
		.symbol_rate_max = 10000000,
#else
		.frequency_min_hz	= 47 * MHz,
		.frequency_max_hz	= 862 * MHz,
		.frequency_stepsize_hz	= 10 * kHz,
		.symbol_rate_min = 1000000,
		.symbol_rate_max = 10000000,
#endif
		.caps =
			FE_CAN_FEC_AUTO |
			FE_CAN_QAM_AUTO |
			FE_CAN_TRANSMISSION_MODE_AUTO |
			FE_CAN_GUARD_INTERVAL_AUTO
	},

	.release = icast_fe_release,
	.init = icast_fe_init,
	.set_frontend = icast_fe_set_frontend,
	.get_tune_settings = icast_fe_get_tune_settings,
	.get_frontend = icast_fe_get_frontend,
	.read_status = icast_fe_read_status,
	.read_signal_strength = icast_fe_read_signal_strength,
	.read_snr = icast_fe_read_snr,
	.read_ber = icast_fe_read_ber,
	/*
	 * read_ucblocks was renamed read_uncorrected_blocks in v4.11.  The
	 * callback signature is identical, only the member name moved.
	 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
	.read_ucblocks = icast_fe_read_ucblocks,
#else
	.read_uncorrected_blocks = icast_fe_read_ucblocks,
#endif
};

struct dvb_frontend *icast_fe_attach(struct dvb_usb_adapter *adap, const struct icast_fe_config *config)
{
	struct icast_state *priv = NULL;

	priv = kzalloc(sizeof(struct icast_state), GFP_KERNEL);
	if (priv == NULL)
		return NULL;

  if (config->dtmb)
  {
  	memcpy(&priv->frontend.ops, &icast_dual_mode_fe_ops, sizeof(struct dvb_frontend_ops));
  }
  else
  {
  	memcpy(&priv->frontend.ops, &icast_single_mode_fe_ops, sizeof(struct dvb_frontend_ops));
  }

	priv->frontend.demodulator_priv = priv;
	priv->dev = adap->dev;
	mutex_init(&priv->io_lock);

	return &priv->frontend;
}

EXPORT_SYMBOL_GPL(icast_fe_attach);

MODULE_DESCRIPTION("iCast frontend driver");
MODULE_AUTHOR("Xiaodong Ni <nxiaodong520@gmail.com>");
MODULE_LICENSE("GPL");
