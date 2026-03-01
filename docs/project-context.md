# Project Context

Quick reference for essential project information.

## What This Is

A native C++ mod (DLL) for **Into The Radius 2** (ITR2), a VR survival shooter game. The mod adds:

- **Battle Arena Mode**: Wave-based combat against spawned enemies
- **Cheat System**: God mode, unlimited ammo, bullet time, etc.
- **Friend NPCs**: AI companions that follow and fight with you
- **Equipment Loadouts**: Save and restore equipment configurations
- **VR Menu**: Controller-based in-game menu

## Game Information

- **Game**: Into The Radius 2
- **Engine**: Unreal Engine (VR build)
- **Platform**: Windows (Steam)
- **Character Class**: `ABP_RadiusPlayerCharacter_Gameplay_C`
- **NPC Base Class**: `ARadiusNPCCharacterBase`

## Technical Stack

- **Language**: C++17
- **Build**: Visual Studio 2019/2022
- **SDK**: Dumper-7 generated SDK
- **Hook Method**: VTable patching (ProcessEvent)
- **Command Interface**: TCP server (port 7777)

## Key Paths

| Purpose | Location |
|---------|----------|
| DLL Output | `x64/Release/battlearena.dll` |
| Game Mods | `<Steam>/IntoTheRadius2/Binaries/Win64/Mods/` |
| Loadouts | `Win64/Loadouts/` |
| Sounds | `Win64/sounds/` |
| NPC Database | `Win64/npcs.txt` |
| Log File | `%USERPROFILE%/AppData/Local/ITR2Mod/mod.log` |

## Entry Points

| Function | Purpose |
|----------|---------|
| `DllMain` | DLL load/unload callbacks |
| `ModMain::Run()` | Main initialization and loop |
| `ModMain::OnTick()` | Per-frame updates (game thread) |
| `ProcessEventDetour()` | Hook entry point |
| `CommandHandlerRegistry::Handle()` | Command processing |

## Critical Constraints

1. **No UI in DllMain** - causes deadlock
2. **SDK calls only from game thread** - OnTick or hooks
3. **Use ScopedProcessEventGuard** - prevents recursion
4. **Clear caches on level change** - pointers go stale
5. **Use recursive_mutex** - SDK callbacks may re-enter

## Documentation Index

### For Users
- [README](README.md) - Overview and quick start
- [Installation](installation.md) - Setup guide
- [User Guide](user-guide.md) - Feature documentation
- [Commands](commands.md) - Command reference
- [VR Menu](vr-menu.md) - Controller navigation
- [Troubleshooting](troubleshooting.md) - Problem solving

### For Developers
- [Architecture](architecture.md) - System design
- [Subsystems](subsystems.md) - Component details
- [Hooks](hooks.md) - ProcessEvent system
- [Modding Guide](modding-guide.md) - Extension guide
- [AI Guidelines](ai-guidelines.md) - AI assistant rules

### Reference
- [Tuning](tuning.md) - Configuration parameters
- [NPCs](npcs.md) - Enemy database
- [Loadout Format](loadout-format.md) - File specification

## Quick Command Reference

| Command | Description |
|---------|-------------|
| `help` | List commands |
| `test` | Enable common cheats + spawn friend |
| `arena_start [n] [d]` | Start arena with n enemies at distance d |
| `arena_stop` | Stop arena |
| `god` | Toggle god mode |
| `ammo` | Toggle unlimited ammo |
| `friend` | Spawn friend NPC |
| `capture [name]` | Save equipment |
| `apply [name]` | Load equipment |
| `cheats` | Show cheat status |

## File Count Summary

| Category | Files |
|----------|-------|
| Mod Source | ~30 (.cpp/.hpp) |
| SDK | ~100+ (generated) |
| Documentation | 14 (.md) |
| Build Files | 5 (.sln, .vcxproj, etc.) |

## Version Info

- **Mod SDK Version**: 1.0.0
- **TCP Port**: 7777
- **Max Friends**: 3
- **Default Wave Size**: 30

## Repositories

- **Mod**: [a-battle-arena-mod-for-itr2](https://github.com/oldgrev/a-battle-arena-mod-for-itr2)
- **Loader**: [proxy-dll-for-loading-ITR2-mod](https://github.com/oldgrev/proxy-dll-for-loading-ITR2-mod)
- **SDK Source**: [5.5.4-0-oculus-5.5.4-release-1.110.0-v78.0-IntoTheRadius2](https://github.com/oldgrev/5.5.4-0-oculus-5.5.4-release-1.110.0-v78.0-IntoTheRadius2)
