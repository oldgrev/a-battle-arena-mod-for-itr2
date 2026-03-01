# Installation Guide

This guide walks you through installing the Battle Arena Mod for Into The Radius 2.

## Requirements

- Into The Radius 2 (Steam version)
- Windows 10/11
- A telnet client (Windows Terminal, PuTTY, or similar) for command access

## Installation Steps

### Step 1: Download the Mod Loader

1. Go to the [Mod Loader Releases](https://github.com/oldgrev/proxy-dll-for-loading-ITR2-mod/releases)
2. Download `version.dll` from the latest release

### Step 2: Install the Mod Loader

1. Navigate to your ITR2 installation directory:
   ```
   <Steam Library>\steamapps\common\IntoTheRadius2\IntoTheRadius2\Binaries\Win64
   ```
2. Copy `version.dll` into this folder

### Step 3: Download the Battle Arena Mod

1. Go to the [Battle Arena Mod Releases](https://github.com/oldgrev/a-battle-arena-mod-for-itr2/releases)
2. Download `battlearena.dll` from the latest release

### Step 4: Install the Battle Arena Mod

1. Navigate to your ITR2 mod folder (create it if it doesn't exist):
   ```
   <Steam Library>\steamapps\common\IntoTheRadius2\IntoTheRadius2\Binaries\Win64\Mods
   ```
2. Copy `battlearena.dll` into the `Mods` folder

### Step 5: Verify Installation

1. Launch Into The Radius 2
2. Open a terminal/command prompt
3. Connect to the mod's TCP server:
   ```
   telnet localhost 7777
   ```
4. Type `help` to see available commands
5. If connected successfully, you'll see a list of commands

## Directory Structure After Installation

```
IntoTheRadius2/
└── IntoTheRadius2/
    └── Binaries/
        └── Win64/
            ├── version.dll          ← Mod loader
            └── Mods/
                └── battlearena.dll  ← This mod
```

## Optional: Custom Content

### Loadouts Folder

Create a `Loadouts` folder next to the game executable to store saved equipment loadouts:
```
Win64/
├── Loadouts/
│   ├── myloadout.loadout
│   └── ...
```

### Sounds Folder

Create a `sounds` folder for custom audio (friend ambient sounds, etc.):
```
Win64/
├── sounds/
│   ├── ambient/
│   │   ├── sound1.wav
│   │   └── sound2.mp3
│   ├── enemyspotted/
│   │   └── alert.wav
│   └── tragedy/
│       └── sad.wav
```

See [User Guide](user-guide.md) for details on sound groups.

## Uninstallation

1. Delete `version.dll` from `Win64/`
2. Delete the `Mods` folder (or just `battlearena.dll`)
3. Optionally delete `Loadouts` and `sounds` folders

## Troubleshooting

See [Troubleshooting](troubleshooting.md) for common issues.

### Can't Connect to Port 7777

- Ensure the game is running
- Check if another application is using port 7777
- Try restarting the game
- Check the mod log file for errors

### Mod Not Loading

- Verify `version.dll` is in the correct `Win64` folder
- Verify `battlearena.dll` is in the `Mods` subfolder
- Check that file names are exactly as shown (case-sensitive)
- Run the game as administrator if needed

### Log File Location

The mod creates a log file at:
```
%USERPROFILE%\AppData\Local\ITR2Mod\mod.log
```

Check this file for detailed error messages.
