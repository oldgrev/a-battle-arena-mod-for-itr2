# Command Reference

Complete list of all commands available via telnet (`telnet localhost 7777`).

## Syntax Conventions

- `<required>` - Required parameter
- `[optional]` - Optional parameter
- `0|1` - Boolean flag (0=off, 1=on, also accepts true/false, on/off, yes/no)

---

## General Commands

### help
Show all available commands.

```
help
```

---

## Cheat Commands

### god
Toggle god mode (invincibility).

```
god
```

### ammo
Toggle unlimited ammo.

```
ammo
```

### durability
Toggle durability bypass (items never degrade).

```
durability
```

### hunger
Toggle hunger disabled.

```
hunger
```

### fatigue
Toggle fatigue disabled.

```
fatigue
```

### bullettime
Toggle bullet time (slow motion).

```
bullettime [scale]
```

**Parameters:**
- `scale` - Time scale from 0.001 to 1.0 (default: 0.2 = 5x slow)

**Examples:**
```
bullettime        # Toggle with default 0.2 scale
bullettime 0.1    # Set 10x slow motion, then toggle
bullettime 0.5    # Set 2x slow motion, then toggle
```

### lights
Adjust portable light (flashlight/headlamp) brightness.

```
lights [scale] [fadeScale]
```

**Parameters:**
- `scale` - Intensity multiplier (default: 1.0)
- `fadeScale` - Fade distance multiplier (default: 1.0)

**Examples:**
```
lights            # Show current settings
lights 5          # 5x brighter
lights 10 2       # 10x brighter, 2x fade distance
```

### anomalies
Toggle anomaly suppression. Destroys existing anomalies and prevents new ones.

```
anomalies
```

### plants
Toggle persistent removal of plant/bush foliage instances in a radius around the player, or run a one-shot delayed cleanup.

```
plants
plants once [radius]
plants [radius] [intervalSec]
```

**Behavior:**
- Collects targets first, then waits 1 second before deleting.
- Validates targets again before delete to avoid deleting already-culled objects.

**Parameters:**
- `radius` - Removal radius in Unreal units (default: `4000`)
- `intervalSec` - Persistent collect interval seconds (default: `15`)

**Examples:**
```
plants              # Toggle persistent cleanup on/off at 4000 radius, 15s interval
plants once         # One-shot cleanup around player (4000 radius)
plants once 2500    # One-shot cleanup in 2500 radius
plants 5000 8       # Toggle persistent mode with 5000 radius, 8s interval
```

### trees
Toggle persistent removal of tree foliage instances in a radius around the player, or run a one-shot delayed cleanup.

```
trees
trees once [radius]
trees [radius] [intervalSec]
```

**Behavior:**
- Collects targets first, then waits 1 second before deleting.
- Validates targets again before delete to avoid deleting already-culled objects.

**Parameters:**
- `radius` - Removal radius in Unreal units (default: `4000`)
- `intervalSec` - Persistent collect interval seconds (default: `15`)

**Examples:**
```
trees              # Toggle persistent cleanup on/off at 4000 radius, 15s interval
trees once         # One-shot cleanup around player (4000 radius)
trees once 3500    # One-shot cleanup in 3500 radius
trees 6000 10      # Toggle persistent mode with 6000 radius, 10s interval
```

### foliage
Toggle persistent removal for `UFoliageInstancedStaticMeshComponent` instances, or run a one-shot delayed cleanup.

```
foliage
foliage once [radius]
foliage [radius] [intervalSec]
```

### grass
Toggle persistent removal for `UGrassInstancedStaticMeshComponent` instances, or run a one-shot delayed cleanup.

```
grass
grass once [radius]
grass [radius] [intervalSec]
```

### staticmeshcomponent
Toggle persistent removal for all `UInstancedStaticMeshComponent` instances, or run a one-shot delayed cleanup.

```
staticmeshcomponent
staticmeshcomponent once [radius]
staticmeshcomponent [radius] [intervalSec]
```

### automag
Toggle auto-refilling magazines in magazine pouches.

```
automag
```

### access
Set player access level for testing gated content.

```
access <level>
```

**Parameters:**
- `level` - Access level (0-3)

### debug
Toggle debug mode for extra log messages.

```
debug
```

### cheats
Show current status of all cheats.

```
cheats
```

### test
Quick setup: enable common cheats and spawn a friend.

```
test
```

Enables:
- God mode
- Unlimited ammo
- Hunger/fatigue disabled
- Anomalies disabled
- Auto-mag
- Spawns friend NPC
- Sets portable light scale to 10

---

## Arena Commands

### arena_start
Start the battle arena.

```
arena_start [count] [distance]
```

**Parameters:**
- `count` - Enemies per wave (default: 30)
- `distance` - Spawn distance in Unreal units (default: 10000, minimum: 8000)

**Examples:**
```
arena_start              # 30 enemies at 10000 units
arena_start 20           # 20 enemies at 10000 units
arena_start 50 15000     # 50 enemies at 15000 units
```

**Auto-enabled cheats:**
- God mode
- Unlimited ammo
- Hunger/fatigue disabled

### arena_stop
Stop the battle arena and clear enemies.

```
arena_stop
```

---

## Friend NPC Commands

### friend
Spawn a friendly NPC companion near the player.

```
friend
```

**Limits:** Maximum 3 simultaneous friends.

### friend_clear
Remove all friend NPCs.

```
friend_clear
```

### friend_status
Show current friend count and limit.

```
friend_status
```

---

## Loadout Commands

### capture
Capture current equipment to a loadout file.

```
capture [name]
```

**Parameters:**
- `name` - Loadout name (default: "default")

**Examples:**
```
capture            # Save as default.loadout
capture assault    # Save as assault.loadout
```

### loadouts
List all available loadouts.

```
loadouts
```

### loadout
Select a loadout for quick application.

```
loadout [name]
```

**Parameters:**
- `name` - Loadout to select (omit to show current selection)

**Examples:**
```
loadout            # Show selected loadout
loadout g36        # Select "g36" loadout
```

### apply
Apply a loadout (spawn saved equipment).

```
apply [name]
```

**Parameters:**
- `name` - Loadout to apply (omit to use selected loadout)

**Examples:**
```
apply              # Apply selected loadout
apply sniper       # Apply "sniper" loadout
```

---

## Diagnostic Commands

### trace_on
Enable ProcessEvent tracing for debugging.

```
trace_on [fnFilter] [objFilter]
```

**Parameters:**
- `fnFilter` - Function name substring filter (use "none" to clear)
- `objFilter` - Object name substring filter (use "none" to clear)

**Examples:**
```
trace_on                      # Trace everything (high volume!)
trace_on Input                # Trace functions containing "Input"
trace_on OnFire               # Trace firing-related functions
trace_on none PlayerCharacter # Trace all functions on PlayerCharacter objects
```

### trace_off
Disable ProcessEvent tracing.

```
trace_off
```

### hud_trace_on
Enable HUD/Widget event tracing.

```
hud_trace_on
```

### hud_trace_off
Disable HUD tracing.

```
hud_trace_off
```

### hud_trace_reset
Clear accumulated HUD trace data.

```
hud_trace_reset
```

### hud_trace_flush
Write HUD trace data to file.

```
hud_trace_flush
```

### hud_trace_status
Show HUD trace statistics.

```
hud_trace_status
```

### hud_trace_path
Show HUD trace file location.

```
hud_trace_path
```

---

## Menu POC Commands

Development commands for menu system prototypes. These may not function correctly in all situations.

| Command | Description |
|---------|-------------|
| `poc1` | Switch ingame menu |
| `poc2` | Attach widget to left hand |
| `poc3` | Show cheat panel |
| `poc4` | Spawn ingame menu actor |
| `poc5` | Use info panel |
| `poc6` | Spawn face-following menu |
| `poc7` | Create user widget |
| `poc8` | Spawn confirmation dialog |
| `poc9` | Create confirm widget on hand |

---

## Response Format

Commands return text responses:
- Success messages include relevant state information
- Error messages start with "Error:" or "Failed:"
- Status commands return formatted multi-line output

**Example Session:**
```
> help
Available commands:
  god
  ammo
  ...

> god
GodMode: ON | Ammo: OFF | Durability: OFF | ...

> arena_start 10
Arena active=true wave=1 enemies=10 ...
```
