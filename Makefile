# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
BUILD ?= build
VOICE_DEPS = alsa gio-2.0 libpulse-mainloop-glib

.PHONY: all install install-data install-prebuilt clean
all: $(BUILD)/vermeer-call-audio $(BUILD)/voice-pcm-test

$(BUILD):
	mkdir -p $@

$(BUILD)/vermeer-call-audio: voice/call-audio.c voice/voice-session.c voice/voice-session.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror voice/call-audio.c voice/voice-session.c $(LDFLAGS) $$( $(PKG_CONFIG) --cflags --libs $(VOICE_DEPS) ) -o $@

$(BUILD)/voice-pcm-test: voice/voice-pcm-test.c voice/voice-session.c voice/voice-session.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -Werror voice/voice-pcm-test.c voice/voice-session.c $(LDFLAGS) $$( $(PKG_CONFIG) --cflags --libs alsa ) -o $@

install: all install-data
	install -Dm755 $(BUILD)/vermeer-call-audio $(DESTDIR)$(PREFIX)/libexec/vermeer-call-audio
	install -Dm755 $(BUILD)/voice-pcm-test $(DESTDIR)$(PREFIX)/libexec/voice-pcm-test

install-prebuilt: install-data
	sha256sum -c SHA256SUMS
	install -Dm755 prebuilt/aarch64-linux-musl/vermeer-call-audio $(DESTDIR)$(PREFIX)/libexec/vermeer-call-audio

install-data:
	install -Dm644 voice/vermeer-call-audio.service $(DESTDIR)/etc/systemd/user/vermeer-call-audio.service
	install -Dm644 voice/51-vermeer-alsa.conf $(DESTDIR)/etc/wireplumber/wireplumber.conf.d/51-vermeer-alsa.conf
	install -Dm644 voice/52-vermeer-bluetooth.conf $(DESTDIR)/etc/pipewire/pipewire.conf.d/52-vermeer-bluetooth.conf
	install -d $(DESTDIR)$(PREFIX)/share/vermeer-voice/ucm2 $(DESTDIR)/usr/share/alsa/ucm2 $(DESTDIR)/lib/firmware/qcom/sm8550
	install -m644 voice/ucm2/*.conf $(DESTDIR)$(PREFIX)/share/vermeer-voice/ucm2/
	cp -R ucm2/. $(DESTDIR)/usr/share/alsa/ucm2/
	install -m644 firmware/qcom/sm8550/*.bin $(DESTDIR)/lib/firmware/qcom/sm8550/

clean:
	rm -f $(BUILD)/vermeer-call-audio $(BUILD)/voice-pcm-test
