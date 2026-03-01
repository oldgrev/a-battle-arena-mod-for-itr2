# VR Menu Guide

The Battle Arena Mod includes a VR-optimized menu system that allows you to access all features without using telnet.

## Accessing the Menu

### Toggle Menu Open/Close

**Hold Left Grip + Press B/Y Button**

The menu appears as text overlay in front of you.

### Navigation

| Action | Control |
|--------|---------|
| Open/Close Menu | Left Grip + B/Y |
| Navigate Up | Left Thumbstick Up |
| Navigate Down | Left Thumbstick Down |
| Select Item | B/Y (without grip) |

## Menu Structure

### Main Menu

The root menu provides quick access to common actions and sub-menus:

```
[MOD MENU]
> God Mode          [ON/OFF]
  Unlimited Ammo    [ON/OFF]
  Bullet Time       [ON/OFF]
  Arena             [→]
  Cheats            [→]
  Loadouts          [→]
  Friend            [→]
```

Items marked with `[→]` navigate to sub-menus.

### Cheats Menu

All cheat toggles:

```
[CHEATS]
> God Mode          [ON/OFF]
  Unlimited Ammo    [ON/OFF]
  Durability        [ON/OFF]
  Hunger            [ON/OFF]
  Fatigue           [ON/OFF]
  Bullet Time       [ON/OFF]
  Anomalies         [ON/OFF]
  Auto-Mag          [ON/OFF]
  Debug Mode        [ON/OFF]
  ← Back
```

### Arena Menu

Arena mode controls:

```
[ARENA]
> Start Arena
  Stop Arena
  Status
  ← Back
```

### Loadouts Menu

Equipment loadout management:

```
[LOADOUTS]
> Select Loadout    [→]
  Apply Selected
  Capture Current
  List Available
  ← Back
```

### Loadout Select

Choose from available loadouts:

```
[SELECT LOADOUT]
> g36               [●/○]
  sr25              [●/○]
  sweatymo          [●/○]
  ← Back
```

### Friend Menu

Friend NPC management:

```
[FRIEND]
> Spawn Friend
  Clear All Friends
  Status
  ← Back
```

## Visual Indicators

- `>` - Current selection cursor
- `[ON]` - Feature enabled (green)
- `[OFF]` - Feature disabled (gray)
- `[→]` - Navigates to sub-menu
- `[●]` - Selected loadout
- `[○]` - Unselected loadout

## Tips

### Anti-Rebound Protection

The menu implements anti-rebound logic to prevent accidental double-navigation:
- After pushing the thumbstick in a direction, you must return it to center before navigating in the opposite direction
- This prevents overshooting past your intended selection

### Menu Positioning

The menu renders using the game's debug string system:
- Text appears at a fixed screen position
- Should be readable in most lighting conditions
- Cyan/green color scheme for visibility

### Quick Toggles

For frequently used features:
1. Navigate to the item once
2. Use B/Y to toggle rapidly
3. Watch the status change in real-time

### Exiting Combat

If you need to access the menu during combat:
1. Find cover first
2. Hold grip to prepare
3. Press B/Y to open
4. Select your action quickly
5. Release grip or press B/Y to dismiss

## Troubleshooting

### Menu Not Appearing

- Ensure you're holding the LEFT grip
- Try pressing B/Y firmly
- Check if you're in a valid game level (not main menu)

### Navigation Not Working

- Ensure menu is open (visible)
- Move thumbstick fully up/down (past deadzone)
- Return stick to center between inputs

### Selection Not Working

- Release grip before pressing B/Y to select
- B/Y with grip held toggles the menu instead

### Text Hard to Read

- Move to an area with different lighting
- The menu uses fixed colors that may blend with some backgrounds

## Technical Notes

### Implementation

The VR menu uses two rendering approaches:
1. **Debug Strings**: DrawDebugString for reliable text rendering
2. **Widget System**: Experimental UMG-based approach (may not work in all scenarios)

Toggle rendering mode via commands:
```
poc9    # Enable widget-based rendering
```

### Input Hooks

The menu intercepts specific input actions:
- `IA_Button2_Left` - B/Y button
- `IA_Grip_Left` / `IA_UnGrip_Left` - Grip state
- Left thumbstick Y axis for navigation

These hooks only fire when the menu subsystem is active.
