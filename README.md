# AntennaGuardianPiLite

**A small, terminal-only Flex antenna interlock for Raspberry Pi and Linux.**

Looking for the Windows overlay and graphical policy editor? See
[AntennaGuardian](https://github.com/johnfking/AntennaGuardian).

AntennaGuardianPiLite enforces an explicit antenna-by-band allow matrix through
the Flex Ethernet antenna interlock. It is a single C11 executable designed to
run in a terminal or as a locked-down `systemd` service. Because enforcement
happens at the radio, the policy applies whether PTT originates in SmartSDR,
SmartSDR CAT, WSJT-X, or another client.

## Safety behavior

AntennaGuardianPiLite is deliberately fail closed while connected:

- It creates a dynamic `ANT` interlock and immediately asserts `not_ready`.
- It sends `ready` only for a PTT request with a known, allowed frequency and
  transmit antenna.
- Unknown frequency, unknown antenna, unsupported bands, and unconfigured
  combinations remain blocked.
- Unkey, forbidden context changes, and out-of-policy transmit reports reassert
  `not_ready`.
- `SIGINT` and `SIGTERM` remove the dynamic interlock before exit when the radio
  connection is still available.
- Connection loss is visible in the logs and triggers a bounded reconnect loop.

Software is an additional guard, not a substitute for the radio's hardware
protection, correct station wiring, or responsible RF operation.

## Supported bands

The native Flex HF/6m catalog is built in: `160m`, `80m`, `60m`, `40m`, `30m`,
`20m`, `17m`, `15m`, `12m`, `10m`, and `6m`.

## Configuration

```json
{
  "radio": {
    "host": "10.0.0.107",
    "port": 4992,
    "reconnect_seconds": 3,
    "reconnect_max_seconds": 30,
    "reconnect_log_seconds": 300
  },
  "interlock": {
    "antennas": ["ANT1", "ANT2"]
  },
  "policy": {
    "ANT1": ["160m", "80m", "60m", "40m", "30m", "20m", "17m"],
    "ANT2": ["20m", "17m", "15m", "12m", "10m", "6m"]
  }
}
```

Configuration is intentionally strict. Unknown keys, unknown bands, duplicate
entries, missing policy rows, and malformed antenna IDs are rejected before a
network connection is opened. An empty band array is valid and blocks that
antenna on every band.

When the radio is unavailable, retry delays grow exponentially from
`reconnect_seconds` to `reconnect_max_seconds`. With the example values, PiLite
retries after 3, 6, 12, 24, and then 30 seconds. A successful connection resets
the next retry to the initial delay. Repeated unavailable-radio messages are
suppressed and emitted no more often than `reconnect_log_seconds`, while quiet
connection attempts continue in the background.

Existing configurations that only specify `reconnect_seconds` remain valid.
For responsive recovery and a quiet journal, configure all three values rather
than increasing the initial retry delay.

## Install a release

Download the archive that matches the Linux system architecture:

- `linux-arm64` for 64-bit Raspberry Pi OS and ARM Linux.
- `linux-armv7` for 32-bit Raspberry Pi OS and ARM Linux.
- `linux-x86_64` for Intel or AMD 64-bit Linux.

Then install without starting the service:

```bash
tar -xzf AntennaGuardianPiLite-linux-arm64.tar.gz
cd AntennaGuardianPiLite-linux-arm64
sudo ./install.sh
```

The installer creates a restricted service account and configuration file, but
it deliberately does **not** enable or start protection. Review the printed
steps and validate the configuration first.

## Service control

Edit the installed policy, then validate it without contacting the radio:

```bash
sudoedit /etc/antennaguardian-pilite/config.json
sudo -u antennaguardian /usr/local/bin/antennaguardian-pilite \
  --config /etc/antennaguardian-pilite/config.json --check-config
```

Use the standard systemd commands to control protection:

```bash
# Start protection now.
sudo systemctl start antennaguardian-pilite

# Show service state and recent log messages.
sudo systemctl status antennaguardian-pilite

# Follow the live service log.
sudo journalctl -u antennaguardian-pilite -f

# Reload protection after changing the configuration.
sudo systemctl restart antennaguardian-pilite

# Stop protection cleanly.
sudo systemctl stop antennaguardian-pilite
```

To start protection automatically whenever Linux boots, enable the service.
The `--now` form also starts it immediately:

```bash
sudo systemctl enable --now antennaguardian-pilite
```

To stop protection and remove it from automatic startup:

```bash
sudo systemctl disable --now antennaguardian-pilite
```

`systemctl stop` and `systemctl restart` ask PiLite to shut down cleanly. While
the radio connection remains available, PiLite removes its dynamic interlock
before exiting.

## Terminal use

```bash
# Validate only; no socket is opened.
antennaguardian-pilite --config ./config.json --check-config

# Inspect radio state without creating an interlock.
antennaguardian-pilite --config ./config.json --observe

# Run protection in the foreground.
antennaguardian-pilite --config ./config.json
```

Use `--verbose` to include raw Flex command and status traffic.

## Build and test

Requirements are a C11 compiler, GNU Make, and Python 3 for the fake-radio
integration test. Python is not used by the application or required at runtime.

```bash
make
make test
```

The test suite uses only a loopback fake Flex server. It verifies immediate
`not_ready`, allowed and blocked PTT decisions, unkey behavior, interleaved
command responses, and clean interlock removal without contacting a radio.

## Author

AntennaGuardianPiLite is created and maintained by John, W3JFK, a former USAF
Communications Intelligence specialist and professional software developer for
the past 25 years. John was licensed in Germany in 1995 and operated as DA4KI
and DA2KI for nearly a decade.

## License

MIT. See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).
