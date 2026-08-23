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
`20m`, `17m`, `15m`, `12m`, `10m`, and `6m`. There is no 2-meter entry.

## Configuration

```json
{
  "radio": {
    "host": "10.0.0.107",
    "port": 4992,
    "reconnect_seconds": 3
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

## Install a release

Download the archive for the Pi operating system architecture:

- `linux-arm64` for 64-bit Raspberry Pi OS.
- `linux-armv7` for 32-bit Raspberry Pi OS.

Then install without starting the service:

```bash
tar -xzf AntennaGuardianPiLite-linux-arm64.tar.gz
cd AntennaGuardianPiLite-linux-arm64
sudo ./install.sh
```

The installer creates a restricted service account and configuration file, but
it deliberately does **not** enable or start protection. Review the printed
steps and validate the configuration first.

## Terminal use

```bash
# Validate only; no socket is opened.
antennaguardian-pilite --config ./config.json --check-config

# Inspect radio state without creating an interlock.
antennaguardian-pilite --config ./config.json --observe

# Run protection in the foreground.
antennaguardian-pilite --config ./config.json
```

Use `--verbose` to include raw Flex command and status traffic. Service logs are
available through `journalctl -u antennaguardian-pilite`.

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
