# Technical Reference

This document consolidates SDK documentation, hooking architecture, and key technical details for the Into The Radius 2 mod.

---

## Table of Contents
- [ProcessEvent Hooking System](#processevent-hooking-system)
- [SDK Class Reference](#sdk-class-reference)
- [Key Data Structures](#key-data-structures)
- [TCP Command Reference](#tcp-command-reference)
- [ProcessEvent Logging](#processevent-logging)

---

## ProcessEvent Hooking System

### Overview

The mod intercepts gameplay logic via **manual VTable patching** of `UObject::ProcessEvent`. This provides a single entry point to intercept all engine-reflected calls (Blueprint functions, RPCs, etc.) without needing direct assembly offsets for individual functions.

### Why ProcessEvent?

Most ITR2 gameplay logic is Blueprint/reflection driven. Hooking `ProcessEvent` allows:
- Maximum flexibility to intercept any reflected function
- Robustness against game updates (VTable slots are more stable than function addresses)
- Centralized "named hook" registry for clean, modular extensions

### Implementation Architecture

**Files:**
- `Mod/HookManager.hpp` - Interface definition
- `Mod/HookManager.cpp` - Implementation

**Key Components:**

1. **VTable Discovery**: At initialization, scans `SDK::UObject::GObjects` to identify unique vtables and patches the `ProcessEventIdx` slot for each.

2. **Named Hook Registry**: Hooks are registered by `UFunction::GetName()` (e.g., `"ServerShootProjectile"`, `"TryExtractNextItem"`). A lock-free snapshot is used for O(1) dispatch in the hot path.

3. **Thread-Local Reentry Guard**: Prevents infinite recursion when hook handlers call SDK functions that would re-trigger `ProcessEvent`.

4. **Held-Weapon Gating**: Two-layer check to scope hooks to player-held items only:
   - **Grip Signal**: Direct check via `UGripMotionControllerComponent::GetIsHeld(actor)`
   - **Shot Context**: Thread-local marking of nested calls during a firing sequence

### Hook Dispatch Flow

```
ProcessEvent Called
    ↓
Early-out if cheats disabled
    ↓
Lookup function name in snapshot registry
    ↓
If match found → Call handler
    ↓
Handler returns true → Skip original
Handler returns false → Call original
```

### Reentry Guard Rules

**CRITICAL**: Only enable the guard when calling SDK maintenance functions (e.g., `ForceResyncFirearm`), NOT when calling the original function for gameplay events. Gameplay events rely on nested `ProcessEvent` calls.

```cpp
// CORRECT: Guard only SDK calls
{
    ScopedProcessEventGuard guard;
    component->InitializeMagazine();  // Guarded
}
originalFn(pThis, func, parms);       // NOT guarded - allows nested calls

// WRONG: Don't guard the gameplay call
{
    ScopedProcessEventGuard guard;
    originalFn(pThis, func, parms);   // Breaks nested ProcessEvent!
}
```

### Registering New Hooks

```cpp
// In HookManager::RegisterDefaultHooks()
RegisterNamedHook("FunctionName", [](UObject* obj, UFunction* func, void* parms, ProcessEventFn originalFn) -> bool {
    // Validate pointers
    if (!obj || !func) return false;
    
    // Apply held-weapon gating if needed
    if (!IsHeldContext(obj)) return false;
    
    // Your logic here
    
    // Return true = handled (skip original)
    // Return false = call original
    return true;
});
```

---

## SDK Class Reference

### Core AI System

#### ARadiusAICharacterBase
- **Base class**: AAlsCharacter (ALS animation system)
- **Purpose**: Base class for all NPCs
- **Key Methods**:
  - `OnDeath(AController* DeathInstigator, AActor* DiedPawn)` - Triggered on NPC death
  - `ChangeMoveSpeed(float NewSpeed)` - Dynamically adjust speed

#### ARadiusAIControllerBase
- **Base class**: AAIController
- **Purpose**: AI controller handling behavior, perception, coordination
- **Key Methods**:
  - `SetNPCState(ENPCState InNPCState)` - Force state: `Idle(1)`, `Suspicious(2)`, `Alarmed(3)`, `Combat(4)`
  - `GetNPCState()` - Returns current state
  - `UpdatePriorityTarget()` - Re-evaluate best target

#### URadiusAICoordinationSubsystem
- **Purpose**: Tracks and coordinates NPC behavior world-wide
- **Finding**: `SDK::UObject::FindObjectFast<SDK::URadiusAICoordinationSubsystem>("RadiusAICoordinationSubsystem")`
- **Key Methods**:
  - `RegisterNpc(ARadiusAIControllerBase* AIController)` - Register spawned NPC
  - `RequestAttackRole(...)` - Request permission to be aggressive (prevents swarming)
  - `GetAgentsInGroup(uint8 GroupID)` - Get NPCs in a specific group
  - `IsAnyoneFromGroupAttacking(...)` - Coordination check

#### URadiusDifficultySettingsSubsystem
- **Purpose**: Global difficulty modifiers
- **Key Methods**:
  - `GetEnemyHealth()` / `GetEnemyDamageModifier()`
  - `GetEnemySpotDistanceModifier()` / `GetEnemySpotTimeModifier()`
  - `GetEnemyHearingDistModifier()`

#### URadiusAIPerceptionComponent
- **Purpose**: NPC sensory input and target evaluation
- **Key Methods**:
  - `SetAllSensesEnabled(bool bActive)` - Toggle vision/hearing
  - `IsTargetVisible(AActor* Target)` - Line-of-sight check

### Specialized NPC Capabilities

**Mimics (`ABP_RadiusNPCCharacterMimicBase_C`)**:
- `Lean(ENPCLeanState LeanState, float Angle)` - Tactical leaning
- `LookOut(float Height)` - Peeking over obstacles
- `OnStartFire(bool bIsWhileMoving)` - Combat trigger

**Fragments (`ABP_RadiusNPCCharacterFragmentBase_C`)**:
- `DashToLocation(FVector Location)` - High-speed repositioning
- `Assemble()` / `Dissasemble()` - Spawn/despawn animations

### Firearm System

#### URadiusFirearmComponent
- **File**: `SDK/IntoTheRadius2_classes.hpp`
- **Purpose**: Core firearm functionality (shooting, reloading, malfunctions)
- **Key Methods**:
  | Method | Purpose |
  |--------|---------|
  | `ServerShootProjectile(FFirearmComponentShotExtendedRep& Rep)` | Primary server-side shooting function. Intercepted for unlimited ammo context. |
  | `TryExtractNextItem()` | Pull ammo from magazine. Intercepted for infinite ammo. |
  | `InitializeMagazine()` | Rebuild magazine state (used for visual resync). |
  | `DeliverAmmoFromMagToChamber()` | Move round from magazine to chamber. |
  | `SetWeaponCocked(bool bCocked)` | Control hammer/cocking state. |
  | `SetSliderLock(bool bLock, bool bForce)` | Control slide/bolt lock. |
  | `DamageDurabilityFromShot()` | Decrement weapon health on firing. |
  | `ChangeDurability(float Delta)` | General durability modification. |
  | `IsOutOfAmmo()` | Check if weapon is empty. |
  | `Debug_GetMagAmmoCount()` | Get current magazine count (debug). |
  | `HasAmmoInBarrel(bool* bIsShell)` | Check if round is chambered. |

- **Events**:
  - `OnBulletFired` - Fired when bullet is shot
  - `OnDysfunction` - Fired when weapon malfunctions
  - `OnShotFailed` - Fired when shot attempt fails

#### ARadiusFirearmBase
- **Base class**: ARadiusItemBase
- **Components**:
  - `WeaponMesh` - Skeletal mesh
  - `FirearmComponent` - URadiusFirearmComponent

### Player Classes

#### ABP_RadiusPlayerCharacter_Gameplay_C
- **Base class**: ARadiusPlayerCharacterGameplay → ARadiusPlayerCharacter → AVRCharacter
- **Purpose**: Concrete gameplay player character
- **Key Properties**:
  - `VRNotificationsComponent` - VR notifications
  - `GripControllerPrimary` / `GripControllerSecondary` - Hand controllers

### Item System

#### ARadiusItemBase
- **Purpose**: Base class for all items
- **Key for hooking**: Use to determine if actor is a held item via grip controller checks

#### UFLItems (Item Function Library)
- **Key Methods**:
  - `CreateNewDynamicData(...)` - Create new item data
  - `GetItemDurabilityRatio(AActor* Item)` - Get durability percentage

### Spawning Functions

#### UGameplayStatics
- `BeginDeferredActorSpawnFromClass(...)` - Start deferred spawn
- `FinishSpawningActor(AActor* Actor, FTransform Transform)` - Complete spawn
- `GetAllActorsOfClass(UWorld*, UClass*, TArray<AActor*>*)` - Find actors
- `GetPlayerController(UWorld*, int)` / `GetPlayerPawn(UWorld*, int)` - Get player

#### UKismetSystemLibrary
- `PrintString(...)` - HUD text overlay (used by ModFeedback)
- `DrawDebugString(...)` - 3D world text
- `OpenLevel(...)` / `LoadStreamLevel(...)` / `UnloadStreamLevel(...)` - Level management

---

## Key Data Structures

### FFirearmComponentShotExtendedRep
- **Purpose**: Data structure for shot information
- **Properties**:
  - `AmmoInBarrel` - Whether weapon thinks round is chambered
  - `AmmoTypeID` - Gameplay tag for round being fired

### FFirearmAmmoStateRep
- **Purpose**: Replicated ammo state
- **Notes**: Magazine and chamber ammo are tracked separately

### ENPCState
```cpp
enum class ENPCState : uint8 {
    Idle = 1,
    Suspicious = 2,
    Alarmed = 3,
    Combat = 4
};
```

---

## TCP Command Reference

Connect via telnet to port **7777**.

### Cheat Commands
| Command | Description |
|---------|-------------|
| `godmode` | Toggle god mode |
| `ammo` / `unlimitedammo` | Toggle unlimited ammo |
| `durability` | Toggle durability bypass |
| `bullettime [scale]` | Toggle bullet time (default 0.2 = 5x slow) |
| `hunger` | Toggle hunger disabled |
| `fatigue` | Toggle fatigue disabled |
| `cheats` | Show all cheat status |

### Arena Commands
| Command | Description |
|---------|-------------|
| `arena_config <class> [count] [dist]` | Configure arena parameters |
| `arena_start <class> [count] [dist]` | Start single wave |
| `arena_stop` | Stop arena |
| `arena_status` | Show arena status |

### NPC Commands
| Command | Description |
|---------|-------------|
| `spawn_npc <ClassName> [dist]` | Spawn NPC by short name |
| `spawn_npc_full <FullName> [d]` | Spawn NPC by full class name |
| `clear_npcs` | Remove all NPCs |
| `list_npcs [count]` | List current NPCs |
| `reinit_npc` | Reinitialize NPC system |

### Item Commands
| Command | Description |
|---------|-------------|
| `spawn_item <ClassName> [dist]` | Spawn item by short name |
| `spawn_item_full <FullName> [d]` | Spawn item by full class name |
| `clear_items` | Remove all items (except player) |
| `list_items [count]` | List current items |

### Utility Commands
| Command | Description |
|---------|-------------|
| `help` | Show available commands |

---

## ProcessEvent Logging

### Enabling Logs
Set environment variable before launching:
```cmd
set LOG_PROCESS_EVENT=1
```

### Log Location
```
C:\Users\<USERNAME>\AppData\Local\Temp\itr2_processevent_log.txt
```

### Output Format
```
[ProcessEvent] ObjectName -> FunctionName
```

### Performance Note
Logging has zero overhead when disabled (environment variable checked once at startup).

---

### NPCs Seen in Logs
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicPoliceman.BP_RadiusNPCCharacterMimicPoliceman_C
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicJaeger.BP_RadiusNPCCharacterMimicJaeger_C
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicAssault.BP_RadiusNPCCharacterMimicAssault_C
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicMarksman.BP_RadiusNPCCharacterMimicMarksman_C
- /Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicHeavy.BP_RadiusNPCCharacterMimicHeavy_C

## Terminology Quick Reference

| Term | Definition |
|------|------------|
| **ProcessEvent** | UE reflection dispatcher for Blueprint/RPC calls |
| **Named Hook** | Handler chosen by `UFunction::GetName()` |
| **Resync** | Calling `InitializeMagazine()` etc. to fix visual state |
| **Held-Weapon Gating** | Scope hooks to player-held items only |
| **Shot Context** | Thread-local marker for nested calls in firing chain |
| **VTable Patching** | Replacing function pointer in class virtual table |
