# Features and Implementation Status

This document describes all mod features, their implementation status, and design details.

---

## Table of Contents
- [Overview](#overview)
- [Implemented Features](#implemented-features)
- [Planned Features](#planned-features)
- [UI and Feedback System](#ui-and-feedback-system)
- [Arena System](#arena-system)
- [Scoring and Rewards (Design)](#scoring-and-rewards-design)

---

## Overview

This mod for **Into The Radius 2** aims to create a wave-based combat arena that helps players develop VR combat muscle memory while learning the game's weapons, enemies, and mechanics. The mod is built as a DLL replacement (`version.dll`) using a Dumper-7 generated SDK.

### Core Goals
1. **Combat Training Arena** - Wave-based enemy spawning with progressive difficulty
2. **Power-Ups/Cheats** - Skills useful for testing and potential in-game power-ups
3. **Focused Experience** - Disable distractions like hunger/fatigue for pure combat
4. **Progression System** - Scoring, rewards, and item purchasing between rounds
5. **Player Feedback** - Visual information and interactivity within VR
6. **Modes** - Different Arena Wave modes
  a. Endless Survival - Infinite waves with increasing difficulty and no breaks until player fails. Every kill grants health/ammo/items.
  b. Sniper hunted - Sniper Spawns and hunts the player. Every minute another sniper spawns. Maybe it's dark, maybe the player needs to hunt a mcguffin.
  c. Glass Cannon - Player can only take one hit before failing the wave, but has a fully auto loadout and only 10 enemies in the wave.
  d. Reload training - Lots of enemies, manual reload, but no need to fill the magazines yourself.
  e. One shot one kill - Player has a mosin and only 10 bullets. Every enemy kill gives you one more bullet. Waves vary between close range enemies and long range snipers.
  f. the legend of teabagger vance - Enemies don't fully die until you teabag them. I don't know, I'm immature, but at least it's a different concept.
  g. Constant spawn - Every 5 seconds, a new enemy spawns until the player fails.
  h. Lizard hunt - gimmic mode, there's a lizard in the world somewhere. It comes out every so often, peforms some feat bizarre feat of masochism and skill, and then has to go back to it's day job. The player has to find it and kill it before it goes back to work. Maybe it's noisy(itr1 mimic scream? itr1 slider sound?), and will path to a position within 100m of the player and then run off. Maybe it can be tempted out with bait.
  i. Bosses? - buffed up versions of existing enemies, sometimes with escorts.
  j. Assasins - You have to kill a high value target on the map.  They have low perception and hearing, and there's no squadsight, but if they do see you they have aimbot vision and will call in reinforcements.
  h. Custom - Player can configure wave parameters like enemy type, count, spawn distance, and modifiers for a personalized challenge.
7. **Custom Levels** - Potential for custom arena maps or modifications to existing levels for better combat flow
8. **No, not everything here is being implemented.** - It's all subject to time, feasibility and interest. It's also a moving target for a game that's still in early access.

---

## Implemented Features

### Unlimited Ammo ✅
**Status**: Fully implemented via ProcessEvent hooking

**How it works**:
- Hooks `ServerShootProjectile` to capture ammo type and mark "held firing" context
- Hooks `TryExtractNextItem` to return captured ammo tag without depleting magazine
- Resync gating prevents visual desync (only resyncs when safe: mag > 1, chamber loaded)
- Scoped to held weapons only via grip controller checks

**Toggle**: TCP command `ammo` or `unlimitedammo`

---

### Durability Bypass ✅
**Status**: Fully implemented via ProcessEvent hooking

**How it works**:
- Hooks `DamageDurabilityFromShot` and `ChangeDurability`
- Returns `true` (handled) for held weapons, preventing wear and malfunctions
- Periodic durability restoration (every 600 frames) restores weapon health
- Scoped to held weapons only

**Toggle**: TCP command `durability`

---

### Bullet Time ✅
**Status**: Fully implemented via time dilation

**How it works**:
- Sets global time dilation via `UGameplayStatics::SetGlobalTimeDilation`
- Compensates player with inverse `CustomTimeDilation` (e.g., world at 0.2x, player at 5.0x)
- Held items inherit player's time dilation for responsive handling
- VR head/hand tracking remains 1:1 with real-time (engine behavior)

**Formula**: If `GlobalTimeDilation = S`, then `Player.CustomTimeDilation = 1/S`

**Toggle**: TCP command `bullettime [scale]` (default 0.2 = 5x slow motion)

**Known Behaviors**:
- Projectiles slow down after leaving barrel (intentional "bullet time" effect)
- Audio may pitch down (matches slow-motion aesthetic)
- Thrown objects transition from player speed to world speed mid-air

---

### God Mode ✅
**Status**: Fully implemented via health locking

**How it works**:
- `Cheats::Update` runs every frame
- When god mode active, player health is locked at maximum
- Damage is recorded before healing (for arena stats tracking)

**Toggle**: TCP command `godmode`

**Arena Integration**: Records damage taken before healing for wave statistics.

---

### Hunger/Fatigue Bypass ✅
**Status**: Fully implemented via stat modification

**How it works**:
- `ApplyStatsCheats` in `Cheats.cpp` resets hunger and fatigue to safe values each frame
- Prevents survival mechanics from interfering with combat focus

**Toggles**: 
- `hunger` - Toggle hunger disabled
- `fatigue` - Toggle fatigue disabled

**Future Potential**: Could invert these to *increase* hunger/fatigue for hardcore survival mode (low priority).

---

### Item Spawning ✅
**Status**: Working via ItemSubsystem

**Capabilities**:
- Spawn items by class name at specified distance from player
- Clear all items in world (except player)
- List current items

**Commands**:
```
spawn_item <ClassName> [distance]
spawn_item_full <FullClassName> [distance]
clear_items
list_items [count]
```

---

### NPC Spawning ✅
**Status**: Working via AISubsystem

**Capabilities**:
- Spawn NPCs by class name at specified distance
- Dynamic mimic class discovery from GObjects
- NPCs registered with coordination subsystem
- Force combat state on spawn

**Commands**:
```
spawn_npc <ClassName> [distance]
spawn_npc_full <FullClassName> [distance]
clear_npcs
list_npcs [count]
```

---

### Arena Subsystem ✅ (Basic)
**Status**: Single-wave manual spawning works

**Current Capabilities**:
- Configure wave parameters (enemy class, count, distance)
- Spawn single wave on demand
- Assign unique GroupID to wave NPCs (100-250)
- Force combat state on spawned enemies
- Attack role suppression for first 10 seconds (prevents immediate swarming)

**Limitations**:
- No automatic wave progression (requires reliable death signal)
- No wave timing tracked
- Manual spawning only

**Commands**:
```
arena_config <NpcClassName> [count] [distance]
arena_start <NpcClassName> [count] [distance]
arena_stop
arena_status
```

---

### Mod Feedback ✅
**Status**: Implemented via PrintString

**How it works**:
- Uses `UKismetSystemLibrary::PrintString` for HUD text overlay
- Shows colored text in top-left of VR view
- Confirms cheat toggles and command execution

**API**:
```cpp
Mod::ModFeedback::ShowMessage(L"Message text", duration, color);
```

---

### TCP Server ✅
**Status**: Running on port 7777

**Capabilities**:
- Accepts external commands via telnet
- Command parsing and dispatch via CommandHandler
- Real-time cheat toggling and arena control

---

## Planned Features

### Wave Progression System 🔄
**Status**: Not yet implemented - blocked on death signal

**Required**:
- Reliable NPC death detection (hook `OnDeath`, `K2_OnDeath`, or health-change functions)
- Actor validity (`IsValid`) does NOT reliably signal death (actors stay valid during death animation/ragdoll)

**Discovery Approach**: Log all `ProcessEvent` calls on `ARadiusAICharacterBase` instances to find death-related function names.

---

### Stat Tracking 🔄
**Status**: Infrastructure exists, awaiting wave system

**Planned Stats**:
| Stat | Implementation | Purpose |
|------|----------------|---------|
| Damage Taken | Accumulate heal delta during god mode | Skill feedback |
| Bullets Fired | Hook `ServerShootProjectile` counter | Efficiency metric |
| Wave Duration | Timer from wave start to clear | Speed feedback |
| Enemies Killed | Death hook counter | Completion tracking |

---

### Health/Ammo Refill on Kill 🔄
**Status**: Not implemented

**Design**: On confirmed NPC death, restore player health or add ammo to inventory.

---

### Enemy Difficulty Scaling 🔄
**Status**: Not implemented

**Planned Factors**:
- Number of enemies per wave
- Enemy awareness/accuracy modifiers (via URadiusDifficultySettingsSubsystem)
- Enemy type difficulty (Mimics vs Fragments)
- Mix of enemy types in later waves

---

### Enemy Aggressiveness Ramp 🔄
**Status**: Not implemented

**Design**: As wave duration increases, reduce attack role throttling to force enemies out of cover.

---

### Stuck Enemy Handling 🔄
**Status**: Not implemented

**Problem**: Enemies glitch under world or reach unreachable positions

**Potential Solutions**:
- Teleport to valid location
- Kill and respawn
- Add to wave completion requirements tolerance

---

### Player-Spawnable Allies 🔄
**Status**: Not implemented

**Concepts**:
- Friendly NPCs (mimics on player's side)
- Deployable turrets
- Spawnable cover objects

---

### Custom Arena Level 🔄
**Status**: Not implemented

**Design**: Load custom level or modify existing level with structures/cover for arena gameplay.

---

### In-Game Interaction 🔄
**Status**: Not implemented

**Current State**: Feedback works (PrintString), interaction does not.

**Potential Approaches** (in priority order):
1. **Cheat Panel Hooks** - Hook existing WBP_CheatPanel button events (dev mode only?)
2. **Wrist Menu Integration** - Hook wrist menu buttons
3. **Physical Props** - Spawn interactable items as triggers
4. **World-Space Widget** - Spawn UWidgetComponent attached to player

See [UI and Feedback System](#ui-and-feedback-system) for details.

---

## UI and Feedback System

### Current State

**Working**:
- `UKismetSystemLibrary::PrintString` - HUD overlay text ✅
- `UKismetSystemLibrary::DrawDebugString` - World-space floating text (available) ✅
- TCP server for external control ✅

**Not Working**:
- In-game player input/interaction ❌
- Arena HUD display ❌
- Settings/configuration UI ❌

### Feedback Implementation

```cpp
// ModFeedback.cpp
void ModFeedback::ShowMessage(const wchar_t* text, float seconds, FLinearColor color) {
    UObject* ctx = GameContext::GetPlayerCharacter();
    if (!ctx) return;
    
    SDK::FString msg(text);
    SDK::UKismetSystemLibrary::PrintString(ctx, msg, true, true, color, seconds, SDK::FName());
}
```

### Interaction Approaches

| Approach | Effort | Risk | Notes |
|----------|--------|------|-------|
| WBP_CheatPanel hooks | Low | Medium | May only work in dev mode |
| Wrist menu hooks | Medium | Low | Always accessible in VR |
| Physical trigger props | Medium | Low | Most immersive ("pick up radio to start wave") |
| World-space widget | High | Medium | Full custom HUD, needs widget class |
| Tablet integration | Medium | Medium | Games's existing UI system |

### Discovery Steps (for interaction)
1. Add temporary `ProcessEvent` logger for `UWBP_CheatPanel_C` objects
2. Log function names when buttons are clicked
3. Register named hooks for discovered function names

---

## Arena System

### Current Architecture

```
ArenaSubsystem (Singleton)
├── Configure() - Set wave parameters
├── Start() - Spawn single wave
├── Stop() - Halt arena
├── Update() - Tick-based logic
├── OnNPCDeath() - Death callback (awaiting hook)
└── Stats tracking (damage, bullets)
```

### Wave Spawn Process

1. Player sends `arena_start <class> [count] [distance]`
2. ArenaSubsystem generates unique GroupID (100-250)
3. For each enemy:
   - Calculate spawn position around player
   - Call `AISubsystem::SpawnNPC`
   - Set GroupID on spawned actor
   - Force `ENPCState::Combat`
4. Start attack role suppression timer (10 seconds)
5. Display wave start message

### GroupID System

Each wave gets a unique GroupID written directly to `ARadiusAICharacterBase` at offset `0x09B0`. This enables:
- Wave-specific behavior throttling
- Group coordination queries
- Distinguishing different wave NPCs

### Attack Role Suppression

During first 10 seconds of wave, `RequestAttackRole` hook denies attack roles to wave NPCs. This:
- Prevents immediate overwhelming
- Forces NPCs to prioritize movement and cover-seeking
- Creates more tactical engagement

---

## Scoring and Rewards (Design)

### Per-Wave Metrics

| Metric | Source | Scoring Impact |
|--------|--------|----------------|
| Damage Taken | God mode heal delta | Lower = better |
| Bullets Fired | Hook counter | Lower = more efficient |
| Wave Duration | Timer | Faster = better |
| Headshots | Future hook | Bonus points |
| Accuracy | Hits/Fired ratio | Efficiency bonus |

### Proposed Scoring Formula

```
Wave Score = Base × Efficiency × Speed × Skill
Where:
  Base = enemies_killed × enemy_difficulty
  Efficiency = max(0.5, 1 - (bullets_fired / (enemies_killed × 3)))
  Speed = max(0.5, 1 - (duration_seconds / target_seconds))
  Skill = 1 + headshot_bonus - damage_penalty
```

### Reward Tiers

| Tier | Requirement | Reward |
|------|-------------|--------|
| Bronze | Complete wave | Basic ammo refill |
| Silver | Score > 1000 | Weapon attachment |
| Gold | Score > 2000 + no damage | Premium weapon |

### Between-Wave Shop (Design)

**Concept**: After wave completion, spawn purchasable items around player

**Currency**: Wave completion rewards points usable for:
- Weapons
- Attachments (scopes, suppressors)
- Ammo types
- Healing items
- Special abilities (one-time bullet time, etc.)

**Implementation**: Use existing `ItemSubsystem::SpawnItem` with purchase validation logic.

---

## Feature Dependencies

```
Death Signal Hook ──┬── Wave Auto-Progression
                    ├── Kill-Based Rewards
                    ├── Wave Completion Detection
                    └── Accurate Enemy Counts

Wave System ────────┬── Duration Tracking
                    ├── Score Calculation
                    └── Between-Wave Shop

In-Game Input ──────┬── Arena Start/Stop
                    ├── Item Selection
                    └── Settings Configuration
```

### Critical Path to Full Arena

1. **Implement death hook** - Find and hook NPC death signal
2. **Wave progression** - Auto-start next wave on completion
3. **Score tracking** - Calculate and display wave scores
4. **In-game input** - Allow player control without headset removal
5. **Reward system** - Spawn purchasable items between waves
