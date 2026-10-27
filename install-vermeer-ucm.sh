#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Run on the Arch phone, not on the Gentoo build host.
set -eu

if [ "$#" -ne 0 ]; then
	printf '%s\n' 'Usage: sudo sh install-vermeer-ucm.sh (on the Vermeer phone)' >&2
	exit 2
fi

if [ "$(id -u)" -ne 0 ]; then
	printf '%s\n' 'Run this installer as root on the Vermeer phone.' >&2
	exit 1
fi

if ! grep -q 'SM8550VERMEER' /proc/asound/cards 2>/dev/null; then
	printf '%s\n' 'SM8550VERMEER sound card not found. Refusing to install on this system.' >&2
	exit 1
fi

if ! command -v alsaucm >/dev/null 2>&1 || [ ! -f /usr/share/alsa/ucm2/ucm.conf ]; then
	printf '%s\n' 'Install alsa-utils and alsa-ucm-conf on the phone first.' >&2
	exit 1
fi

# Preserve Android's micbias and require the mainline AMIC1 DAPM route.
# The intermediate Android-pair DT used INPUT4/9 and cannot power AMIC1
# when the corrected profile selects INPUT0. Voltage alone is insufficient.
micbias_property=/sys/firmware/devicetree/base/audio-codec/qcom,micbias1-microvolt
if [ ! -r "$micbias_property" ] ||
   [ "$(od -An -tx1 "$micbias_property" | tr -d '[:space:]')" != '0029f630' ]; then
	printf '%s\n' \
		'Updated Vermeer microphone DT (2.75 V micbias) not found.' \
		'Boot the updated kernel/DTB and install its lpass-tx-macro module first.' \
		'Existing UCM files were not changed.' >&2
	exit 1
fi

audio_routes=/sys/firmware/devicetree/base/sound/audio-routing
if [ ! -r "$audio_routes" ] ||
   ! tr '\000' '\n' < "$audio_routes" | awk '
       NR % 2 == 1 { sink = $0; next }
       sink == "TX SWR_INPUT0" && $0 == "ADC1_OUTPUT" { found = 1 }
       END { exit !found }'; then
	printf '%s\n' \
		'Corrected Vermeer DT route TX SWR_INPUT0 -> ADC1_OUTPUT not found.' \
		'Boot the corrected DTB before installing this microphone profile.' \
		'Existing UCM files were not changed.' >&2
	exit 1
fi

profile_source=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/ucm2
profile_files='conf.d/sm8550/SM8550-VERMEER.conf Qualcomm/sm8550/VERMEER/HiFi.conf'
for profile_file in $profile_files; do
	if [ ! -f "$profile_source/$profile_file" ]; then
		printf 'Missing profile file: %s\n' "$profile_source/$profile_file" >&2
		exit 1
	fi
done

profile_backup=''
for profile_file in $profile_files; do
	profile_target=/usr/share/alsa/ucm2/$profile_file
	if [ -e "$profile_target" ] || [ -L "$profile_target" ]; then
		if [ -z "$profile_backup" ]; then
			install -d -m0755 /var/backups
			profile_backup=$(mktemp -d /var/backups/vermeer-ucm2.XXXXXXXX)
		fi
		install -d -m0755 "$(dirname -- "$profile_backup/$profile_file")"
		cp -a -- "$profile_target" "$profile_backup/$profile_file"
	fi
done

if [ -n "$profile_backup" ]; then
	printf 'Previous profile files backed up to: %s\n' "$profile_backup"
fi
for profile_file in $profile_files; do
	install -Dm0644 -- "$profile_source/$profile_file" "/usr/share/alsa/ucm2/$profile_file"
done

alsaucm -c hw:SM8550VERMEER list _verbs
printf '%s\n' \
	'Installed the Vermeer UCM speaker/AMIC1 profile (one physical microphone).' \
	'Requires the matching updated Vermeer DTB and lpass-tx-macro module.' \
	'WARNING: this profile sets both amps to 360 (0 dB), without Android DSP speaker protection.' \
	'Lower software output volume before restarting audio or selecting HiFi.' \
	'As your normal KDE user (not root), run:' \
	'  systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service' \
	'  wpctl status' \
	'Select the HiFi profile and Built-in stereo speakers in KDE if necessary.'
