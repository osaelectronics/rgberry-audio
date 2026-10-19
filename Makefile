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

all: rgberry-audio libasound_module_pcm_rgberry.so libasound_module_ctl_rgberry.so

rgberry-audio: src/rgberry_audio.c src/dpwm.h src/rgb_shm.h $(PIOLIB_SRC)
	$(CC) $(CFLAGS) -o $@ src/rgberry_audio.c $(PIOLIB_SRC) $(LDLIBS)

libasound_module_pcm_rgberry.so: src/pcm_rgberry.c src/rgb_shm.h
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -Isrc -o $@ src/pcm_rgberry.c -lasound

libasound_module_ctl_rgberry.so: src/ctl_rgberry.c src/rgb_shm.h
	$(CC) $(CFLAGS) -fPIC -DPIC -shared -Isrc -o $@ src/ctl_rgberry.c -lasound

# Host-side tests (no Pi needed): instruction encoding + PIO emulation
test: test/encode_check test/pio_sim test/plugin_test libasound_module_pcm_rgberry.so
	./test/encode_check
	./test/plugin_test $(CURDIR)/libasound_module_pcm_rgberry.so 5
	cd test && ./pio_sim 0.3 && python3 analyze.py sim_out.f32

test/encode_check: test/encode_check.c src/dpwm.h
	$(CC) -O2 -Ithird_party/piolib/include -o $@ $< -lm

test/pio_sim: test/pio_sim.c src/dpwm.h
	$(CC) -O2 -o $@ $< -lm

test/plugin_test: test/plugin_test.c src/rgb_shm.h
	$(CC) -O2 -o $@ $< -lasound -pthread

ALSA_PLUGIN_DIR ?= $(shell d=$$(pkg-config --variable=libdir alsa 2>/dev/null); [ -n "$$d" ] || d=/usr/lib/$$(gcc -dumpmachine); echo $$d/alsa-lib)

install: rgberry-audio libasound_module_pcm_rgberry.so libasound_module_ctl_rgberry.so
	install -D -m 755 rgberry-audio $(DESTDIR)$(PREFIX)/bin/rgberry-audio
	install -D -m 644 libasound_module_pcm_rgberry.so $(DESTDIR)$(ALSA_PLUGIN_DIR)/libasound_module_pcm_rgberry.so
	install -D -m 644 libasound_module_ctl_rgberry.so $(DESTDIR)$(ALSA_PLUGIN_DIR)/libasound_module_ctl_rgberry.so

clean:
	rm -f rgberry-audio libasound_module_pcm_rgberry.so libasound_module_ctl_rgberry.so test/plugin_test test/encode_check test/pio_sim test/sim_out.f32

.PHONY: all test install clean
