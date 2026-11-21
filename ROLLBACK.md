# Rollback
Units update from esp32cam/manifest.json. To roll every unit back, edit that file so
"version" is HIGHER than the bad release (units only update to a newer number) and point
"url" at the known-good .bin. Known-good builds: 1.3.9 (scene-change burst), 1.3.8 (Wi-Fi memory).
Example: copy GameWatchingDJ-1.3.9.bin to GameWatchingDJ-1.4.1.bin and publish 1.4.1.
