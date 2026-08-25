#!/usr/bin/env python3
import json
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


class FakeRadio:
    def __init__(self) -> None:
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.connected = threading.Event()
        self.ready_for_stop = threading.Event()
        self.complete = threading.Event()
        self.error: BaseException | None = None

    def start(self) -> None:
        threading.Thread(target=self._run, daemon=True).start()

    @staticmethod
    def _read_command(stream) -> tuple[int, str]:
        line = stream.readline()
        if not line:
            raise AssertionError("client closed the connection")
        text = line.decode("ascii").strip()
        prefix, command = text.split("|", 1)
        if not prefix.startswith("C"):
            raise AssertionError(f"expected command, received {text!r}")
        return int(prefix[1:]), command

    @staticmethod
    def _reply(stream, sequence: int, body: str = "") -> None:
        stream.write(f"R{sequence}|0|{body}\n".encode("ascii"))
        stream.flush()

    def _expect(self, stream, expected: str, body: str = "") -> str:
        sequence, command = self._read_command(stream)
        if command != expected:
            raise AssertionError(f"expected {expected!r}, received {command!r}")
        self._reply(stream, sequence, body)
        return command

    def _run(self) -> None:
        try:
            connection, _ = self.listener.accept()
            connection.settimeout(10)
            self.connected.set()
            with connection, connection.makefile("rwb", buffering=0) as stream:
                self._expect(stream, "name AntennaGuardianPiLite")
                self._expect(stream, "sub radio all")
                self._expect(stream, "sub slice all")
                self._expect(stream, "sub tx all")
                self._expect(
                    stream,
                    "interlock create type=ANT name=AntennaGuardianPiLite "
                    "serial=raspberrypi valid_antennas=ANT1,ANT2",
                    "42|",
                )
                self._expect(stream, "interlock not_ready 42")

                stream.write(b"S0|transmit freq=14.074000 tx_antenna=ANT1\n")
                stream.write(b"S0|interlock state=PTT_REQUESTED source=SW\n")
                stream.flush()
                ready_sequence, ready_command = self._read_command(stream)
                if ready_command != "interlock ready 42":
                    raise AssertionError(f"expected ready command, received {ready_command!r}")
                stream.write(b"S0|interlock state=TRANSMITTING source=SW\n")
                stream.write(b"S0|interlock state=UNKEY_REQUESTED source=SW\n")
                stream.flush()
                self._reply(stream, ready_sequence)
                not_ready_sequence, not_ready_command = self._read_command(stream)
                if not_ready_command != "interlock not_ready 42":
                    raise AssertionError(
                        f"expected deferred not-ready command, received {not_ready_command!r}"
                    )
                self._reply(stream, not_ready_sequence)

                stream.write(b"S0|transmit freq=7.074000 tx_antenna=ANT2\n")
                stream.flush()
                self._expect(stream, "interlock not_ready 42")
                stream.write(b"S0|interlock state=PTT_REQUESTED source=SW\n")
                stream.flush()
                self._expect(stream, "interlock not_ready 42")

                # AetherSDR can publish a burst of transmit updates during a band
                # change. Delay their command replies to reproduce the radio's
                # response ordering under that load.
                burst_size = 20
                stream.write(
                    b"S0|transmit freq=7.074000 tx_antenna=ANT2\n" * burst_size
                )
                stream.flush()
                burst_commands = 0
                while select.select([connection], [], [], 0.25)[0]:
                    sequence, command = self._read_command(stream)
                    if command != "interlock not_ready 42":
                        raise AssertionError(
                            f"expected burst not-ready command, received {command!r}"
                        )
                    self._reply(stream, sequence)
                    burst_commands += 1
                if not 1 <= burst_commands <= 2:
                    raise AssertionError(
                        f"band change produced {burst_commands} interlock commands"
                    )

                stream.write(b"S0|interlock state=PTT_REQUESTED source=SW\n")
                stream.flush()
                self._expect(stream, "interlock not_ready 42")
                self.ready_for_stop.set()

                self._expect(stream, "interlock remove 42")
            self.complete.set()
        except BaseException as error:
            self.error = error
            self.ready_for_stop.set()
            self.complete.set()
        finally:
            self.listener.close()


class ObserveRadio(FakeRadio):
    def _run(self) -> None:
        try:
            connection, _ = self.listener.accept()
            connection.settimeout(10)
            self.connected.set()
            with connection, connection.makefile("rwb", buffering=0) as stream:
                self._expect(stream, "name AntennaGuardianPiLite")
                self._expect(stream, "sub radio all")
                self._expect(stream, "sub slice all")
                self._expect(stream, "sub tx all")

                stream.write(b"S0|transmit freq=7.171600 tx_antenna=ANT1\n")
                stream.write(b"S0|interlock state=PTT_REQUESTED source=SW\n")
                stream.write(b"S0|interlock state=TRANSMITTING source=SW\n")
                stream.write(b"S0|interlock state=UNKEY_REQUESTED source=SW\n")
                stream.flush()
                if select.select([connection], [], [], 0.25)[0]:
                    _, command = self._read_command(stream)
                    raise AssertionError(
                        f"observe mode unexpectedly sent command {command!r}"
                    )
                self.ready_for_stop.set()
                if stream.readline():
                    raise AssertionError("observe mode sent a command while stopping")
            self.complete.set()
        except BaseException as error:
            self.error = error
            self.ready_for_stop.set()
            self.complete.set()
        finally:
            self.listener.close()


class DelayedObserveRadio(ObserveRadio):
    def __init__(self, delay_seconds: float) -> None:
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.settimeout(10)
        self.port = self.listener.getsockname()[1]
        self.delay_seconds = delay_seconds
        self.connected = threading.Event()
        self.ready_for_stop = threading.Event()
        self.complete = threading.Event()
        self.error: BaseException | None = None

    def _run(self) -> None:
        time.sleep(self.delay_seconds)
        self.listener.listen(1)
        super()._run()


def discovery_packet(
    serial: str,
    ip: str,
    port: int,
    packet_class: int = 0x534CFFFF,
) -> bytes:
    payload = (
        f"model=FLEX-6600 serial={serial} version=3.8.23 "
        f"nickname=TestRadio callsign=N0CALL ip={ip} port={port}"
    ).encode("ascii")
    payload += b"\0" * (-len(payload) % 4)
    packet_words = (28 + len(payload)) // 4
    header = 0x38500000 | packet_words
    return struct.pack(">7I", header, 0x00000800, 0, packet_class, 0, 0, 0) + payload


def send_discovery(packet: bytes) -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.sendto(packet, ("127.0.0.1", 4992))


def test_discovery(binary: str) -> None:
    observer = ObserveRadio()
    observer.start()
    config = {
        "radio": {
            "serial": "1234-5678-6600-ABCD",
            "discovery_ip": "127.0.0.1",
        },
        "interlock": {"antennas": ["ANT1", "ANT2"]},
        "policy": {"ANT1": ["20m"], "ANT2": []},
    }
    with tempfile.TemporaryDirectory() as directory:
        config_path = Path(directory) / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen(
            [binary, "--config", str(config_path), "--observe", "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            time.sleep(0.2)
            rejected = (
                b"not a VITA packet",
                discovery_packet(
                    "1234-5678-6600-ABCD", "127.0.0.1", observer.port, 0x12345678
                ),
                discovery_packet("WRONG-SERIAL", "127.0.0.1", observer.port),
                discovery_packet(
                    "1234-5678-6600-ABCD", "127.0.0.2", observer.port
                ),
            )
            for packet in rejected:
                send_discovery(packet)
                time.sleep(0.1)
            if observer.connected.is_set():
                raise AssertionError("a non-matching discovery packet opened a TCP session")

            matching = discovery_packet(
                "1234-5678-6600-ABCD", "127.0.0.1", observer.port
            )
            for _ in range(10):
                send_discovery(matching)
                if observer.ready_for_stop.wait(0.2):
                    break
            if not observer.ready_for_stop.is_set():
                raise AssertionError("matching discovery packet did not open a TCP session")
            if observer.error is not None:
                raise observer.error
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode != 0:
                raise AssertionError(
                    f"discovery observer exited with {process.returncode}\n"
                    f"stdout:\n{stdout}\nstderr:\n{stderr}"
                )
            if "Discovered FLEX-6600 TestRadio serial 1234-5678-6600-ABCD" not in stderr:
                raise AssertionError(f"matching discovery was not logged\n{stderr}")
            if not observer.complete.wait(2):
                raise AssertionError("discovery observer did not disconnect cleanly")
            if observer.error is not None:
                raise observer.error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_fake_radio.py PATH_TO_BINARY")

    test_discovery(sys.argv[1])

    fake = FakeRadio()
    fake.start()
    config = {
        "radio": {"host": "127.0.0.1", "port": fake.port, "reconnect_seconds": 1},
        "interlock": {"antennas": ["ANT1", "ANT2"]},
        "policy": {"ANT1": ["20m"], "ANT2": ["10m", "6m"]},
    }
    with tempfile.TemporaryDirectory() as directory:
        config_path = Path(directory) / "config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        process = subprocess.Popen(
            [sys.argv[1], "--config", str(config_path), "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            if not fake.ready_for_stop.wait(10):
                raise AssertionError("fake radio did not complete policy checks")
            if fake.error is not None:
                raise fake.error
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode != 0:
                raise AssertionError(
                    f"client exited with {process.returncode}\nstdout:\n{stdout}\nstderr:\n{stderr}"
                )
            if not fake.complete.wait(2):
                raise AssertionError("client did not remove the interlock")
            if fake.error is not None:
                raise fake.error
            for expected in ("PROTECTED", "ALLOWED ANT1 on 20m", "BLOCKED ANT2 on 40m"):
                if expected not in stderr:
                    raise AssertionError(f"missing log entry {expected!r}\n{stderr}")
            if "response cache is full" in stderr:
                raise AssertionError(f"response cache overflowed during band change\n{stderr}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    delayed = DelayedObserveRadio(1.5)
    delayed.start()
    delayed_config = {
        "radio": {
            "host": "127.0.0.1",
            "port": delayed.port,
            "reconnect_seconds": 1,
            "reconnect_max_seconds": 2,
            "reconnect_log_seconds": 300,
        },
        "interlock": {"antennas": ["ANT1", "ANT2"]},
        "policy": {"ANT1": ["20m"], "ANT2": []},
    }
    with tempfile.TemporaryDirectory() as directory:
        config_path = Path(directory) / "config.json"
        config_path.write_text(json.dumps(delayed_config), encoding="utf-8")
        started = time.monotonic()
        process = subprocess.Popen(
            [sys.argv[1], "--config", str(config_path), "--observe"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            if not delayed.ready_for_stop.wait(5):
                raise AssertionError("client did not reconnect when the fake radio became available")
            elapsed = time.monotonic() - started
            if elapsed > 4.5:
                raise AssertionError(f"reconnection took too long: {elapsed:.2f} seconds")
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode != 0:
                raise AssertionError(
                    f"delayed observer exited with {process.returncode}\n"
                    f"stdout:\n{stdout}\nstderr:\n{stderr}"
                )
            if stderr.count("Radio unavailable; retrying") != 1:
                raise AssertionError(f"unavailable logs were not suppressed\n{stderr}")
            if "Connected to 127.0.0.1" not in stderr:
                raise AssertionError(f"successful reconnect was not logged\n{stderr}")
            if not delayed.complete.wait(2):
                raise AssertionError("delayed observer did not disconnect cleanly")
            if delayed.error is not None:
                raise delayed.error
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    observer = ObserveRadio()
    observer.start()
    observe_config = {
        "radio": {"host": "127.0.0.1", "port": observer.port, "reconnect_seconds": 1},
        "interlock": {"antennas": ["ANT1", "ANT2"]},
        "policy": {"ANT1": ["20m"], "ANT2": []},
    }
    with tempfile.TemporaryDirectory() as directory:
        config_path = Path(directory) / "config.json"
        config_path.write_text(json.dumps(observe_config), encoding="utf-8")
        process = subprocess.Popen(
            [sys.argv[1], "--config", str(config_path), "--observe", "--once"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            if not observer.ready_for_stop.wait(10):
                raise AssertionError("fake radio did not complete observe checks")
            if observer.error is not None:
                raise observer.error
            process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=10)
            if process.returncode != 0:
                raise AssertionError(
                    f"observer exited with {process.returncode}\nstdout:\n{stdout}\nstderr:\n{stderr}"
                )
            if not observer.complete.wait(2):
                raise AssertionError("observer did not disconnect cleanly")
            if observer.error is not None:
                raise observer.error
            for expected in (
                "WOULD BLOCK ANT1 on 40m",
                "OBSERVED TRANSMITTING outside policy",
                "Observed transmit ended",
            ):
                if expected not in stderr:
                    raise AssertionError(f"missing observe log entry {expected!r}\n{stderr}")
            for forbidden in ("FAULT", "interlock returned to not-ready"):
                if forbidden in stderr:
                    raise AssertionError(f"misleading observe log entry {forbidden!r}\n{stderr}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    print("fake radio integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
