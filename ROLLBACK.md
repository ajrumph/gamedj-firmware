# Rollback
Units update from esp32cam/manifest.json. To roll every unit back, edit that file so
"version" is HIGHER than the bad release (units only update to a newer number) and point
"url" at the known-good .bin. Known-good builds: 1.3.9 (scene-change burst), 1.3.8 (Wi-Fi memory).
Example: copy GameWatchingDJ-1.3.9.bin to GameWatchingDJ-1.4.1.bin and publish 1.4.1.

1.5.0 (LAN live stream, decimal intervals) released Nov 23 2026. Rollback: publish 1.5.1 pointing at a copy of GameWatchingDJ-1.4.0.bin (known good before the live stream).

## 1.5.1 (ESP32-CAM)
Adds Wi-Fi signal, restart reason, drop count and uptime to each check; Wi-Fi power 15 dBm (falls back to 11 dBm for one boot after an early-boot failure).
Rollback: publish a higher version number whose url points at esp32cam/GameWatchingDJ-1.5.0.bin (known good, 1.5.0 behavior).

## 1.5.8 (test rollback)
1.5.0 code rebuilt and labeled 1.5.8 to test whether the Eve dropouts come from later code or from her physical setup. 1.5.0 plus the unit-label bump only; no 1.5.1-1.5.7 changes.
