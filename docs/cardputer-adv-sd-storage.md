# Cardputer ADV SD configuration storage

This fork makes the Cardputer ADV microSD card the durable copy of Meshtastic runtime configuration while retaining internal LittleFS as a working copy and an offline fallback.

## Behavior

- On the first boot with a usable SD card, the current internal configuration is copied to `/meshtastic` on the card.
- On later boots, the SD configuration is restored to internal flash before `NodeDB` loads it.
- Successful writes to preferences and backups are mirrored to the SD card. Direct file mutations that cannot be mirrored safely in-place mark flash as newer so the complete tree is reconciled on the next boot.
- If the device boots without a usable card, it continues from internal flash and marks that copy as newer. Insert the card and reboot; the newer internal state is then copied to SD instead of being overwritten by stale SD data.
- A factory reset removes the mirrored preference and backup trees as well as their internal copies.

The SD layout is:

```text
/meshtastic/
├── .storage-v1
├── backups/
└── prefs/
```

The `.storage-v1` file identifies an initialized card. Do not edit files while the device is running. Insert or remove the card only while the Cardputer is powered off, then boot with the card installed.

## Security

The SD copy includes sensitive Meshtastic state, including channel PSKs and the device private key. A normal FAT-formatted microSD card does not protect these files at rest. Keep the card physically secure and erase it before reuse or disposal.

## Build

Use the official Cardputer ADV PlatformIO environment:

```sh
pio run -e m5stack-cardputer-adv
```

The build output is under `.pio/build/m5stack-cardputer-adv/`. Flash the firmware with the normal Meshtastic ESP32-S3 procedure or copy the update binary to the SD card for M5Launcher. An update flash preserves M5Launcher; factory/erase flashing does not.

## Recovery

To restore a saved configuration to another build of this fork, power off the device, insert the initialized SD card, and boot. If the destination already booted without a card, remove `/prefs/.sd-fallback-dirty` from its internal filesystem first or use a clean device; that marker deliberately makes internal flash win to avoid discarding changes made while the card was absent.
