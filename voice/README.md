# Native voice support

`call-audio.c` implements the CallAudio D-Bus interface and follows ModemManager
call state. `voice-session.c` manages the hostless ALSA voice PCM and UCM route.
The controller preserves the desktop profile/default devices and restores them
after call teardown or audio service recovery. The matching kernel supplies
handset/speaker routing, volume and microphone mute controls.

The version 3 firmware contains the RX/TX graphs and calibration used by the
working phone. The TX volume-control module is after the ordinary microphone
tap. The kernel validates all image sizes and graph endpoints before use.

To regenerate it, build `ar_osal` and `acdb` from
[AudioReach graphservices](https://github.com/AudioReach/audioreach-graphservices)
revision `76d04ffb30caa1fe609b94f9f6081210cb962fc1`:

```sh
autoreconf -fi
./configure --with-cutils=no --with-syslog=yes --with-dummy_diag=yes \
  --with-glib=yes --with-ats_transport_tcp_ip=no --with-ats_data_logging=no
make -C ar_osal -j4
make -C acdb -j4
```

Set `GS` to that checkout, then run from this repository:

```sh
cc -O2 -Wall -Wextra -Werror -I"$GS/acdb/api" -I"$GS/ar_osal/api" \
  voice/export-acdb.c -L"$GS/acdb/.libs" -L"$GS/ar_osal/.libs" \
  -lar-acdb -lar-osal -o build/export-acdb
LD_LIBRARY_PATH="$GS/acdb/.libs:$GS/ar_osal/.libs" \
  build/export-acdb /path/to/Tutu_acdb_cal.acdb /path/to/new-export
python3 voice/check-export.py /path/to/new-export
python3 voice/build-firmware.py /path/to/new-export /path/to/new-firmware
```

Use the stock ACDB from the same device firmware. Export directories are inputs
kept outside Git. `decode-gpr.py` is an optional offline protocol decoder.

The host regression test takes the kernel source through `KERNEL_SOURCE`:

```sh
KERNEL_SOURCE=../vermeer-mainline python3 voice/test-driver.py firmware/qcom/sm8550
```

`test-controller.py` requires an explicitly prepared private audio session with
mock voice hardware. Do not run it against a normal desktop or a live call.
