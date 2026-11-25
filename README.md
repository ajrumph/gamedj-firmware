# gamedj-firmware

Over-the-air firmware updates for Game DJ units.

Each unit reads `<board>/manifest.json` at start-up and every 6 hours:

```json
{"version": "1.3.1", "url": "https://raw.githubusercontent.com/ajrumph/gamedj-firmware/main/esp32cam/GameWatchingDJ-1.3.1.bin"}
```

If the version is newer than what it's running, it downloads the `.bin`, installs it and restarts (only while on game audio).

Board: `esp32cam` (AI Thinker ESP32-CAM). The M5Stack Timer Camera is no longer supported.

Releases are built on the Mac with `zsh ~/GameDJ/flash.sh esp release` and published from Claude.
