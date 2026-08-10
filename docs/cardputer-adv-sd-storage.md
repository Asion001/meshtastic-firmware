# Cardputer ADV SD configuration storage

This fork makes the Cardputer ADV microSD card the durable copy of Meshtastic runtime configuration while retaining internal LittleFS as a working copy and an offline fallback.

## Behavior

- On the first boot with a usable SD card, the current internal configuration is copied to `/meshtastic` on the card.
- On later boots, the SD configuration is restored to internal flash before `NodeDB` loads it.
- Successful writes to preferences and backups are mirrored to the SD card. Flash is marked authoritative before each atomic preference replacement, closing the reset/power-loss window before the SD copy; the complete tree is reconciled and the marker cleared on the next boot. Direct file mutations that cannot be mirrored safely in-place use the same recovery path.
- The SD filesystem is mounted only while restoring or mirroring data, then unmounted. Files are closed before unmounting and the shared SPI bus remains active for the LoRa radio. This releases the FAT/VFS/card heap while Bluetooth and the interface are running.
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

## New-node notifications

When the Cardputer first learns a node's identity, it can display a five-second `New node detected` banner. Under **System → Notifications → New Node Alerts**, **To all** alerts for every newly learned node, **Only this** alerts only for NodeInfo addressed directly to this Cardputer, and **Off** disables the banner. The active choice has a check mark and is also shown in the parent Notifications menu. This preference is stored in `/prefs/cardputer_adv_ui.bin` and is therefore mirrored to SD with the other settings. Existing Enabled/Disabled records are migrated automatically.
## Offline map screen

The Cardputer ADV build includes a dedicated offline map screen opened by pressing uppercase `M` from any normal screen. Lowercase `m` retains its global Messages shortcut, including while the map is open. The map uses the tile format from [lunarc3/CardputerGPSMap](https://github.com/lunarc3/CardputerGPSMap) and reads JPEG tiles from `/gpsmap/{zoom}/{x}/{y}.jpg` on the SD card. Generate the tile tree with that project's map converter, then copy the resulting `gpsmap` directory to the card. Without a GPS or saved map position, the screen centers itself on the first available tile and omits the user-position marker.

Use the arrow keys to pan, `Z`/`X` to zoom out/in, and the backtick key to return to the current Meshtastic GPS position. Press Backspace or Escape to return to the GPS frame. The last position and zoom are saved to `/gpsmap/gpsmap.ini`. Map tiles are rendered with ordered monochrome dithering to fit Meshtastic's memory-efficient Cardputer display pipeline.

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
