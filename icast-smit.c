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
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/string.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
#include <dvb_frontend.h>
#else
#include <media/dvb_frontend.h>
#endif

#include "dvb-usb.h"
#include "icast-fe.h"
#include "smit.h"

//#define CONFIG_DYNAMIC_DEBUG

#define USB_VID_SMIT							0x29DF
#define USB_PID_SMIT_ICAST				0x0001

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

static bool smit_async = true;
module_param(smit_async, bool, 0644);
MODULE_PARM_DESC(smit_async,
	"initialise the dongle from a workqueue instead of probe context (default: true)");

/*
 * A dual-mode stick answers "DTMB:Y" in its capability string, and the
 * frontend then advertises FE_OFDM.  Enigma2 switches on fe_info.type to
 * label a USB tuner (FE_QAM -> "DVB-C", FE_OFDM -> "DVB-T"), and a cable
 * operator's bouquet cannot be attached to a DVB-T tuner, so on a stick
 * that is only ever used on cable the label has to be FE_QAM.
 *
 * This only overrides what FE_GET_INFO reports.  smit_init() still runs
 * with the mode the hardware reports, because that flag decides whether
 * the vendor init sequence sends cmd10/cmd11 (the single-mode cable
 * switch); flipping it would change how the dongle behaves rather than
 * just how it is labelled.  set_frontend() already dispatches on the
 * delivery system, so cable tuning works either way.
 */
static char *icast_mode = "dvbc";
module_param(icast_mode, charp, 0444);
MODULE_PARM_DESC(icast_mode,
	"frontend type: dvbc (FE_QAM, default -- this stick is used on cable), auto (as reported by the dongle), dtmb (FE_OFDM)");

/*
 * The MCU answers the initialisation handshake exactly once per power cycle
 * and then parks itself in the busy state (01 00 80 02 01 00).  A second
 * smit_init() therefore does not merely fail: it makes the dongle unusable
 * until it is unplugged.  Everything below exists so that exactly one
 * smit_init() can run per device instance.
 */
struct smit_dev {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct work_struct init_work;
	bool registered;
};

static DEFINE_MUTEX(smit_lock);

/*
 * The dongle is a single-port device, so there is at most one instance.  The
 * pointer is deliberately not kept in intfdata: dvb_usb_device_init() stores
 * its own struct dvb_usb_device there, and dvb_usb_device_exit() would
 * happily dereference ours as if it were one.
 */
static struct smit_dev *smit_dev;

static struct icast_fe_config icast_config = {
	.dtmb = 0,
};

static int icast_frontend_attach(struct dvb_usb_adapter *adap)
{
	if ((adap->fe_adap[0].fe = dvb_attach(icast_fe_attach, adap, &icast_config)) != NULL) {
		return 0;
	}
	info("not attached iCast usb device");
	return -ENODEV;
}

static struct usb_device_id icast_id_table[] = {
	{USB_DEVICE(USB_VID_SMIT, USB_PID_SMIT_ICAST)},
	{}
};

MODULE_DEVICE_TABLE(usb, icast_id_table);

static struct dvb_usb_device_properties icast_properties = {
	.num_adapters = 1,

	.adapter = {
		{
		.num_frontends = 1,
		.fe = {{
			/*
			 * At high speed this flag only advertises that the
			 * firmware could filter; dvb-usb keeps pid_filtering
			 * off unless the link is full speed, so the complete
			 * transport stream is handed to the software demux.
			 * That is what we want: the firmware sends the whole
			 * mux regardless, and E2 does the PID selection.
			 */
			.caps = DVB_USB_ADAP_HAS_PID_FILTER,
			.frontend_attach  = icast_frontend_attach,
			.stream = {
				.type = USB_BULK,
				.count = 8,
				.endpoint = SMIT_BULK_ENDPOINT_STREAM,
				.u = {
					.bulk = {
						.buffersize = 8192,
					}
				}
			},
		}},
		},
	},
	.num_device_descs = 1,
	.devices = {
		{
			.name = "iCast DVB USB DONGLE adapter",
		  .warm_ids = {&icast_id_table[0], NULL},
		},
	}
};

/*
 * The id table only matches on VID/PID, and such a match is not per endpoint
 * set: usbcore calls probe once for every interface of the dongle.  Only the
 * interface that carries the SMIT command endpoints can talk to the MCU;
 * probing the others would run smit_init() a second time and burn the one
 * handshake the firmware accepts.
 *
 * Note the comparison is against bEndpointAddress, not usb_endpoint_num():
 * the latter masks off the direction bit and only ever returns 0..15, so
 * comparing it to 0x82 would silently never match.
 */
static bool smit_has_endpoints(struct usb_interface *intf)
{
	int a, i;

	for (a = 0; a < intf->num_altsetting; a++) {
		struct usb_host_interface *alt = &intf->altsetting[a];
		bool cmd_out = false, cmd_in = false;

		for (i = 0; i < alt->desc.bNumEndpoints; i++) {
			struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;

			if (usb_endpoint_is_bulk_out(ep) &&
			    ep->bEndpointAddress == SMIT_BULK_ENDPOINT_CMD_SEND)
				cmd_out = true;
			if (usb_endpoint_is_bulk_in(ep) &&
			    ep->bEndpointAddress == SMIT_BULK_ENDPOINT_CMD_RECEIVE)
				cmd_in = true;
		}

		if (cmd_out && cmd_in)
			return true;
	}

	return false;
}

/*
 * The vendor "hello" exchange: 0xFE 0x00 0x10 0x00 out, four bytes back in.
 * It is the cheapest way to find out whether the MCU is answering at all
 * before spending the one-shot initialisation on it.
 */
static int smit_hello(struct usb_device *udev)
{
	u8 *buf;
	int size = 0;
	int ret;

	buf = kmalloc(4, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = 0xFE;
	buf[1] = 0x00;
	buf[2] = 0x10;
	buf[3] = 0x00;

	ret = usb_bulk_msg(udev, usb_sndbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_SEND),
			   buf, 4, &size, 500);
	if (!ret)
		ret = usb_bulk_msg(udev, usb_rcvbulkpipe(udev, SMIT_BULK_ENDPOINT_CMD_RECEIVE),
				   buf, 4, &size, 500);

	kfree(buf);
	return ret;
}

/*
 * Bring the dongle up and register it with dvb-usb.  Returns true when the
 * adapter is live.
 *
 * By default this runs from a workqueue.  smit_init() issues well over a
 * hundred bulk round trips and, when the MCU is slow, its completion polls
 * sit in msleep() for seconds.  Doing that inside ->probe blocks khubd, the
 * single thread that also drives enumeration for every other USB device --
 * the kind of stall that turns into a use-after-free in the descriptor paths
 * of old vendor kernels.
 */
static bool smit_do_init(struct smit_dev *sd)
{
	struct usb_device *udev = sd->udev;
	u8 hw_dtmb = 0;
	int ret;

	mutex_lock(&smit_lock);

	ret = smit_hello(udev);
	if (ret) {
		info("no answer to the hello exchange (%d)", ret);
		goto fail;
	}

	ret = smit_init(udev, &hw_dtmb);
	if (ret) {
		info("smit_init failed (%d)", ret);
		goto fail;
	}

	/*
	 * What the hardware can do, versus what the frontend claims to be.
	 * The hardware flag is what smit_init() just acted on; the frontend
	 * flag is what E2 reads back from FE_GET_INFO.
	 */
	icast_config.dtmb = hw_dtmb;
	if (!strcmp(icast_mode, "dvbc"))
		icast_config.dtmb = 0;
	else if (!strcmp(icast_mode, "dtmb"))
		icast_config.dtmb = 1;

	info("dongle initialised, hardware=%s frontend=%s (icast_mode=%s)",
	     hw_dtmb ? "DTMB" : "DVB-C",
	     icast_config.dtmb ? "DTMB/FE_OFDM" : "DVB-C/FE_QAM",
	     icast_mode);

	ret = dvb_usb_device_init(sd->intf, &icast_properties, THIS_MODULE,
				  NULL, adapter_nr);
	if (ret) {
		info("dvb_usb_device_init failed (%d)", ret);
		goto fail;
	}

	sd->registered = true;
	mutex_unlock(&smit_lock);
	return true;

fail:
	/*
	 * Leave the MCU in a defined state and release whatever dvb-usb
	 * managed to set up before giving up, so a rebind starts clean
	 * instead of finding a half-owned interface.
	 */
	smit_reset(udev);
	dvb_usb_device_exit(sd->intf);
	mutex_unlock(&smit_lock);
	return false;
}

static void smit_init_worker(struct work_struct *work)
{
	struct smit_dev *sd = container_of(work, struct smit_dev, init_work);

	if (!smit_do_init(sd))
		info("dongle left bound but not registered; unbind/rebind or replug to retry");
}

static int smit_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct smit_dev *sd;

	if (!smit_has_endpoints(intf)) {
		info("interface %d has no SMIT command endpoints, not ours",
		     intf->cur_altsetting->desc.bInterfaceNumber);
		return -ENODEV;
	}

	mutex_lock(&smit_lock);
	if (smit_dev) {
		mutex_unlock(&smit_lock);
		info("already claimed by another interface, ignoring this one");
		return -ENODEV;
	}

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd) {
		mutex_unlock(&smit_lock);
		return -ENOMEM;
	}

	/*
	 * Hold our own references: the worker may still be running when the
	 * interface is unbound, and it must not touch freed structures.
	 */
	sd->udev = usb_get_dev(udev);
	sd->intf = usb_get_intf(intf);
	smit_dev = sd;
	mutex_unlock(&smit_lock);

	INIT_WORK(&sd->init_work, smit_init_worker);

	if (smit_async) {
		schedule_work(&sd->init_work);
		return 0;
	}

	/* Synchronous fallback for bring-up: same code path, probe context. */
	if (smit_do_init(sd))
		return 0;

	mutex_lock(&smit_lock);
	smit_dev = NULL;
	mutex_unlock(&smit_lock);
	usb_put_intf(sd->intf);
	usb_put_dev(sd->udev);
	kfree(sd);
	return -ENODEV;
}

static void smit_disconnect(struct usb_interface *intf)
{
	struct smit_dev *sd;

	mutex_lock(&smit_lock);
	sd = smit_dev;
	if (sd && sd->intf == intf)
		smit_dev = NULL;
	else
		sd = NULL;
	mutex_unlock(&smit_lock);

	/*
	 * Wait for a possibly running initialisation before tearing anything
	 * down, so the worker and this path never race on dvb-usb state.
	 * Nothing else frees sd, so holding it across cancel_work_sync() is
	 * safe.
	 */
	if (sd)
		cancel_work_sync(&sd->init_work);

	dvb_usb_device_exit(intf);

	if (sd) {
		usb_put_intf(sd->intf);
		usb_put_dev(sd->udev);
		kfree(sd);
	}
}

static struct usb_driver smit_driver = {
	.name = KBUILD_MODNAME,
	.probe = smit_probe,
	.disconnect = smit_disconnect,
	.id_table = icast_id_table,
};

static int __init smit_module_init(void)
{
	int result = 0;
	if ((result = usb_register(&smit_driver))) {
		err("usb_register failed. Error number %d", result);
	}
	return result;
}

static void __exit smit_module_exit(void)
{
	usb_deregister(&smit_driver);
}

module_init(smit_module_init);
module_exit(smit_module_exit);

MODULE_DESCRIPTION("SMIT iCast dvb driver");
MODULE_AUTHOR("Xiaodong Ni <nxiaodong520@gmail.com>");
MODULE_LICENSE("GPL");
