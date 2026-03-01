# User Guide

Complete guide to using the Battle Arena Mod for Into The Radius 2.

## Connecting to the Mod

The mod runs a TCP server on port 7777 for command input. Connect using any telnet client:

```bash
telnet localhost 7777
```

Or use Windows Terminal, PuTTY, or any TCP client.

> **Security Note**: The TCP server has no authentication. Do not run on untrusted networks.

## VR Menu (Controller-Based)

For in-VR control without telnet, use the VR menu:

- **Toggle Menu**: Hold Left Grip + Press B/Y button
- **Navigate**: Left thumbstick Up/Down
- **Select**: B/Y button (without grip)

See [VR Menu Guide](vr-menu.md) for details.

---

## Battle Arena Mode

The core feature: wave-based combat against enemy NPCs.

### Starting the Arena

```
arena_start [count] [distance]
```

- `count` - Enemies per wave (default: 30)
- `distance` - Spawn distance in units (default: 10000, min: 8000)

**Examples:**
```
arena_start           # 30 enemies at 10000 units
arena_start 10        # 10 enemies at 10000 units
arena_start 50 15000  # 50 enemies at 15000 units
```

> **Note**: Starting the arena automatically enables god mode, unlimited ammo, and disables hunger/fatigue.

### Stopping the Arena

```
arena_stop
```

Stops all arena activity and clears spawned enemies.

### Arena Mechanics

#### Wave System
- Enemies spawn staggered to prevent hitching
- Wave completes when all enemies are eliminated
- 15-second cooldown between waves
- Automatic wave progression

#### Enemy Spawning
- NPCs spawn outside your line of sight
- Spawn locations avoid your "escape direction" (direction you were facing at wave start)
- Ground traces ensure valid placement

#### Anti-Stuck System
- NPCs immobile for 10+ seconds may be teleported closer
- NPCs stuck for 120+ seconds are culled
- Teleports only happen when NPC is not in your view

#### Proximity Warnings
After 30 seconds into a wave, you'll receive proximity notifications every 5 seconds showing the distance to the nearest enemy.

#### Time of Day
Arena locks time to midday for consistent visibility. Restored when arena stops.

### Available Enemy Types

The mod discovers NPCs from the game and saves them to `npcs.txt`:

| Enemy Type | Class Name |
|------------|------------|
| Scout | BP_RadiusNPCCharacterMimicScout |
| Policeman | BP_RadiusNPCCharacterMimicPoliceman |
| Jaeger | BP_RadiusNPCCharacterMimicJaeger |
| Assault | BP_RadiusNPCCharacterMimicAssault |
| Marksman | BP_RadiusNPCCharacterMimicMarksman |
| Heavy | BP_RadiusNPCCharacterMimicHeavy |

---

## Cheats

Toggle various quality-of-life cheats. All commands toggle the state unless otherwise noted.

### Combat Cheats

| Command | Description |
|---------|-------------|
| `god` | Toggle god mode (invincibility) |
| `ammo` | Toggle unlimited ammo |
| `durability` | Toggle durability bypass (items don't degrade) |
| `bullettime [scale]` | Toggle bullet time. Optional scale (0.001-1.0, default 0.2) |

### Survival Cheats

| Command | Description |
|---------|-------------|
| `hunger` | Toggle hunger disabled |
| `fatigue` | Toggle fatigue disabled |
| `anomalies` | Toggle anomaly suppression (destroys and prevents anomalies) |
| `automag` | Toggle auto-refilling magazines in mag pouches |

### Utility

| Command | Description |
|---------|-------------|
| `cheats` | Show current cheat status |
| `debug` | Toggle debug mode (extra log messages) |
| `access <level>` | Set access level (0-3) to test gated content |
| `lights [scale] [fadeScale]` | Adjust flashlight/headlamp brightness |

### Quick Setup

```
test
```

Enables a common set of cheats and spawns a friend NPC:
- God mode
- Unlimited ammo
- Hunger/fatigue disabled
- Anomalies disabled
- Auto-mag enabled
- Spawns one friend NPC
- Sets portable light scale to 10

---

## Friend NPC System

Spawn friendly AI companions that follow and fight alongside you.

### Commands

| Command | Description |
|---------|-------------|
| `friend` | Spawn a friend NPC near you |
| `friend_clear` | Remove all friend NPCs |
| `friend_status` | Show current friend count and limit |

### Friend Behavior

- **Following**: Friends stay within 600-2000 units of you
- **Combat**: Friends engage enemy NPCs naturally
- **Unstuck**: Friends immobile too long attempt to catch up
- **Max Count**: Up to 3 simultaneous friends

### Audio Feedback

Friends use spatial audio for immersion:
- **Ambient Sounds**: Random periodic sounds from the `ambient` sound group
- **Enemy Spotted**: Alert sounds when detecting enemies (from `enemyspotted` group)
- **Tragedy**: Sound played on friend death (from `tragedy` group)

Place audio files in appropriate subfolders of `sounds/`:
```
sounds/
├── ambient/
│   └── *.wav, *.mp3
├── enemyspotted/
│   └── *.wav, *.mp3
└── tragedy/
    └── *.wav, *.mp3
```

---

## Equipment Loadouts

Save and restore complete equipment configurations.

### Saving a Loadout

```
capture [name]
```

Saves your current equipment to a `.loadout` file:
- All items on your body (armor, weapons, backpack contents)
- Item durability
- Attachment configurations
- Stacked items (ammo in magazines)
- Positional transforms

**Examples:**
```
capture            # Save as "default.loadout"
capture mysetup    # Save as "mysetup.loadout"
```

### Listing Loadouts

```
loadouts
```

Shows all available loadouts in the `Loadouts` folder.

### Selecting a Loadout

```
loadout <name>
```

Selects a loadout for quick application.

### Applying a Loadout

```
apply [name]
```

Spawns all items from the loadout:
- If no name given, uses the selected loadout
- Items spawn in their saved positions

**Examples:**
```
apply              # Apply the selected loadout
apply g36          # Apply the "g36" loadout
```

### Loadout File Location

Loadouts are stored in:
```
<Game Directory>\IntoTheRadius2\Binaries\Win64\Loadouts\
```

---

## Diagnostics & Debugging

### ProcessEvent Tracing

For debugging game internals:

| Command | Description |
|---------|-------------|
| `trace_on [fnFilter] [objFilter]` | Enable ProcessEvent tracing |
| `trace_off` | Disable tracing |

Use `none` to clear a filter:
```
trace_on Input          # Trace functions containing "Input"
trace_on none Player    # Trace all functions on "Player" objects
trace_off               # Stop tracing
```

### HUD Tracing

Specialized tracing for UI/HUD events:

| Command | Description |
|---------|-------------|
| `hud_trace_on` | Enable HUD event tracing |
| `hud_trace_off` | Disable HUD tracing |
| `hud_trace_reset` | Clear accumulated data |
| `hud_trace_flush` | Write trace to file |
| `hud_trace_status` | Show trace statistics |
| `hud_trace_path` | Show trace file location |

### Menu POC Commands

Development/testing commands for menu system prototypes:

```
poc1 - poc9
```

These are proof-of-concept implementations and may not function correctly in all situations.

---

## Command Reference

Use `help` to see all available commands:

```
help
```

For detailed command syntax, see [Command Reference](commands.md).

---

## Tips & Best Practices

### Arena Combat
1. Start with smaller wave sizes (10-15) to learn enemy behavior
2. Use the VR menu for quick cheat toggling mid-combat
3. Listen for proximity warnings to track remaining enemies

### Loadouts
1. Capture loadouts before arena sessions
2. Apply loadouts to quickly reset between arena runs
3. Create multiple loadouts for different combat styles

### Performance
1. Large wave sizes (50+) may cause frame drops during spawn
2. The pre-spawn system staggers spawning to reduce hitching
3. Anti-stuck teleports are throttled to minimize visual pops

### Debugging
1. Check the log file for detailed error information
2. Use trace commands to understand game event flow
3. Enable debug mode for additional log output
