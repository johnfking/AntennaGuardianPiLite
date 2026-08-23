#!/usr/bin/env python3
import json
import signal
import socket
import subprocess
import sys
import tempfile
import threading
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
                not_ready_sequence, not_ready_command = self._read_command(stream)
                if not_ready_command != "interlock not_ready 42":
                    raise AssertionError(
                        f"expected nested not-ready command, received {not_ready_command!r}"
                    )
                # Deliver the outer response while the nested command is waiting.
                self._reply(stream, ready_sequence)
                self._reply(stream, not_ready_sequence)

                stream.write(b"S0|transmit freq=7.074000 tx_antenna=ANT2\n")
                stream.flush()
                self._expect(stream, "interlock not_ready 42")
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


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_fake_radio.py PATH_TO_BINARY")

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
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

    print("fake radio integration test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
