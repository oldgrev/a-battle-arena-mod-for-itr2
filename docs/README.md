# Battle Arena Mod for Into The Radius 2

A comprehensive modding framework for Into The Radius 2 (ITR2) that adds a battle arena mode, cheats, friendly NPC companions, equipment loadouts, and a VR-optimized menu system.

## Documentation Index

### For Users
- [Installation Guide](installation.md) - How to install and set up the mod
- [User Guide](user-guide.md) - Complete guide to all features and commands
- [Command Reference](commands.md) - Full list of available commands
- [VR Menu Guide](vr-menu.md) - Using the VR controller menu system
- [Troubleshooting](troubleshooting.md) - Common issues and solutions

### For Developers
- [Architecture Overview](architecture.md) - System design and component overview
- [Subsystems Reference](subsystems.md) - Detailed documentation of each subsystem
- [Hook System](hooks.md) - ProcessEvent hooking and interception
- [Modding Guide](modding-guide.md) - How to extend and modify the codebase
- [AI Agent Guidelines](ai-guidelines.md) - Guidelines for AI coding assistants

### Quick Links
- [Project Context](project-context.md) - Quick reference summary
- [Tuning Parameters](tuning.md) - All configurable constants
- [NPC Database](npcs.md) - Available NPC types
- [Loadout Format](loadout-format.md) - Loadout file specification

## Features Overview

### Battle Arena Mode
Wave-based survival combat against progressively spawning enemies. Features include:
- Configurable wave size and spawn distance
- Intelligent NPC positioning (line-of-sight avoidance, escape direction blocking)
- Anti-stuck teleportation for NPCs
- Real-time kill notifications and proximity warnings
- Automatic time-of-day locking for consistent visibility

### Cheat System
Quality-of-life cheats for testing and casual play:
- God mode (invincibility)
- Unlimited ammo
- No hunger/fatigue
- Bullet time (slow motion)
- No-clip movement
- Enhanced flashlight brightness
- Anomaly suppression
- Auto-refilling magazines

### Friend NPC System
Spawn friendly AI companions that follow and fight alongside you:
- Up to 3 simultaneous companions
- Automatic following behavior
- Enemy detection and engagement
- Spatial audio feedback (ambient sounds, enemy spotting)

### Equipment Loadouts
Save and restore complete equipment configurations:
- Capture all equipped items with attachments
- Persists durability, stacked items, and transforms
- Multiple named loadout slots

### VR Menu System
Controller-based menu for in-VR command access:
- Grip + B/Y to toggle menu
- Thumbstick navigation
- Quick access to all cheats and features

## Quick Start

1. Install the mod (see [Installation Guide](installation.md))
2. Launch Into The Radius 2
3. Connect to the command server: `telnet localhost 7777`
4. Type `help` to see available commands
5. Try `test` to enable common cheats and spawn a friend
6. Try `arena_start` to begin a battle arena session

## Version

Current SDK Version: 1.0.0

## Links

- [Video Demo](https://youtu.be/z89-_Wb_13k)
- [Mod Releases](https://github.com/oldgrev/a-battle-arena-mod-for-itr2/releases)
- [Mod Loader Releases](https://github.com/oldgrev/proxy-dll-for-loading-ITR2-mod/releases)
