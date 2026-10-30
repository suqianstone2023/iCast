/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Build-time sanity check for the vendored DVB-USB private header.
 *
 * kapi/dvb-usb.h is a verbatim copy of
 *   drivers/media/usb/dvb-usb/dvb-usb.h  @ v6.16
 *
 * Its struct layouts are identical from v5.15 through v6.16 - the only
 * change in that range is const-qualification of cold_ids/warm_ids/i2c_algo
 * in v6.13, which does not move any member.  So one copy of the header
 * serves the whole range, and it does not have to be swapped on every
 * kernel upgrade.
 *
 * Outside that range the layout is not verified.  A layout mismatch does not
 * fail to build - it silently corrupts memory - so warn loudly instead.
 */

#ifndef ICAST_KAPI_GUARD_H
#define ICAST_KAPI_GUARD_H

#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 15, 0)
#warning "kapi/ headers are verified from v5.15 on; re-check struct dvb_usb_adapter/dvb_usb_device layout for this kernel"
#elif LINUX_VERSION_CODE > KERNEL_VERSION(6, 16, 99)
#warning "kernel is newer than v6.16; re-check struct layout in kapi/dvb-usb.h before trusting this build"
#endif

#endif /* ICAST_KAPI_GUARD_H */
