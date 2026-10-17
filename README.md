<p align="center">
  <img src="docs/_static/images/logo.svg">
</p>

<p align="center">
 PebbleOS 
</p>

<p align="center">
  <a href="https://github.com/coredevices/PebbleOS/actions/workflows/build-firmware.yml?query=branch%3Amain"><img src="https://github.com/coredevices/PebbleOS/actions/workflows/build-firmware.yml/badge.svg?branch=main"></a>
  <a href="https://pebbleos-core.readthedocs.io/en/latest"><img src="https://readthedocs.org/projects/pebbleos-core/badge/?version=latest&style=flat"></a>
  <a href="https://forum.repebble.com/"><img src="https://img.shields.io/discourse/posts?server=https%3A%2F%2Fforum.repebble.com&label=forum"></a>
</p>

## This fork: direct MiniMed pump connection

This fork adds a direct Bluetooth LE link from the watch to a Medtronic MiniMed 780G insulin pump
(no phone bridge required) and forwards live data to a companion watchface over the
[Pebble Glucose Protocol](https://github.com/mortenfyhn/pebble-glucose-protocol):

- Blood glucose (SG), trend arrow, insulin-on-board, and pump/SmartGuard status
- Pump-connection state and low-battery/annunciation alerts
- A dual-slot build for Pebble Time 2 (`obelix`) alongside the original `asterix` target
- Companion watchface: [mortenfyhn/pebble-glucose-watchface](https://github.com/mortenfyhn/pebble-glucose-watchface)

Start with `PROGRESS.md` (architecture/status), `TESTING.md` (build + flash loop),
`VERSIONS.md` (build/version history), `BATTERY.md`, `CONNECTIVITY.md`, and `WATCHFACE.md`.

- 🐛 [This fork's issue tracker](https://github.com/OpenMinimed/PebbleOS/issues)

## Resources

Here's a quick summary of resources to help you find your way around:

### Getting Started

- 📖 [Documentation](https://pebbleos-core.readthedocs.io/en/latest)
- 🚀 [Prerequisites Guide](https://pebbleos-core.readthedocs.io/en/latest/development/getting_started.html)

### Code and Development

- ⌚ [Source Code Repository](https://github.com/coredevices/PebbleOS)
- 🐛 [Issue Tracker](https://github.com/coredevices/PebbleOS/issues)
- 🤝 [Contribution Guide](CONTRIBUTING.md)

### Community and Support

- 💬 [Discord](https://discordapp.com/invite/aRUAYFN)
- 👥 [Discussions](https://github.com/coredevices/PebbleOS/discussions)
