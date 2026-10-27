# Vermeer audio

AudioReach topology, ALSA UCM profiles and the native CallAudio controller for
Poco F6 Pro / Redmi K70 (SM8550, `vermeer`). Requires the matching
[kernel](https://github.com/zuwwik/vermeer-mainline).

`firmware/` contains the deployed stereo topology, AW88261 configuration and
version 3 handset/speaker voice graphs. `prebuilt/aarch64-linux-musl/` contains
the deployed controller for the musl rootfs. Verify them with `sha256sum -c
SHA256SUMS`. These are the working versions; earlier firmware experiments are
not included.

## Build and install

Build on the phone with a C compiler, pkg-config, ALSA, GLib/GIO and PulseAudio
development headers:

```sh
make -j2
make DESTDIR="$PWD/stage" install
# To stage the deployed aarch64 musl binary without recompiling:
make DESTDIR="$PWD/stage" install-prebuilt
```

The staged tree contains firmware, the controller, UCM and session configuration.
Copy its contents into the target rootfs before booting it. On an existing phone,
back up the corresponding files and close audio applications first. Use the
matching kernel, DTB and modules; do not unload live DSP/audio modules to update
them. Firmware changes take effect after an ordinary reboot.

After installing ALSA UCM, PipeWire, pipewire-pulse, WirePlumber, ModemManager and
GNOME Calls, activate the controller from the desktop user's session:

```sh
mkdir -p ~/.local/share/dbus-1/services
cp voice/org.mobian_project.CallAudio.service ~/.local/share/dbus-1/services/
systemctl --user daemon-reload
systemctl --user enable --now vermeer-call-audio.service
```

This controller owns `org.mobian_project.CallAudio`; disable another callaudiod
instance if one owns that name. PipeWire and WirePlumber remain user services.
`voice/install-userspace.sh DESKTOP_USER` is the alternative live installer with
backups and card checks; it builds the controller and activates the service.
`install-vermeer-ucm.sh` installs the ordinary speaker/microphone profiles after
checking the loaded DT microphone wiring.

## Rebuild firmware

Rebuild the media topology with m4 and alsatplg/libatopology:

```sh
python3 build-vermeer-topology.py /path/to/audioreach-topology
```

The tested template revision is pinned in `sources.lock`. The upstream templates are from
[linux-msm/audioreach-topology](https://github.com/linux-msm/audioreach-topology).
Voice firmware can be regenerated from an ACDB export with
`python3 voice/build-firmware.py EXPORT_DIRECTORY NEW_OUTPUT_DIRECTORY`.
See [voice/README.md](voice/README.md) for the exporter and source revision.

## Scope

The ordinary capture path is one physical AMIC1 duplicated into two PCM
channels. Its analog gain is 8 and digital gain is 84 (unity). The speaker
amplifiers use raw volume 360; Android speaker protection is not implemented.
Call routing, system-media restoration and the native voice paths are included.
Bluetooth desktop audio uses the distribution's PipeWire/BlueZ stack; this does
not imply working Bluetooth modem-call speech or verified simultaneous dual-SIM
calls. Persistent DSP firmware failures remain a separate kernel/firmware issue.

Source licenses are identified per file. The topology templates use BSD-3-Clause;
the controller uses GPL-2.0-only. Stock amplifier and ACDB-derived data retain
their original vendor terms; no additional license is asserted for those blobs.
