#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$(id -u)" != 0 ] || [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
	echo "Usage (on the phone, as root): $0 DESKTOP_USER [--stage]" >&2
	exit 2
fi
desktop_user=$1
stage=${2:-}
if [ -n "$stage" ] && [ "$stage" != --stage ]; then
	exit 2
fi
desktop_uid=$(id -u "$desktop_user")
desktop_gid=$(id -g "$desktop_user")
desktop_home=$(getent passwd "$desktop_user" | cut -d: -f6)
source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=$(mktemp -d)
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM

cc -O2 -Wall -Wextra -Werror "$source_dir/voice-session.c" "$source_dir/voice-pcm-test.c" \
	$(pkg-config --cflags --libs alsa) -o "$build_dir/voice-pcm-test"
cc -O2 -Wall -Wextra -Werror "$source_dir/voice-session.c" "$source_dir/call-audio.c" "$source_dir/voice-subscriptions.c" \
	$(pkg-config --cflags --libs alsa gio-2.0 libpulse-mainloop-glib qmi-glib qrtr-glib) \
	-o "$build_dir/vermeer-call-audio"

install -d -m 700 /var/lib/vermeer-call-audio
backup_dir=$(mktemp -d /var/lib/vermeer-call-audio/backup.XXXXXX)
save() {
	if [ -e "$1" ] || [ -L "$1" ]; then
		mkdir -p "$backup_dir$(dirname -- "$1")"
		cp -a "$1" "$backup_dir$1"
	fi
}
for name in vermeer-call-audio voice-pcm-test; do
	save "/usr/local/libexec/$name"
	install -d /usr/local/libexec
	staged_binary=$(mktemp "/usr/local/libexec/.$name.XXXXXX")
	install -m 755 "$build_dir/$name" "$staged_binary"
	# Make data durable before atomically replacing a running executable.
	sync
	mv -f "$staged_binary" "/usr/local/libexec/$name"
done
for name in ucm.conf VermeerVoice.conf VoiceCall.conf; do
	save "/usr/local/share/vermeer-voice/ucm2/$name"
	install -D -m 644 "$source_dir/ucm2/$name" "/usr/local/share/vermeer-voice/ucm2/$name"
done
save /etc/systemd/user/vermeer-call-audio.service
install -D -m 644 "$source_dir/vermeer-call-audio.service" /etc/systemd/user/vermeer-call-audio.service
save /etc/wireplumber/wireplumber.conf.d/51-vermeer-alsa.conf
install -D -m 644 "$source_dir/51-vermeer-alsa.conf" \
	/etc/wireplumber/wireplumber.conf.d/51-vermeer-alsa.conf
save /etc/pipewire/pipewire.conf.d/52-vermeer-bluetooth.conf
install -D -m 644 "$source_dir/52-vermeer-bluetooth.conf" \
	/etc/pipewire/pipewire.conf.d/52-vermeer-bluetooth.conf
sync

if [ "$stage" = --stage ]; then
	echo "Voice controller staged; desktop activation is unchanged. Backup: $backup_dir"
	exit 0
fi

# Check the actual card, new controls and PCM before replacing activation.
# Zero seconds performs no UCM enable sequence and opens no voice stream.
/usr/local/libexec/voice-pcm-test /usr/local/share/vermeer-voice/ucm2 handset 0
if [ ! -S "/run/user/$desktop_uid/bus" ]; then
	echo "Desktop user must have a running session before activation" >&2
	exit 1
fi
as_user() {
	runuser -u "$desktop_user" -- env XDG_RUNTIME_DIR="/run/user/$desktop_uid" \
		DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$desktop_uid/bus" "$@"
}
service_dir="$desktop_home/.local/share/dbus-1/services"
save "$service_dir/org.mobian_project.CallAudio.service"
as_user mkdir -p "$service_dir"
install -m 644 -o "$desktop_uid" -g "$desktop_gid" \
	"$source_dir/org.mobian_project.CallAudio.service" \
	"$service_dir/org.mobian_project.CallAudio.service"
pkill -u "$desktop_uid" -x callaudiod || :
as_user systemctl --user daemon-reload
as_user systemctl --user enable vermeer-call-audio.service
as_user systemctl --user restart vermeer-call-audio.service
echo "Voice controller activated for $desktop_user. Backup: $backup_dir"
