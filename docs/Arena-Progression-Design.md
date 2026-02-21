# Arena Progression Design

This document describes the step-by-step player experience through arena waves, the events that trigger at each stage, and the mod actions performed throughout.

---

## Table of Contents
- [Overview](#overview)
- [Arena Session Flow](#arena-session-flow)
- [Stage-by-Stage Breakdown](#stage-by-stage-breakdown)
- [Stage Gates and Triggers](#stage-gates-and-triggers)
- [Mod Actions Timeline](#mod-actions-timeline)
- [Difficulty Scaling](#difficulty-scaling)
- [Implementation Status](#implementation-status)

---

## Overview

The arena mod transforms Into The Radius 2 into a wave-based combat training experience. The goal is to help players:

1. **Develop Muscle Memory** - Repeated combat encounters build VR handling skills
2. **Learn Weapons** - Progression through different weapon types
3. **Understand Enemies** - Face enemy variants in controlled conditions
4. **Track Improvement** - Scoring provides feedback on skill development

### Core Loop
```
Arena Start → Wave Spawns → Combat → Wave Clear → 
Score/Rewards → Shop Phase → Next Wave → ...
```

---

## Arena Session Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                        ARENA SESSION                            │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐                                               │
│  │ ENTRY PHASE │ ← Player activates arena                      │
│  └──────┬──────┘                                               │
│         │                                                       │
│         ▼                                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                   WAVE CYCLE (Repeats)                   │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐ │   │
│  │  │  Spawn   │→ │  Combat  │→ │  Clear   │→ │  Shop    │ │   │
│  │  │  Phase   │  │  Phase   │  │  Phase   │  │  Phase   │ │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────┘ │   │
│  │       ▲                           │                      │   │
│  │       └───────────────────────────┘ (Loop)               │   │
│  └─────────────────────────────────────────────────────────┘   │
│         │                                                       │
│         ▼ (Player exits or completes max waves)                │
│  ┌─────────────┐                                               │
│  │ EXIT PHASE  │ → Final score, session stats                  │
│  └─────────────┘                                               │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## Stage-by-Stage Breakdown

### 1. Entry Phase

**Player Experience**:
- Player activates arena via command or in-game trigger
- Loadout selection (if implemented)
- Starting gear spawned
- Countdown to first wave

**Mod Actions**:
| Action | Implementation |
|--------|----------------|
| Disable hunger/fatigue | `Cheats::SetHungerDisabled(true)` |
| Enable god mode (optional) | `Cheats::ToggleGodMode()` |
| Clear existing NPCs | `AISubsystem::ClearNPCs()` |
| Initialize wave counter | `ArenaSubsystem::Start()` |
| Display entry message | `ModFeedback::ShowMessage()` |

**Feedback**:
```
"Arena Mode Active"
"Wave 1 starting in 3... 2... 1..."
```

---

### 2. Wave Spawn Phase

**Player Experience**:
- Brief countdown/warning
- Enemies spawn at configured distance (default 300 units)
- Enemies spread in semi-circle around player
- Visual/audio spawn indicators

**Mod Actions**:
| Action | Timing | Implementation |
|--------|--------|----------------|
| Generate wave GroupID | Pre-spawn | `currentWaveGroupID_ = 100 + wave_` |
| Calculate spawn positions | Pre-spawn | Distribute around player forward vector |
| Spawn each NPC | During spawn | `AISubsystem::SpawnNPC()` |
| Set NPC GroupID | Post-spawn | Write to offset `0x09B0` |
| Force combat state | Post-spawn | `SetNPCState(ENPCState::Combat)` |
| Start role suppression | Post-spawn | Block `RequestAttackRole` for 10s |
| Display wave message | End of spawn | `ModFeedback::ShowMessage()` |
| Start wave timer | End of spawn | `waveStartTime_ = now` |
| Reset wave stats | End of spawn | Damage=0, Bullets=0 |

**Spawn Distribution**:
```
Player at center, facing forward (0°)

Enemy positions for 5 NPCs:
  [-40°] [−20°] [0°] [+20°] [+40°]
     ↓      ↓     ↓     ↓      ↓
   NPC1   NPC2  NPC3  NPC4   NPC5

All at configured distance (e.g., 300 units)
```

**Feedback**:
```
"Wave 3 - 5 Enemies"
"Mimics incoming!"
```

---

### 3. Combat Phase

**Player Experience**:
- Active combat with spawned enemies
- Enemies initially seek cover (10s attack suppression)
- After 10s, enemies begin aggressive attacks
- As wave progresses, enemies become more aggressive (planned)
- Player stats accumulate (damage taken, bullets fired)

**Phase Sub-Stages**:

#### 3a. Grace Period (0-10 seconds)
- Attack role requests blocked
- NPCs prioritize movement and cover-seeking
- Player has time to assess situation

#### 3b. Active Combat (10s+)
- Attack role suppression lifted
- NPCs begin attacking
- Normal AI coordination resumes

#### 3c. Aggression Ramp (Future)
- After 30s: Reduce cover time
- After 60s: Force aggressive pushes
- After 90s: Add reinforcement spawns (?)

**Mod Actions During Combat**:
| Action | Trigger | Implementation |
|--------|---------|----------------|
| Track bullets fired | Each shot | Hook `ServerShootProjectile`, increment counter |
| Track damage taken | Each heal | Accumulate pre-heal health delta |
| Check enemy count | Each tick | Poll active enemies list |
| Lift attack suppression | Timer reaches 10s | Disable `RequestAttackRole` block |
| Handle stuck enemies | Enemy unreachable 30s+ | Teleport or kill (planned) |
| Update aggression | Timer thresholds | Modify AI parameters (planned) |

**Feedback** (rate-limited):
```
"3 enemies remaining"
"Enemy down!"
```

---

### 4. Wave Clear Phase

**Player Experience**:
- Final enemy killed
- Wave completion fanfare
- Stats displayed
- Score calculated and shown
- Brief respite before shop phase

**Trigger**: All wave enemies confirmed dead (death hook required)

**Mod Actions**:
| Action | Implementation |
|--------|----------------|
| Stop wave timer | `waveDuration = now - waveStartTime` |
| Calculate wave score | Score formula (see below) |
| Record session stats | Accumulate to total |
| Display stats | `ModFeedback::ShowMessage()` |
| Restore player health | `player->Health = MaxHealth` |
| Increment wave counter | `wave_++` |
| Start shop countdown | `shopPhaseStart = now` |

**Score Calculation**:
```cpp
float efficiency = 1.0f - (bulletsFired / (enemyCount * 5.0f));
efficiency = std::max(0.5f, std::min(1.0f, efficiency));

float speedBonus = targetDuration / actualDuration;
speedBonus = std::max(0.5f, std::min(1.5f, speedBonus));

float damageModifier = 1.0f - (damageTaken / playerMaxHealth);
damageModifier = std::max(0.5f, damageModifier);

int score = (int)(baseScore * efficiency * speedBonus * damageModifier);
```

**Feedback**:
```
"Wave 3 Complete!"
"Time: 45s | Bullets: 28 | Damage Taken: 15"
"Score: 1,250 | Total: 3,800"
```

---

### 5. Shop Phase

**Player Experience**:
- Weapons and items spawn around player
- Price tags visible (world-space text)
- Player can grab items to "purchase" with points
- Timer counts down to next wave
- Player can skip timer if ready

**Duration**: 30-60 seconds (configurable)

**Mod Actions**:
| Action | Implementation |
|--------|----------------|
| Spawn shop items | `ItemSubsystem::SpawnItem()` at positions |
| Display prices | `DrawDebugString` at item locations |
| Track purchases | Hook item pickup, deduct points |
| Display countdown | Update timer display |
| Check skip trigger | Player input (planned) |
| Clear unpurchased | Remove items before next wave |

**Shop Item Layout**:
```
Player standing position

Behind player: Ammo refills
      [Pistol Ammo] [Rifle Ammo] [Shotgun Ammo]

Left side: Weapons
      [Pistol] [SMG] [Rifle]

Right side: Attachments
      [Scope] [Suppressor] [Grip]

Front: Special items
      [Medkit] [Bullet Time] [Shield]
```

**Feedback**:
```
"Shop Phase - 30 seconds"
"Points: 3,800"
"Next wave in 15 seconds..."
"Grab items to purchase!"
```

---

### 6. Wave Transition

**Player Experience**:
- Shop items despawn
- Brief preparation countdown
- Next wave begins (return to Spawn Phase)

**Mod Actions**:
| Action | Implementation |
|--------|----------------|
| Clear shop items | `ClearItems()` for shop-spawned actors |
| Display countdown | `ModFeedback::ShowMessage()` |
| Update difficulty | Scale enemy params for next wave |
| Loop to Spawn Phase | `ArenaSubsystem::StartWave()` |

---

### 7. Exit Phase

**Player Experience**:
- Arena ended (manual exit or max waves reached)
- Final session statistics displayed
- Rewards summary
- Return to normal gameplay

**Mod Actions**:
| Action | Implementation |
|--------|----------------|
| Stop arena | `ArenaSubsystem::Stop()` |
| Clear all NPCs | `AISubsystem::ClearNPCs()` |
| Display final stats | Comprehensive session summary |
| Restore normal settings | Re-enable hunger/fatigue if desired |
| Spawn final rewards | Based on total session score |

**Feedback**:
```
"Arena Complete!"
"Waves Cleared: 5"
"Total Score: 8,750"
"Best Wave: Wave 3 (2,100)"
"Total Enemies: 25"
"Accuracy: 68%"
```

---

## Stage Gates and Triggers

### Gate Conditions

| Gate | From | To | Condition |
|------|------|-----|-----------|
| Start Gate | Entry | Wave 1 | Player command OR trigger interaction |
| Wave Gate | Wave N | Wave N+1 | All enemies dead |
| Shop Gate | Combat | Shop | Wave cleared |
| Skip Gate | Shop | Next Wave | Player ready trigger OR timer expires |
| Exit Gate | Any | Exit | Player command OR max waves |

### Trigger Types

| Trigger | Type | Status |
|---------|------|--------|
| TCP Command | External | ✅ Implemented |
| Timer Expiry | Automatic | ⏳ Planned |
| Death Hook | Event-based | ❌ Blocked (no death signal) |
| Player Input | In-game | ❌ Not implemented |
| Physical Prop | In-game | ❌ Not implemented |

### Blocked Triggers (Require Implementation)

**Death Hook**: Critical for automatic wave progression
- Need to discover exact function name via ProcessEvent logging
- Candidates: `OnDeath`, `K2_OnDeath`, health-change functions
- Discovery: Log all ProcessEvent on `ARadiusAICharacterBase` instances

**Player Input**: Required for in-game control
- Cheat panel button hooks
- Wrist menu integration
- Physical prop triggers

---

## Mod Actions Timeline

### Complete Wave Cycle (Target Implementation)

```
Time    Action                              Module
─────────────────────────────────────────────────────────
t=0     Wave N begins                       ArenaSubsystem
t=0     Spawn countdown "3...2...1..."      ModFeedback
t=3     Spawn NPCs at positions             AISubsystem
t=3     Set GroupID on each NPC             ArenaSubsystem
t=3     Force Combat state                  AISubsystem
t=3     Display "Wave N - X Enemies"        ModFeedback
t=3     Start attack role suppression       HookManager
t=3     Reset wave stats                    ArenaSubsystem
t=3     Start wave timer                    ArenaSubsystem
│
│       [Combat Phase]
│
t=13    Lift attack role suppression        HookManager
t=??    Track bullets (per shot)            HookManager
t=??    Track damage (per hit)              Cheats
│
│       [Last Enemy Dies]
│
t=X     OnNPCDeath detected                 HookManager
t=X     Decrement enemy count               ArenaSubsystem
t=X     Check: count == 0?                  ArenaSubsystem
t=X     Stop wave timer                     ArenaSubsystem
t=X     Calculate score                     ArenaSubsystem
t=X     Display stats                       ModFeedback
t=X     Restore player health               Cheats
│
│       [Shop Phase]
│
t=X+2   Spawn shop items                    ItemSubsystem
t=X+2   Display prices                      ModFeedback
t=X+2   Start shop timer (30s)              ArenaSubsystem
│
│       [Player shops / timer expires]
│
t=X+32  Clear shop items                    ItemSubsystem
t=X+32  Display countdown                   ModFeedback
t=X+35  Start Wave N+1                      ArenaSubsystem
        (Loop to t=0)
```

---

## Difficulty Scaling

### Wave Progression Table

| Wave | Enemy Count | Enemy Types | Spawn Distance | Aggression |
|------|-------------|-------------|----------------|------------|
| 1 | 3 | 1 type (Mimic) | 400 units | Low |
| 2 | 4 | 1 type (Mimic) | 350 units | Low |
| 3 | 5 | 1-2 types | 300 units | Medium |
| 4 | 6 | 2 types | 300 units | Medium |
| 5 | 7 | 2-3 types | 250 units | High |
| 6+ | 8+ | Mixed | 200 units | Very High |

### Enemy Type Difficulty

| Type | Difficulty | Notes |
|------|------------|-------|
| Basic Mimic | 1.0 | Standard encounter |
| Armed Mimic | 1.5 | Ranged attacks |
| Policeman Mimic | 2.0 | Tactical, uses cover |
| Fragment | 2.5 | Fast movement, dash ability |

### Aggression Modifiers

| Time in Wave | Aggression Level | Behavior |
|--------------|------------------|----------|
| 0-10s | Suppressed | Cover-seeking only |
| 10-30s | Normal | Standard AI |
| 30-60s | Elevated | Reduced cover time |
| 60s+ | Aggressive | Active pushing |

### Difficulty Subsystem Integration

Future implementation can leverage `URadiusDifficultySettingsSubsystem`:
```cpp
// Scale enemy parameters per wave
difficultySubsystem->SetEnemyHealth(baseHealth * waveMultiplier);
difficultySubsystem->SetEnemyDamageModifier(baseDamage * waveMultiplier);
difficultySubsystem->SetEnemySpotDistanceModifier(baseSpot * (1.0f + waveBonus));
```

---

## Implementation Status

### Fully Implemented ✅
- TCP command interface
- Manual wave spawning
- NPC spawn and GroupID assignment
- Combat state forcing
- Attack role suppression (first 10s)
- God mode / damage tracking
- Unlimited ammo / durability bypass
- HUD feedback messages

### Partially Implemented ⏳
- Arena subsystem structure (needs death hook)
- Stat tracking infrastructure (needs triggers)
- Wave configuration

### Not Implemented ❌
- Automatic wave progression
- Death detection hook
- Score calculation and display
- Shop phase spawning
- Between-wave shop mechanics
- Purchase tracking
- In-game player triggers
- Difficulty scaling
- Aggression ramp
- Stuck enemy handling
- Session statistics
- Custom arena level

### Critical Blockers

1. **Death Hook** - #1 priority for full arena functionality
2. **In-Game Input** - Required for headset-only control
3. **Wave Timer** - Requires death hook for accurate timing

### Recommended Implementation Order

1. Death hook discovery and implementation
2. Wave completion detection
3. Automatic wave progression
4. Score calculation
5. Shop phase item spawning
6. In-game trigger mechanism
7. Difficulty scaling
8. Full scoring and rewards
