# RGBerry SCART - Pi 5 audio daemon
CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -pthread \
           -Isrc -Ithird_party/piolib/include -DLIBRARY_BUILD=1
LDLIBS  += -lasound -lm -pthread

PIOLIB_SRC = third_party/piolib/piolib.c \
             third_party/piolib/library_piochips.c \
             third_party/piolib/pio_rp1.c

PREFIX  ?= /usr/local

all: rgberry-audio

rgberry-audio: src/rgberry_audio.c src/dpwm.h $(PIOLIB_SRC)
	$(CC) $(CFLAGS) -o $@ src/rgberry_audio.c $(PIOLIB_SRC) $(LDLIBS)

# Host-side tests (no Pi needed): instruction encoding + PIO emulation
test: test/encode_check test/pio_sim
	./test/encode_check
	cd test && ./pio_sim 0.3 && python3 analyze.py sim_out.f32

test/encode_check: test/encode_check.c src/dpwm.h
	$(CC) -O2 -Ithird_party/piolib/include -o $@ $< -lm

test/pio_sim: test/pio_sim.c src/dpwm.h
	$(CC) -O2 -o $@ $< -lm

install: rgberry-audio
	install -D -m 755 rgberry-audio $(DESTDIR)$(PREFIX)/bin/rgberry-audio

clean:
	rm -f rgberry-audio test/encode_check test/pio_sim test/sim_out.f32

.PHONY: all test install clean
