# Loadout File Format

Technical specification for `.loadout` files used by the equipment save/restore system.

## Overview

Loadout files store a complete snapshot of player equipment in an INI-like text format. They capture all items, attachments, durability, stacked items, and spatial transforms.

## File Extension

`.loadout`

## File Location

```
<Game Directory>\IntoTheRadius2\Binaries\Win64\Loadouts\<name>.loadout
```

## Format Structure

```
[LOADOUT]
name=<loadout_name>
timestamp=<YYYY-MM-DD HH:MM:SS>
version=<format_version>
item_count=<total_items>

[ITEM]
type=<gameplay_tag>
uid=<unique_id>
parent=<parent_container>
durability=<0-100>
transform=<x,y,z|pitch,yaw,roll|sx,sy,sz>
[data_<key>=<value>]
[stacked=<tag:count,tag:count,...>]
[/ITEM]

[ITEM]
...
[/ITEM]
```

## Sections

### [LOADOUT] Section

Header section containing metadata.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `name` | string | Yes | Loadout name (matches filename) |
| `timestamp` | string | Yes | Capture time in `YYYY-MM-DD HH:MM:SS` |
| `version` | int | Yes | Format version (currently 1) |
| `item_count` | int | Yes | Total item count for validation |

### [ITEM] Section

Each item is wrapped in `[ITEM]` and `[/ITEM]` tags.

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | FGameplayTag as string (e.g., `Item.Weapon.AK74`) |
| `uid` | string | Yes | Unique instance ID for relationship tracking |
| `parent` | string | Yes | Parent container UID or holster name |
| `durability` | int | Yes | Durability percentage (0-100) |
| `transform` | string | Yes | Position, rotation, scale (see format below) |
| `data_*` | string | No | Additional key-value data |
| `stacked` | string | No | Stacked items for magazines/containers |

## Field Formats

### Gameplay Tag (type)

Format: `Item.Category.Subcategory.Name`

Examples:
```
Item.Equipment.Tablet
Item.Weapon.Rifle.AK74
Item.Equipment.Armor.Modular.Heavy.Sand
Item.Equipment.Modules.HolsterPistol_R_NoMolle.Sand
Item.Ammo.Rifle.762x39.AP
```

### Unique ID (uid)

Format: `<type>-<numeric_suffix>` or `<type>-<instance_id>`

Examples:
```
Item.Equipment.Tablet-1
Item.Equipment.Armor.Modular.Heavy.Sand-7262
Item.Weapon.Rifle.AK74-12345
```

The UID is used to establish parent-child relationships for attachments.

### Parent Container (parent)

Can be:
- Player holster: `Player-Holster.Player.<SlotName>`
- Item attachment slot: `<parent_uid>-AttachSlot.<SlotType>.<SlotName>`

Examples:
```
Player-Holster.Player.Tablet
Player-Holster.Player.Vest
Item.Equipment.Armor.Modular.Heavy.Sand-7262-AttachSlot.ModularVest.Slot3
```

### Transform

Format: `x,y,z|pitch,yaw,roll|sx,sy,sz`

- Position: x,y,z in centimeters (relative to parent)
- Rotation: pitch,yaw,roll in degrees
- Scale: sx,sy,sz (usually 1,1,1)

Examples:
```
0,0,0|0,0,0|1,1,1
-20.7838,-0.806406,11.8433|0,85.4294,-0|1,1,1
-2.0961e-13,-16.27,-20.2906|-1.95679e-14,-2.54444e-14,-1.13804e-14|1,1,1
```

Scientific notation is valid for small values.

### Stacked Items

Format: `<tag>:<count>,<tag>:<count>,...`

Used for magazines and ammo boxes to track loaded ammunition.

Example:
```
stacked=Item.Ammo.Rifle.545x39.FMJ:30
stacked=Item.Ammo.Pistol.9x19.HP:5,Item.Ammo.Pistol.9x19.FMJ:10
```

### Additional Data (data_*)

Any field starting with `data_` stores additional item properties.

Examples:
```
data_AutoreturnHolster=Player-Holster.Player.Tablet
data_SerialNumber=12345
data_CustomName=My Favorite Rifle
```

## Complete Example

```ini
[LOADOUT]
name=g36
timestamp=2026-02-23 18:04:42
version=1
item_count=3

[ITEM]
type=Item.Equipment.Tablet
uid=Item.Equipment.Tablet-1
parent=Player-Holster.Player.Tablet
durability=100
transform=0,0,0|0,0,0|1,1,1
data_AutoreturnHolster=Player-Holster.Player.Tablet
[/ITEM]

[ITEM]
type=Item.Weapon.Rifle.G36
uid=Item.Weapon.Rifle.G36-5001
parent=Player-Holster.Player.RifleRight
durability=95
transform=0,0,0|0,0,0|1,1,1
[/ITEM]

[ITEM]
type=Item.Magazine.Rifle.G36.30
uid=Item.Magazine.Rifle.G36.30-5002
parent=Item.Weapon.Rifle.G36-5001-AttachSlot.Magazine
durability=100
transform=0,0,0|0,0,0|1,1,1
stacked=Item.Ammo.Rifle.556x45.FMJ:30
[/ITEM]
```

## Parsing Algorithm

### Load

1. Read file line by line
2. Parse `[LOADOUT]` section for metadata
3. For each `[ITEM]...[/ITEM]` block:
   - Parse required fields
   - Parse optional fields
   - Create LoadoutItem structure
4. Validate `item_count` matches parsed items

### Apply

1. For each item in root-first order:
   - Look up FGameplayTag from `type`
   - Spawn item using game's spawn system
   - Set durability
   - Load stacked items if present
   - Store UID → spawned actor mapping
2. For items with attachments:
   - Look up parent actor from UID mapping
   - Attach to appropriate slot
   - Apply transform

## Error Handling

| Error | Cause | Behavior |
|-------|-------|----------|
| Missing required field | Malformed item block | Skip item, continue |
| Invalid gameplay tag | Unknown item type | Log warning, skip item |
| Missing parent | Parent item not spawned | Log warning, skip item |
| Version mismatch | Outdated format | Attempt compatibility, warn |

## Compatibility

### Version 1 (Current)

Initial format as documented.

### Future Versions

When `version > 1`:
- New required fields may be added
- Existing fields remain compatible
- Unknown fields are ignored

## Creating Loadouts Programmatically

```cpp
// Build LoadoutItem
LoadoutItem item;
item.itemTypeTag = "Item.Weapon.Rifle.AK74";
item.instanceUid = "Item.Weapon.Rifle.AK74-" + GenerateUID();
item.parentContainerUid = "Player-Holster.Player.RifleRight";
item.durability = 1.0f;
item.transform.posX = 0; item.transform.posY = 0; item.transform.posZ = 0;
// ... rotation, scale ...

// Add attachments
LoadoutItem magazine;
magazine.itemTypeTag = "Item.Magazine.Rifle.AK74.30";
magazine.instanceUid = "Item.Magazine.Rifle.AK74.30-" + GenerateUID();
magazine.parentContainerUid = item.instanceUid + "-AttachSlot.Magazine";
magazine.durability = 1.0f;
item.attachments.push_back(magazine);

// Build LoadoutData
LoadoutData loadout;
loadout.name = "custom";
loadout.timestamp = GetTimestamp();
loadout.version = 1;
loadout.items.push_back(item);

// Serialize
SerializeToFile(loadout, "Loadouts/custom.loadout");
```

## Limitations

- **Transform Precision**: Small floating-point drift may occur on apply
- **Dynamic Items**: Some procedurally generated items may not restore correctly
- **Attachment Order**: Complex attachment hierarchies may require specific application order
- **Game Updates**: Item tags may change between game versions

## Best Practices

1. **Capture in a clean state**: Remove unwanted items before capture
2. **Test after capture**: Apply immediately to verify
3. **Backup loadouts**: Copy files before game updates
4. **Use descriptive names**: `assault_primary`, `stealth_loadout`, etc.
