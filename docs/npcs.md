# NPC Database

Documentation of available NPC types for the arena and friend systems.

## Overview

NPCs are discovered at runtime by scanning loaded game assets. The discovered NPCs are persisted to `npcs.txt` for reliable spawning.

## Discovery Process

1. **Initial Scan**: When the arena starts, it scans `GObjects` for `ARadiusNPCCharacterBase` instances
2. **Path Extraction**: Full Unreal object paths are captured (e.g., `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_...`)
3. **Persistence**: Paths are saved to `npcs.txt` in the game directory
4. **Preservation**: Existing entries are preserved; file is only updated with new discoveries

## NPC Types

### Known NPC Classes

| Display Name | Class Name | Path |
|--------------|------------|------|
| Scout | BP_RadiusNPCCharacterMimicScout_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C` |
| Policeman | BP_RadiusNPCCharacterMimicPoliceman_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicPoliceman.BP_RadiusNPCCharacterMimicPoliceman_C` |
| Jaeger | BP_RadiusNPCCharacterMimicJaeger_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicJaeger.BP_RadiusNPCCharacterMimicJaeger_C` |
| Assault | BP_RadiusNPCCharacterMimicAssault_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicAssault.BP_RadiusNPCCharacterMimicAssault_C` |
| Marksman | BP_RadiusNPCCharacterMimicMarksman_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicMarksman.BP_RadiusNPCCharacterMimicMarksman_C` |
| Heavy | BP_RadiusNPCCharacterMimicHeavy_C | `/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicHeavy.BP_RadiusNPCCharacterMimicHeavy_C` |

### NPC Characteristics

| Type | Role | Notes |
|------|------|-------|
| Scout | Light enemy | Fast, aggressive, common |
| Policeman | Standard enemy | Balanced, reliable |
| Jaeger | Hunter | Often appears with Scouts |
| Assault | Offensive | Aggressive tactics |
| Marksman | Ranged | Engages at distance |
| Heavy | Tank | High durability |

## npcs.txt Format

Simple text file with one full object path per line:

```
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicPoliceman.BP_RadiusNPCCharacterMimicPoliceman_C
...
```

### Path Format

```
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C
└───────────────────────────────────────────────────────────────┬──────────────────────────────────┘
                          Asset Path                            │        Class Suffix (_C)
```

- Full path is required for reliable runtime loading
- `_C` suffix indicates compiled Blueprint class
- Path must be exact (case-sensitive)

### File Location

```
<Game Directory>\IntoTheRadius2\Binaries\Win64\npcs.txt
```

Or relative to game executable working directory.

## Spawning Logic

### Random Selection

The arena randomly selects from discovered NPCs:

```cpp
std::string GetRandomNPCClass() {
    auto& npcs = arenaSubsystem_->GetDiscoveredNPCs();
    if (npcs.empty()) return "";
    
    int index = std::rand() % npcs.size();
    return npcs[index];
}
```

### Class Loading

Blueprint classes are loaded using soft object path:

```cpp
FSoftClassPath softPath(fullPath);
UClass* npcClass = softPath.TryLoadClass<AActor>();

// Or using LoadClassAsset_Blocking
TSoftClassPtr<AActor> softRef(fullPath);
UClass* npcClass = softRef.LoadSynchronous();
```

### Spawn Parameters

```cpp
FActorSpawnParameters params;
params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

AActor* npc = world->SpawnActor(npcClass, &location, &rotation, params);
```

## Adding Custom NPCs

### Requirements

1. NPC must be a `ARadiusNPCCharacterBase` subclass
2. NPC must be loaded in memory (exists in a loaded level)
3. Full object path must be captured

### Manual Addition

Add paths directly to `npcs.txt`:

```
/Game/YourMod/NPCs/BP_CustomEnemy.BP_CustomEnemy_C
```

**Note:** The asset must exist and be loadable for this to work.

### Discovery Enhancement

To discover NPCs in specific levels:
1. Play the game and visit areas where NPCs spawn
2. The mod will automatically discover loaded NPCs
3. Check `npcs.txt` for newly added entries

## Friend NPC Selection

Friends use the same NPC pool but with perception disabled:

```cpp
std::string FriendSubsystem::SpawnFriend(UWorld* world) {
    // Pick random NPC class from arena's discovered list
    std::string npcClass = GetRandomNPCClass();
    
    // Spawn
    AActor* actor = SpawnNPC(world, npcClass);
    
    // Disable hostile perception
    ZeroSenses(actor);
    RegisterFriend(actor);
}
```

## Troubleshooting

### No NPCs Discovered

**Symptoms:** `npcs.txt` is empty or missing

**Solutions:**
1. Enter a game level with enemies (not shelter)
2. Encounter or see enemy NPCs
3. NPCs are discovered when loaded by the game
4. Start arena to trigger scan

### NPCs Not Spawning

**Symptoms:** Arena starts but no enemies appear

**Solutions:**
1. Check `npcs.txt` has entries
2. Verify paths are complete (not truncated)
3. Check log for loading errors
4. Some NPCs may require specific game state to load

### Wrong NPC Types

**Symptoms:** Only Scouts appear, no variety

**Solutions:**
1. Play the game normally to encounter different NPC types
2. Each type must be loaded at least once to be discovered
3. Check that `npcs.txt` has multiple unique entries

### Corrupted npcs.txt

**Symptoms:** Spawn errors, crashes on arena start

**Solutions:**
1. Delete `npcs.txt` to force re-discovery
2. Ensure no malformed paths (no partial lines)
3. Check for encoding issues (should be UTF-8 or ASCII)

## NPC Behavior in Arena

### AI State

Arena NPCs receive enhanced awareness:
- `MaxSenseDistance`: 10000 units
- Detection time reduced
- Group attack limit increased to 100

### Movement Pressure

Every 30 seconds, NPCs receive movement commands:
- Move toward player position
- Various pressure patterns (flank, ring, leapfrog)
- Target jittering prevents clustering

### After Death

```cpp
void ArenaSubsystem::OnNPCDeath(AActor* actor) {
    // Remove from tracking
    RemoveFromActiveEnemies(actor);
    
    // Update wave progress
    CheckWaveCompletion();
    
    // Show notification
    ShowKillMessage(remaining);
}
```
