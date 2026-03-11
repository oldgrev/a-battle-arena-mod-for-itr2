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

## NVG Commands

### nvg
Toggle night vision on/off.

```
nvg
```

### nvg_on / nvg_off
Explicitly enable or disable NVG.

```
nvg_on
nvg_off
```

### nvg_mode
Set NVG display mode.

```
nvg_mode <0|1|2|3|4>
```

- `0` = Fullscreen — camera PP covers both eyes. Best working state for full-screen NVG.
- `1` = LensBlackout — camera PP (both eyes) + MI_PP_NightVision weight toggle (right-eye-only stereo bug, legacy)
- `2` = **LensOverlay** — mesh-based NVG lens renders in BOTH eyes; normal view outside lens.
- `3` = **LensMeshBlackout** — mesh-based NVG lens in BOTH eyes; blacked out outside lens.
- `4` = GameNVGOnly — pure `SetNV(true)` delegation, no camera PP. For isolated stereo testing.

**Modes 2 and 3 are the primary mesh-based lens modes.** They spawn a scene capture actor, render the world with NVG post-processing into a render target, then display it on a lens mesh attached to the VR camera. This avoids the stereo shader bug entirely.

### nvg_intensity
Set NVG brightness multiplier (0.1 – 5.0, default 1.0).

```
nvg_intensity <value>
```

### nvg_grain
Set film grain / noise amount (0.0 – 1.0, default 0.3).

```
nvg_grain <value>
```

### nvg_bloom
Set bloom intensity for light sources (0.0 – 10.0, default 2.0).

```
nvg_bloom <value>
```

### nvg_aberration
Set chromatic aberration / edge distortion (0.0 – 5.0, default 1.0).

```
nvg_aberration <value>
```

### nvg_status
Print current NVG status.

```
nvg_status
```

### nvg_diag
Run comprehensive NVG diagnostics — dumps camera PP, blendables, BPC component, TidePPMesh, and MobilePostProcessSubsystem state.

```
nvg_diag
```

### nvg_lens
Toggle the mesh-based NVG lens system on/off (manual control, independent of mode). Calls SetupLens or TeardownLens.

```
nvg_lens
```

### nvg_lens_setup / nvg_lens_teardown
Force setup or teardown of the lens system.

```
nvg_lens_setup
nvg_lens_teardown
```

### nvg_lens_fov
Set the capture FOV for the lens scene capture (10–170 degrees, default 90).

```
nvg_lens_fov <value>
```

### nvg_lens_scale
Set the lens mesh scale (0.01–10.0, default 0.25). Controls how large the lens circle appears.

```
nvg_lens_scale <value>
```

### nvg_lens_dist
Set the lens mesh distance from the camera (1–200 units, default 20). Controls depth/position.

```
nvg_lens_dist <value>
```

### nvg_lens_res
Set the render target resolution (64–4096, default 512). Requires teardown+setup to take effect.

```
nvg_lens_res <value>
```

### nvg_lens_mat
Set a scalar parameter on the lens MID (M_Lens material). Available parameters include: `Rim Scale`, `RimDepth`, `GridScale`, `GridDepth`, `ImageScale`, `BrightnessMult`, `ImageDepth`, `FocusDistanceOffset`, `OuterRimDepth`, `OuterRimScale`.

```
nvg_lens_mat <paramName> <value>
nvg_lens_mat BrightnessMult 2.0
nvg_lens_mat ImageScale 1.5
nvg_lens_mat Rim Scale 0.7
```

### nvg_lens_mat_type
Switch the lens material type. Type 0 = M_Lens (circular scope material with rim mask). Type 1 = M_Particle (simple translucent material, square view, no artifacts). Triggers a full lens teardown and rebuild.

```
nvg_lens_mat_type <0|1>
nvg_lens_mat_type 0   # M_Lens circular (default)
nvg_lens_mat_type 1   # M_Particle simple translucent
```

### nvg_probe
Discover the game's NVG blendable material by calling SetNV(true), reading WeightedBlendables before/after, then calling SetNV(false). Logs all findings. Required for lens modes.

```
nvg_probe
```

### nvg_pp_element
Directly call SwitchPPElement on the BPC_PlayerPostProcess component for experimentation. Use with `nvg_blendables` to see what changes.

```
nvg_pp_element <index> <0|1>
```

- `index` = PP element index to toggle
- `0` = turn off, `1` = turn on

### nvg_blendables
Dump the current WeightedBlendables array from the VR camera's PostProcessSettings.

```
nvg_blendables
```

### nvg_game_on / nvg_game_off
Directly call `SetNV(true)` or `SetNV(false)` on the game's `BPC_PlayerPostProcess` component, bypassing mod camera PP entirely. Useful for isolated stereo testing of `MI_PP_NightVision`.

```
nvg_game_on
nvg_game_off
```

### nvg_dump_mat_params
Dump all scalar, vector, and texture parameters of the `MI_PP_NightVision` material instance. Reveals every exposed parameter including any stereo/lens controls. Run `nvg_probe` first to populate the material pointer.

```
nvg_dump_mat_params
```

### nvg_mobile_dump
Dump the current `UMobilePostProcessSubsystem` NV-relevant fields (`NightVisionFactor`, `VignetteRadius`, etc). Run before and after `nvg_game_on` to verify whether `SetNV` modifies these on PC.

```
nvg_mobile_dump
```

### nvg_create_mid / nvg_remove_mid
Create a `UMaterialInstanceDynamic` (MID) from `MI_PP_NightVision` and add it as an additional camera blendable. MIDs allow runtime parameter modification. Hypothesis: may behave differently to the static `MaterialInstanceConstant` for stereo. Requires `nvg_probe` first.

```
nvg_create_mid
nvg_remove_mid
```

### nvg_mid_param / nvg_mid_vec
Set a scalar or vector parameter on the currently active NVG MID. Use `nvg_dump_mat_params` to find parameter names. Requires `nvg_create_mid` or `nvg_mid_fix` first.

```
nvg_mid_param <paramName> <value>
nvg_mid_vec <paramName> <r> <g> <b> <a>
```

### nvg_dump_all
Dump scalar/vector/texture parameters for ALL 6 camera blendable materials (not just MI_PP_NightVision). Use to discover useful params on VisionPP, M_LowHealth, etc.

```
nvg_dump_all
```

### nvg_mobile_w
Write a float field on UMobilePostProcessSubsystem directly. Cheap experiment to see if forcing NV values on PC has any visual effect.

```
nvg_mobile_w <field> <value>
```

**Fields:** `nvfactor`, `vigradius`, `nvilmin`, `nvilmax`, `nvolmin`, `nvolmax`, `sat`, `con`, `blink`, `fade`

**Examples:**
```
nvg_mobile_w nvfactor 1        # Force NightVisionFactor = 1
nvg_mobile_w vigradius 0.5     # Set VignetteRadius = 0.5
```

### nvg_mid_fix
**CRITICAL TEST** — Create a MID from MI_PP_NightVision and REPLACE the existing blendable slot in-place (fixes TArray::Add failure from nvg_create_mid). Tests whether MID renders on BOTH eyes.

```
nvg_mid_fix [index]
```

**Parameters:**
- `index` - Blendable slot to replace (default: 4 = MI_PP_NightVision)

### nvg_mid_undo
Restore the original object in the MID-replaced blendable slot. Undo for `nvg_mid_fix`.

```
nvg_mid_undo
```

### nvg_pp_dump
Dump current camera PostProcessSettings field values (ColorGain, VignetteIntensity, AutoExposureBias, BloomIntensity, etc).

```
nvg_pp_dump
```

### nvg_pp_set
Set a camera PostProcessSettings field directly (sets both override bit and value). For rapid VR iteration.

```
nvg_pp_set <field> <value>
```

**Fields:** `vig`, `aeb`, `bloom`, `grain`, `fringe`, `toe`, `slope`, `green`, `red`, `blue`, `sharp`, `ppw`, `temp`

**Examples:**
```
nvg_pp_set vig 5         # Set VignetteIntensity = 5
nvg_pp_set green 3       # Set ColorGain.Green = 3
nvg_pp_set aeb -2        # Set AutoExposureBias = -2
```

### nvg_blend
Set weight on any blendable by index. Activate/deactivate any of the 6 post-process materials.

```
nvg_blend <index> <weight>
```

**Examples:**
```
nvg_blend 1 1     # Activate VisionPP (index 1)
nvg_blend 1 0     # Deactivate VisionPP
nvg_blend 4 1     # Activate MI_PP_NightVision manually
```

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
