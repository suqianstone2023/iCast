# icast-libusb - userspace driver for the SMIT iCast DTMB/DVBC dongle
#
# Needs only libusb-1.0 and a C99 compiler. No kernel headers, no kernel
# module, no dependency on the running kernel.

CC      ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS  ?= -O2 -g -Wall -Wextra -std=c99 -D_POSIX_C_SOURCE=200809L
CFLAGS  += -DPACKAGE_VERSION=\"1.0\"

# ?= so an explicit LIBUSB_CFLAGS / LIBUSB_LIBS in the environment wins.
LIBUSB_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libusb-1.0 2>/dev/null)
LIBUSB_LIBS   ?= $(shell $(PKG_CONFIG) --libs libusb-1.0 2>/dev/null)

# Fall back to the usual defaults when pkg-config is not around.
ifeq ($(strip $(LIBUSB_LIBS)),)
LIBUSB_CFLAGS := -I/usr/include/libusb-1.0
LIBUSB_LIBS   := -lusb-1.0
endif

PREFIX  ?= /usr/local

BIN  := icast-libusb
OBJS := src/icast.o src/sink.o src/smit.o src/tsfilter.o
TEST := tests/sink_test tests/tsfilter_test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBUSB_LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) -c -o $@ $<

src/icast.o src/smit.o: src/smit.h
src/icast.o src/sink.o tests/sink_test: src/sink.h
src/icast.o src/sink.o: src/tsfilter.h

# The sink logic is testable without the dongle.
tests/sink_test: tests/sink_test.c src/sink.o src/tsfilter.o
	$(CC) $(CFLAGS) -o $@ tests/sink_test.c src/sink.o src/tsfilter.o

tests/tsfilter_test: tests/tsfilter_test.c src/tsfilter.o
	$(CC) $(CFLAGS) -o $@ tests/tsfilter_test.c src/tsfilter.o

test: $(TEST)
	./tests/sink_test
	./tests/tsfilter_test

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -d $(DESTDIR)$(PREFIX)/share/doc/icast-libusb
	install -m 0644 README.md $(DESTDIR)$(PREFIX)/share/doc/icast-libusb/
ifeq ($(shell id -u),0)
	install -d $(DESTDIR)/etc/udev/rules.d
	install -m 0644 udev/60-icast-libusb.rules $(DESTDIR)/etc/udev/rules.d/
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) $(OBJS) $(TEST)

.PHONY: all test install uninstall clean
