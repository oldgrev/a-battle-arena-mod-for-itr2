# Subsystems Reference

Detailed documentation for each major subsystem in the mod.

---

## ArenaSubsystem

**File:** `Mod/ArenaSubsystem.cpp/.hpp`

Wave-based combat manager that spawns and tracks enemy NPCs.

### Responsibilities

- Wave progression and enemy count tracking
- NPC spawning with intelligent positioning
- Anti-stuck detection and teleportation
- Proximity notifications
- Time-of-day locking
- Kill tracking and wave completion

### Key Methods

```cpp
// Start arena with wave size and spawn distance
void Start(int enemiesPerWave, float distance);

// Stop arena and clear all enemies
void Stop();

// Called every game tick
void Update(SDK::UWorld* world);

// Notify when NPC dies (called from HookManager)
void OnNPCDeath(SDK::AActor* actor);

// Get current arena state as string
std::string GetStatus() const;
```

### State Machine

```
        Start()
    ┌──────┬──────┐
    ▼      │      │
┌───────┐  │  ┌───────────┐
│Inactive│  │  │Pre-Spawning│
└───┬───┘  │  └─────┬─────┘
    │      │        │ Timer expires
    │      │        ▼
    │ Start│  ┌───────────┐
    │      │  │ Wave Active │◀────┐
    │      │  └─────┬─────┘      │
    │      │        │ All killed  │ Next wave
    │      │        ▼            │
    │      │  ┌───────────┐      │
    │      └──│Wave Cooldown├────┘
    │         └─────┬─────┘
    │               │ Stop()
    ▼               ▼
┌───────────────────────┐
│       Inactive         │
└───────────────────────┘
```

### Spawning Logic

1. **Pre-Spawn Phase**: During cooldown, spawn NPCs far away
2. **Wave Start**: Reposition pre-spawned NPCs into final ring
3. **Spawn Constraints**:
   - Avoid player's line of sight
   - Avoid "escape direction" hemisphere
   - Require valid ground trace
   - Minimum distance from player

```cpp
// Spawn validation chain
bool IsValidSpawnLocation(FVector candidate) {
    return HasGround(candidate)
        && !IsInEscapeDirection(candidate)
        && !IsInPlayerLoS(candidate)
        && !DoesNpcSeePlayer(candidate);
}
```

### Anti-Stuck System

| Condition | Elapsed Time | Action |
|-----------|--------------|--------|
| Immobile | 10+ seconds | Mark as stuck |
| Stuck | 10+ seconds | Teleport closer |
| Overdue | 120+ seconds | Cull (destroy) |

**Teleport Rules:**
- Never teleport while in player's view
- Minimum 3 seconds between teleports
- Maintain minimum distance from player

### NPC Discovery

NPCs are discovered at runtime and persisted to `npcs.txt`:

```cpp
void ScanForNPCs(UWorld* world) {
    // Iterate GObjects for ARadiusNPCCharacterBase
    // Extract full object paths
    // Save to npcs.txt
}
```

Format: Full Unreal object paths for reliable loading:
```
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C
```

---

## FriendSubsystem

**File:** `Mod/FriendSubsystem.cpp/.hpp`

Manages friendly NPC companions that follow the player.

### Responsibilities

- Spawn friendly NPCs from enemy pool
- Disable hostile perception
- Maintain following behavior
- Play spatial audio feedback
- Handle friend death sequences

### Key Methods

```cpp
// Spawn a friendly NPC near the player
std::string SpawnFriend(SDK::UWorld* world);

// Update all friends (called each tick)
void Update(SDK::UWorld* world);

// Check if actor is a friend
bool IsFriendActor(SDK::AActor* actor) const;

// Handle friend death
void OnActorDeath(SDK::AActor* actor);
```

### Friend Entry State

```cpp
struct FriendEntry {
    SDK::AActor* Actor;
    bool IsDead;
    
    // Timers for periodic behaviors
    float nextRepositionTime;
    float nextAmbientSoundTime;
    float nextEnemySpotTime;
    float nextIdleResetTime;
    float nextFollowTime;
    
    // Unstuck tracking
    FVector lastFollowPosition;
    int stuckFollowAttempts;
};
```

### Perception Disabling (3-Layer Approach)

1. **Perception Component**: `SetAllSensesEnabled(false)`
2. **Stimuli Clear**: `ForgetAll()`
3. **ProcessEvent Hook**: Drop player perception events for friend controllers

```cpp
// Hook registered in FriendSubsystem::Initialize
HookManager::RegisterNamedHook("OnPerceptionUpdated", [](obj, fn, parms, orig) {
    if (IsFriendController(obj)) {
        if (IsPlayerStimulus(parms)) {
            return true;  // Suppress
        }
    }
    return false;  // Allow
});
```

### Following Behavior

```
Every Tick:
┌────────────────────────────────────┐
│ For each friend:                   │
│   ├── Check distance to player     │
│   │   ├── > 2000 units: Follow     │
│   │   └── < 600 units: OK          │
│   ├── Check stuck state            │
│   │   └── Teleport if needed       │
│   └── Reset to Idle periodically   │
└────────────────────────────────────┘
```

### Audio Integration

Friends play spatial audio using the `SpatialAudio` system:

| Event | Sound Group | Trigger |
|-------|-------------|---------|
| Idle | `ambient` | Random interval (30s base) |
| Enemy spotted | `enemyspotted` | Proximity check |
| Death | `tragedy` | On death |

---

## Cheats

**File:** `Mod/Cheats.cpp/.hpp`

Quality-of-life cheat system.

### Cheat States

```cpp
class Cheats {
    std::atomic<bool> godModeActive_;
    std::atomic<bool> unlimitedAmmoActive_;
    std::atomic<bool> durabilityBypassActive_;
    std::atomic<bool> hungerDisabled_;
    std::atomic<bool> fatigueDisabled_;
    std::atomic<bool> bulletTimeActive_;
    std::atomic<float> bulletTimeScale_;
    std::atomic<bool> noClipActive_;
    std::atomic<bool> anomaliesDisabled_;
    std::atomic<bool> autoMagActive_;
    // ...
};
```

### Update Loop

```cpp
void Cheats::Update(UWorld* world) {
    auto* player = FindPlayer(world);
    
    if (godModeActive_) ApplyGodMode(player);
    if (hungerDisabled_ || fatigueDisabled_) ApplyStatsCheats(player);
    if (durabilityBypassActive_) ApplyDurabilityFix(world, player);
    if (bulletTimeActive_) UpdateHeldItemsDilation(world, bulletTimeScale_, player);
    if (anomaliesDisabled_) ApplyAnomalySuppression(world);
    if (portableLightScaleDirty_) ApplyPortableLightBrightness(world, player);
}
```

### Hook Integration

Some cheats require ProcessEvent hooks:

```cpp
// In HookManager ProcessEvent detour
if (unlimitedAmmoEnabled_ && IsWeaponFire(function)) {
    // Restore ammo after fire
}

if (durabilityBypassEnabled_ && IsDurabilityReduce(function)) {
    // Prevent durability loss
}
```

### Bullet Time Implementation

```cpp
void ToggleBulletTime() {
    bulletTimeActive_ = !bulletTimeActive_;
    bulletTimeDirty_ = true;
}

// In Update:
if (bulletTimeDirty_) {
    UGameplayStatics::SetGlobalTimeDilation(world, 
        bulletTimeActive_ ? bulletTimeScale_ : 1.0f);
    bulletTimeDirty_ = false;
}
```

**Special handling:** Player's held items get inverse dilation so hands move normally.

---

## LoadoutSubsystem

**File:** `Mod/LoadoutSubsystem.cpp/.hpp`

Equipment save/load system.

### Responsibilities

- Capture player equipment hierarchy
- Serialize to/from `.loadout` files
- Restore equipment by spawning items

### Data Structures

```cpp
struct LoadoutItem {
    std::string itemTypeTag;        // FGameplayTag as string
    std::string instanceUid;        // Unique ID for relationships
    std::string parentContainerUid; // Parent container
    float durability;
    std::vector<std::pair<std::string, int>> stackedItems;
    std::map<std::string, std::string> additionalData;
    struct Transform { ... };
    std::vector<LoadoutItem> attachments;
};

struct LoadoutData {
    std::string name;
    std::string timestamp;
    int version;
    std::vector<LoadoutItem> items;
};
```

### Capture Flow

```
CaptureLoadout(world, name)
    ├── Get RadiusContainerSubsystem
    ├── GetPlayersInventory()
    ├── For each URadiusItemDynamicData:
    │   ├── Extract item type tag
    │   ├── Extract durability
    │   ├── Extract stacked items
    │   ├── Extract transform
    │   └── Recursively capture attachments
    ├── Build LoadoutData
    └── Serialize to file
```

### File Format

```ini
[LOADOUT]
name=myloadout
timestamp=2026-02-23 18:04:42
version=1
item_count=44

[ITEM]
type=Item.Equipment.Tablet
uid=Item.Equipment.Tablet-1
parent=Player-Holster.Player.Tablet
durability=100
transform=0,0,0|0,0,0|1,1,1
data_AutoreturnHolster=Player-Holster.Player.Tablet
[/ITEM]

[ITEM]
...
[/ITEM]
```

### Apply Flow

```
ApplyLoadout(world, name)
    ├── Load and parse file
    ├── Validate LoadoutData
    └── For each item:
        ├── Look up FGameplayTag from string
        ├── SpawnItemByTypeTag()
        ├── Set durability
        ├── Set stacked items
        ├── Position in container/transform
        └── Spawn attachments recursively
```

---

## VRMenuSubsystem

**File:** `Mod/VRMenuSubsystem.cpp/.hpp`

VR controller-based menu system.

### Responsibilities

- Track VR input state (grip, buttons)
- Navigate menu hierarchy
- Execute menu actions
- Render menu to screen

### Input Handling

```cpp
// Registered hooks
HookManager::RegisterNamedHook("IA_Button2_Left", OnButtonBY);
HookManager::RegisterNamedHook("IA_Grip_Left", OnGripPressed);
HookManager::RegisterNamedHook("IA_UnGrip_Left", OnGripReleased);
```

### Menu State

```cpp
class VRMenuSubsystem {
    std::atomic<bool> menuOpen_;
    std::atomic<bool> gripHeld_;
    MenuPage currentPage_;
    int selectedIndex_;
    NavDirection lastNavDirection_;  // For anti-rebound
    std::chrono::steady_clock::time_point lastInputTime_;
};
```

### Menu Pages

```cpp
enum class MenuPage {
    Main,
    Cheats,
    Arena,
    Loadouts,
    LoadoutSelect,
    SpawnFriend,
    FriendClass
};
```

### Navigation Anti-Rebound

```cpp
bool OnNavigate(float thumbstickY) {
    NavDirection newDir = GetDirection(thumbstickY);
    
    // Must return to center before going opposite direction
    if (lastNavDirection_ == UP && newDir == DOWN) {
        if (!passedCenter_) return true;  // Block
    }
    
    if (newDir == CENTER) {
        passedCenter_ = true;
    }
    
    // Process navigation
    ...
}
```

### Rendering

Two approaches implemented:

1. **Debug Strings**: Uses `DrawDebugString` - reliable but limited styling
2. **Widget**: Uses existing game widgets - experimental

```cpp
void Update(UWorld* world) {
    if (!menuOpen_) return;
    
    if (debugStringEnabled_) {
        RenderWithDebugString(world);
    }
    if (widgetEnabled_) {
        RenderWithWidget(world);
    }
}
```

---

## AISubsystem

**File:** `Mod/AISubsystem.cpp/.hpp`

NPC spawning and management.

### Key Methods

```cpp
SpawnResult SpawnNPC(UWorld* world, const SpawnParams& params);
int ClearNPCs(UWorld* world);
std::string ListNPCs(UWorld* world, size_t maxLines);
```

### Spawn Process

```cpp
SpawnResult SpawnNPC(world, params) {
    // Calculate spawn location
    FVector location = CalculateSpawnLocation(world, params.distance, params.lateralOffset);
    
    // Find class
    UClass* npcClass = FindClass(params.className, params.useFullName);
    
    // Spawn
    FActorSpawnParameters spawnParams;
    AActor* actor = world->SpawnActor(npcClass, location, rotation, spawnParams);
    
    // Register with game's NPC subsystem
    if (APawn* pawn = Cast<APawn>(actor)) {
        RegisterNPCWithSubsystem(world, pawn);
    }
    
    return SpawnResult{success, message, actor};
}
```

---

## ItemSubsystem

**File:** `Mod/ItemSubsystem.cpp/.hpp`

Item spawning and management.

### Key Methods

```cpp
SpawnResult SpawnItem(UWorld* world, const SpawnParams& params);
SpawnResult SpawnToInventory(UWorld* world, const SpawnParams& params);
int ClearItems(UWorld* world);
```

Similar structure to AISubsystem but for game items.

---

## HookManager

**File:** `Mod/HookManager.cpp/.hpp`

ProcessEvent interception via VTable patching.

### VTable Patching

```cpp
bool InstallProcessEventHook() {
    // Find a loaded UObject
    UObject* target = FindSuitableTarget();
    
    // Get VTable
    void** vtable = *reinterpret_cast<void***>(target);
    
    // ProcessEvent is typically at index ~77 (SDK-dependent)
    originalProcessEvent_ = vtable[PROCESS_EVENT_INDEX];
    
    // Replace with our detour
    DWORD oldProtect;
    VirtualProtect(&vtable[PROCESS_EVENT_INDEX], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect);
    vtable[PROCESS_EVENT_INDEX] = &ProcessEventDetour;
    VirtualProtect(&vtable[PROCESS_EVENT_INDEX], sizeof(void*), oldProtect, &oldProtect);
}
```

### Detour Function

```cpp
void ProcessEventDetour(UObject* obj, UFunction* fn, void* parms) {
    // Guard against recursion
    if (ScopedProcessEventGuard::IsActive()) {
        return originalProcessEvent_(obj, fn, parms);
    }
    
    // Named hooks
    std::string fnName = fn->GetName();
    if (auto hook = FindNamedHook(fnName)) {
        if (hook(obj, fn, parms, originalProcessEvent_)) {
            return;  // Hook handled, don't call original
        }
    }
    
    // Cheat hooks
    if (unlimitedAmmoEnabled_ && fnName == "Fire") {
        HandleUnlimitedAmmo(obj, parms);
    }
    
    // Tracing
    if (traceEnabled_ && MatchesFilter(fnName)) {
        LOG_INFO("[Trace] " << obj->GetName() << "." << fnName);
    }
    
    // Tick callback
    if (fnName == "Tick" && IsWorldObject(obj)) {
        ModMain::OnTick(GetWorld(obj));
    }
    
    // Call original
    originalProcessEvent_(obj, fn, parms);
}
```

---

## ModFeedback

**File:** `Mod/ModFeedback.cpp/.hpp`

Player feedback (messages, sounds).

### Message Display

```cpp
void ShowMessage(const wchar_t* text, float seconds, FLinearColor color) {
    // Uses UKismetSystemLibrary::PrintString
    // Queued during loading, replayed when safe
}
```

### Audio System

```cpp
// Game sound assets
bool PlaySoundAsset2D(const std::string& path, float volume, float pitch, bool isUi);

// Local media files (via UMediaPlayer)
bool PlayMediaFile2D(const std::string& path, bool loop, float volume);

// Sound groups (for friend ambient sounds, etc.)
bool PlayRandomSoundGroup2D(const std::string& group, bool loop, float volume);
```

---

## SpatialAudio

**File:** `Mod/SpatialAudio.cpp/.hpp`

Windows waveOut-based 3D audio (bypasses Unreal).

### Why Not Unreal Audio?

- More control over positioning
- Easier to debug
- Works with arbitrary .wav files
- Reliable attenuation model

### Distance Model

```cpp
Volume = 1.0                          (distance <= InnerRadius)
Volume = 1.0 - (d - In) / (Out - In)  (InnerRadius < distance < OuterRadius)
Volume = 0.0                          (distance >= OuterRadius)
```

### Low-Pass Filter

Distance-based muffling:

```cpp
alpha = Lerp(1.0, 0.05, distanceFactor);
y[n] = alpha * x[n] + (1 - alpha) * y[n-1];
```

### Usage

```cpp
SpatialAudio::Initialize();

SoundHandle h = SpatialAudio::Play3D(
    "sounds/ambient/idle.wav",
    sourcePos, listenerPos, listenerFwd, listenerRight,
    volume, loop);

// On tick:
SpatialAudio::SetSourcePosition(h, newSourcePos);
SpatialAudio::SetListenerState(playerPos, playerFwd, playerRight);
SpatialAudio::Tick();
```
