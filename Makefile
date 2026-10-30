# Native (in-tree framework) build for a stock Linux kernel.
#
# The OpenWrt cross-compile flow that ships whole dvb-core sources lives in
# Makefile.openwrt.  That flow rebuilds dvb-core / dvb-usb / dvb-usb-v2 as
# out-of-tree modules, which collides with the dvb_core / dvb_usb modules a
# normal distribution kernel already provides.
#
# Here we only build the two iCast modules and link them against the DVB-USB
# framework of the running kernel (dvb_usb_device_init / dvb_usb_device_exit
# are EXPORT_SYMBOL'd).  kapi/ carries verbatim copies of the two private
# headers that linux-headers-$(uname -r) does not ship.

obj-m += dvb-usb-icast-fe.o
obj-m += dvb-usb-icast-smit.o

dvb-usb-icast-fe-objs   := smit.o icast-fe.o
dvb-usb-icast-smit-objs := smit.o icast-smit.o

ccflags-y += -I$(src)/kapi
# Warn when the kernel is outside the range the vendored header was verified for.
ccflags-y += -include $(src)/kapi/kapi-guard.h

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.ko *.o *.mod *.mod.c modules.order Module.symvers

# Install under /lib/modules/$(uname -r)/extra/icast and refresh the module
# dependency table.  Prefer install.sh, which does this through DKMS.
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
