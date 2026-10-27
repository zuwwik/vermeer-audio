#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test CallAudio API/profile restoration with a mock voice session.

Run as the desktop user. Uses a private D-Bus bus (also in place of the
system bus), never observes real calls and never loads a DSP voice graph.
The real desktop ALSA card profiles are briefly switched off and restored.
"""
import argparse
import configparser
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUS = "org.mobian_project.CallAudio"
OBJECT = "/org/mobian_project/CallAudio"


def profiles():
    if os.environ.get("VERMEER_TEST_PROFILES"):
        state = configparser.ConfigParser()
        state.optionxform = str
        state.read(os.environ["VERMEER_TEST_PROFILES"])
        return dict(state["cards"])
    cards = json.loads(subprocess.check_output(["pactl", "--format=json", "list", "cards"]))
    return {c["name"]: c["active_profile"] for c in cards
            if c.get("properties", {}).get("alsa.id") == "SM8550VERMEER"}


def dbus(method, *args, check=True):
    return subprocess.run(["gdbus", "call", "--session", "--dest", BUS,
                           "--object-path", OBJECT, "--method", method, *args],
                          text=True, capture_output=True, check=check, timeout=30)


def has_owner():
    result = subprocess.check_output(["gdbus", "call", "--session",
                                      "--dest", "org.freedesktop.DBus",
                                      "--object-path", "/org/freedesktop/DBus",
                                      "--method", "org.freedesktop.DBus.NameHasOwner", BUS],
                                     text=True, timeout=5)
    return "true" in result


def wait_for(predicate):
    end = time.monotonic() + 10
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.05)
    raise RuntimeError("Controller test timed out")


def inside(exe):
    # The controller sees an empty, private system bus instead of the
    # phone's real ModemManager. No real telephony service is contacted.
    os.environ["DBUS_SYSTEM_BUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
    baseline = profiles()
    default_sink = subprocess.check_output(["pactl", "get-default-sink"], text=True).strip()
    default_source = subprocess.check_output(["pactl", "get-default-source"], text=True).strip()
    assert baseline, "Vermeer desktop audio card is absent"
    process = None
    streams = []
    log = Path(exe).with_suffix(".log").open("w")
    state_file = Path(os.environ["XDG_RUNTIME_DIR"]) / "vermeer-voice-restore.ini"

    def start():
        child = subprocess.Popen([exe, "/unused-mock-ucm"], stdout=log, stderr=log)
        wait_for(has_owner)
        assert child.poll() is None
        return child

    def call(method, value):
        result = dbus(BUS + "." + method, value)
        assert "true" in result.stdout, result

    def capture_targets():
        sources = {s["index"]: s["name"] for s in json.loads(subprocess.check_output(
            ["pactl", "--format=json", "list", "sources"]))}
        outputs = json.loads(subprocess.check_output(
            ["pactl", "--format=json", "list", "source-outputs"]))
        return {s["properties"].get("application.name"): sources.get(s["source"]) for s in outputs}

    def check_internal_streams():
        if streams:
            targets = capture_targets()
            for name in ("vermeer-test-loopback",):
                assert targets[name] == default_source, targets

    try:
        if os.environ.get("VERMEER_TEST_PROFILES"):
            # Real Pulse clients, with no owning module, like WirePlumber's
            # Bluetooth microphone proxy. They must never be moved to a call
            # microphone. This only records the private null test source.
            for name, prop in (("vermeer-test-loopback", "bluez5.loopback=true"),):
                streams.append(subprocess.Popen(
                    ["parec", "--raw", "--device=" + default_source,
                     "--client-name=" + name, "--property=" + prop],
                    stdout=subprocess.DEVNULL, stderr=log))
            try:
                wait_for(lambda: all(capture_targets().get(name) == default_source for name in
                         ("vermeer-test-loopback",)))
            except RuntimeError:
                print(subprocess.check_output(["pactl", "--format=json", "list", "source-outputs"],
                                              text=True), file=sys.stderr)
                raise
        process = start()
        assert dbus(BUS + ".SelectMode", "2", check=False).returncode != 0
        call("SelectMode", "1")
        assert all(value == "off" for value in profiles().values())
        sinks = json.loads(subprocess.check_output(["pactl", "--format=json", "list", "sinks"]))
        assert any(s["name"] == "vermeer_voice" for s in sinks)
        sources = json.loads(subprocess.check_output(["pactl", "--format=json", "list", "sources"]))
        assert any(s["name"] == "vermeer_voice_mic" for s in sources)
        check_internal_streams()
        subprocess.run(["pactl", "set-sink-volume", "vermeer_voice", "40%"], check=True)
        if os.environ.get("VERMEER_TEST_VOLUME"):
            wait_for(lambda: Path(os.environ["VERMEER_TEST_VOLUME"]).read_text() == "2 0\n")
            subprocess.run(["pactl", "set-sink-mute", "vermeer_voice", "1"], check=True)
            wait_for(lambda: Path(os.environ["VERMEER_TEST_VOLUME"]).read_text() == "2 1\n")
            subprocess.run(["pactl", "set-sink-mute", "vermeer_voice", "0"], check=True)
        subprocess.run(["pactl", "set-source-volume", "vermeer_voice_mic", "50%"], check=True)
        if os.environ.get("VERMEER_TEST_CAPTURE"):
            wait_for(lambda: Path(os.environ["VERMEER_TEST_CAPTURE"]).read_text() == "1024 0\n")
            subprocess.run(["pactl", "set-source-mute", "vermeer_voice_mic", "1"], check=True)
            wait_for(lambda: Path(os.environ["VERMEER_TEST_CAPTURE"]).read_text() == "1024 1\n")
            assert "uint32 0" in dbus("org.freedesktop.DBus.Properties.Get", BUS, "MicState").stdout
            subprocess.run(["pactl", "set-source-mute", "vermeer_voice_mic", "0"], check=True)
        call("EnableSpeaker", "true")
        call("MuteMic", "true")
        assert "uint32 0" in dbus("org.freedesktop.DBus.Properties.Get", BUS, "MicState").stdout
        call("MuteMic", "false")
        call("SelectMode", "0")
        check_internal_streams()
        wait_for(lambda: profiles() == baseline and not state_file.exists())
        if os.environ.get("VERMEER_TEST_SUBSCRIPTION"):
            subscription = Path(os.environ["VERMEER_TEST_SUBSCRIPTION"])
            selected = Path(os.environ["VERMEER_TEST_SELECTED_SUBSCRIPTION"])
            for first, second in ((2, 1), (1, 2)):
                subscription.write_text(f"{first} 1\n")
                time.sleep(0.3)
                call("SelectMode", "1")
                wait_for(lambda: selected.read_text() == f"{first}\n")
                subscription.write_text(f"{second} 1\n")
                wait_for(lambda: selected.read_text() == f"{second}\n")
                call("EnableSpeaker", "false")
                assert selected.read_text() == f"{second}\n"
                call("SelectMode", "0")
                wait_for(lambda: profiles() == baseline and not state_file.exists())
            print("PASS: native subscription events select SIM2 with unchanged primary SIM; active switching and speaker changes preserve the subscription")
        assert subprocess.check_output(["pactl", "get-default-sink"], text=True).strip() == default_sink
        assert subprocess.check_output(["pactl", "get-default-source"], text=True).strip() == default_source
        print("PASS: CallAudio methods, invalid mode, speaker/mic state, profile restoration")
        assert not any(s["name"] == "vermeer_voice" for s in json.loads(
            subprocess.check_output(["pactl", "--format=json", "list", "sinks"])))
        print("PASS: PipeWire call output/microphone, volume/mute, device removal and default restoration")
        for _ in range(2):
            call("SelectMode", "1")
            check_internal_streams()
            call("SelectMode", "0")
            check_internal_streams()
            assert profiles() == baseline and not state_file.exists()
        print("PASS: repeated calls restore devices after delayed ALSA release")
        if streams:
            print("PASS: Bluetooth internal capture streams retain their source across calls")
        call("SelectMode", "1")
        assert state_file.exists()
        process.kill()
        process.wait(timeout=10)
        process = None
        wait_for(lambda: not has_owner())
        process = start()
        wait_for(lambda: profiles() == baseline and not state_file.exists())
        print("PASS: SIGKILL followed by restart restores both desktop card profiles")
        for active in (False, True):
            if active:
                call("SelectMode", "1")
            elif os.environ.get("VERMEER_TEST_PROFILES"):
                # ALSA re-enumeration may expose an Off profile before policy
                # restores it. Keep the pre-disconnection idle snapshot.
                time.sleep(0.2)
                state = configparser.ConfigParser()
                state.optionxform = str
                state["cards"] = {name: "off" for name in baseline}
                with open(os.environ["VERMEER_TEST_PROFILES"], "w") as output:
                    state.write(output)
            process.send_signal(signal.SIGUSR1)  # Mock ALSA poll HUP.
            assert process.wait(timeout=5) == 1
            process = None
            wait_for(lambda: not has_owner())
            assert not any(s["name"] == "vermeer_voice" for s in json.loads(
                subprocess.check_output(["pactl", "--format=json", "list", "sinks"])))
            assert not any(s["name"] == "vermeer_voice_mic" for s in json.loads(
                subprocess.check_output(["pactl", "--format=json", "list", "sources"])))
            assert state_file.exists()
            saved = configparser.ConfigParser()
            saved.read(state_file)
            assert {saved[s]["name"]: saved[s]["profile"] for s in saved.sections()
                    if s != "desktop"} == baseline, (baseline, state_file.read_text())
            process = start()
            wait_for(lambda: profiles() == baseline and not state_file.exists())
        print("PASS: idle/active ALSA disconnection saves working profiles and removes call adapters")
        call("SelectMode", "1")
        process.send_signal(signal.SIGTERM)
        assert process.wait(timeout=15) == 0
        process = None
        assert profiles() == baseline and not state_file.exists()
        print("PASS: SIGTERM closes the route and restores desktop audio")
    finally:
        for stream in streams:
            stream.terminate()
            stream.wait(timeout=5)
        if process and process.poll() is None:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        # Test-only final restoration, even if a test assertion fails.
        if not os.environ.get("VERMEER_TEST_PROFILES"):
            for name, profile in baseline.items():
                subprocess.run(["pactl", "set-card-profile", name, profile], check=True)
        log.close()


def isolated():
    """Private PipeWire instance: no desktop device or real modem is touched."""
    with tempfile.TemporaryDirectory(prefix="vermeer-pulse-test-") as tmp:
        tmp = Path(tmp)
        runtime = tmp / "runtime"
        runtime.mkdir(mode=0o700)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), PIPEWIRE_RUNTIME_DIR=str(runtime),
                   PULSE_RUNTIME_PATH=str(runtime / "pulse"),
                   PULSE_SERVER="unix:" + str(runtime / "pulse/native"),
                   VERMEER_TEST_PROFILES=str(tmp / "profiles.ini"),
                   VERMEER_TEST_VOLUME=str(tmp / "volume.txt"))
        env["VERMEER_TEST_CAPTURE"] = str(tmp / "capture.txt")
        env["VERMEER_TEST_SUBSCRIPTION"] = str(tmp / "subscription.txt")
        env["VERMEER_TEST_SELECTED_SUBSCRIPTION"] = str(tmp / "selected-subscription.txt")
        env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + str(tmp / "unused-bus")
        env.update(XDG_STATE_HOME=str(tmp / "state"), XDG_CONFIG_HOME=str(tmp / "config"),
                   XDG_CACHE_HOME=str(tmp / "cache"))
        config = tmp / "pipewire.conf"
        config.write_text(Path("/usr/share/pipewire/pipewire.conf").read_text())
        wp_config = tmp / "wireplumber.conf"
        disabled = ["hardware.audio", "hardware.bluetooth", "hardware.video-capture",
                    "monitor.alsa", "monitor.alsa-midi", "monitor.bluez", "monitor.bluez-midi",
                    "monitor.libcamera", "monitor.v4l2", "support.logind"]
        wp_config.write_text(Path("/usr/share/wireplumber/wireplumber.conf").read_text().replace(
            "  main = {", "  main = {\n" + "\n".join(f"    {f} = disabled" for f in disabled), 1) +
            # Test the controller's explicit stream moves independently of
            # policy moving ordinary streams that used the former default.
            "\nwireplumber.settings = { linking.follow-default-target = false }\n")
        Path(env["VERMEER_TEST_PROFILES"]).write_text(
            "[cards]\nalsa_card.platform-sound=HiFi\nalsa_card.comprC0=HiFi\n")
        Path(env["VERMEER_TEST_VOLUME"]).write_text("")
        Path(env["VERMEER_TEST_CAPTURE"]).write_text("")
        Path(env["VERMEER_TEST_SUBSCRIPTION"]).write_text("1 1\n")
        Path(env["VERMEER_TEST_SELECTED_SUBSCRIPTION"]).write_text("")
        servers = []
        with (tmp / "servers.log").open("w+") as log:
            try:
                # Device monitors are explicitly disabled in this private
                # WirePlumber. Real default-node policy remains enabled.
                servers.append(subprocess.Popen(["pipewire", "-c", str(config)],
                                                env=env, stdout=log, stderr=log))
                servers.append(subprocess.Popen(["pipewire-pulse"], env=env, stdout=log, stderr=log))
                servers.append(subprocess.Popen(["wireplumber", "-c", str(wp_config)],
                                                env=env, stdout=log, stderr=log))
                deadline = time.monotonic() + 10
                while subprocess.run(["pactl", "info"], env=env,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
                    if time.monotonic() > deadline or any(s.poll() is not None for s in servers):
                        raise RuntimeError("Private PipeWire server did not start")
                    time.sleep(0.05)
                while not any(o.get("type", "").endswith(":Metadata") and
                              o.get("props", {}).get("metadata.name") == "default"
                              for o in json.loads(subprocess.check_output(["pw-dump"], env=env))):
                    if time.monotonic() > deadline:
                        raise RuntimeError("Private default metadata did not become ready")
                    time.sleep(0.05)
                subprocess.run(["pactl", "load-module", "module-null-sink",
                                "sink_name=test_desktop"], env=env, check=True,
                               stdout=subprocess.DEVNULL)
                subprocess.run(["pactl", "set-default-sink", "test_desktop"], env=env, check=True)
                subprocess.run(["pactl", "load-module", "module-remap-source",
                                "master=test_desktop.monitor source_name=test_microphone"], env=env,
                               check=True, stdout=subprocess.DEVNULL)
                subprocess.run(["pactl", "set-default-source", "test_microphone"], env=env, check=True)
                deadline = time.monotonic() + 10
                while subprocess.check_output(["pactl", "get-default-source"], env=env,
                                              text=True).strip() != "test_microphone":
                    if time.monotonic() > deadline:
                        raise RuntimeError("Private default-node policy did not become ready")
                    time.sleep(0.05)
                subprocess.run([sys.executable, str(Path(__file__).resolve())], env=env, check=True)
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                for server in reversed(servers):
                    server.terminate()
                    server.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inside", type=Path)
    parser.add_argument("--isolated", action="store_true")
    args = parser.parse_args()
    if args.isolated:
        isolated()
        return
    if args.inside:
        inside(str(args.inside))
        return
    with tempfile.TemporaryDirectory(prefix="vermeer-call-audio-test-") as tmp:
        tmp = Path(tmp)
        mock = tmp / "mock-session.c"
        mock.write_text((HERE / "test-pulse.c").read_text())
        exe = tmp / "controller"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "alsa",
                                         "gio-2.0", "libpulse-mainloop-glib"], text=True).split()
        subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I", str(HERE),
                        str(HERE / "call-audio.c"), str(mock), "-o", str(exe), "-ldl", *flags], check=True)
        env = os.environ.copy()
        env["PULSE_SERVER"] = env.get("PULSE_SERVER", "unix:" + env["XDG_RUNTIME_DIR"] + "/pulse/native")
        runtime = tmp / "runtime"
        runtime.mkdir(mode=0o700)
        env["XDG_RUNTIME_DIR"] = str(runtime)
        result = subprocess.run(["dbus-run-session", "--", sys.executable,
                                 str(Path(__file__).resolve()), "--inside", str(exe)], env=env)
        if result.returncode:
            print(exe.with_suffix(".log").read_text(), file=sys.stderr)
            raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
