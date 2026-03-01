# Tuning Parameters

All configurable constants are centralized in `Mod/ModTuning.hpp`.

## TCP Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `kTcpDefaultPort` | 7777 | TCP server listening port |

---

## Arena Configuration

### Wave Settings

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaDefaultWaveSize` | 30 | Default enemies per wave |
| `kArenaDefaultSpawnDistance` | 10000.0 | Default spawn distance (units) |
| `kArenaMinSpawnDistance` | 8000.0 | Minimum allowed spawn distance |

### Timing

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaWaveCooldownSeconds` | 15.0 | Delay between waves |
| `kArenaNextWaveDelaySeconds` | 15.0 | Delay before first wave |
| `kArenaSpawnTickIntervalSeconds` | 0.5 | Time between spawn attempts |
| `kArenaScanIntervalSeconds` | 10.0 | NPC discovery scan interval |
| `kArenaMoveToPlayerIntervalSeconds` | 30.0 | NPC pressure movement interval |

### Pre-Spawning

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaPreSpawnExtraDistance` | 8000.0 | Extra distance for pre-spawn |
| `kArenaMinPreSpawnInterval` | 0.05 | Minimum ms between pre-spawns |

### Proximity Notifications

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaProximityNoticeStartSeconds` | 30.0 | Start notices after this into wave |
| `kArenaProximityNoticeIntervalSeconds` | 5.0 | Time between notices |

### Spawn Selection

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaSpawnMaxAttempts` | 10 | Max spawn location attempts |
| `kArenaCandidateSearchAttempts` | 6 | Candidates per attempt |
| `kArenaSpawnJitterXY` | 200.0 | Random XY offset for spawns |
| `kArenaNpcGroupId` | 101 | Group ID for arena NPCs |

### Ground Tracing

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaGroundTraceUp` | 1000.0 | Upward trace distance |
| `kArenaGroundTraceDown` | 3000.0 | Downward trace distance |
| `kArenaGroundSpawnZOffset` | 100.0 | Height above ground for spawn |

### Anti-Stuck System

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaStuckImmobileSeconds` | 10.0 | Time before considered stuck |
| `kArenaStuckTeleportCooldownSeconds` | 3.0 | Minimum time between teleports |
| `kArenaOverdueWaveSeconds` | 120.0 | Time before culling stuck NPCs |
| `kArenaTeleportMinDistanceFromPlayer` | 1000.0 | Min distance on teleport |
| `kArenaTeleportDistanceFactor` | 0.2 | Teleport step as distance fraction |
| `kArenaStuckMoveDistanceSq` | 100.0 | (10 units)² considered movement |
| `kArenaMimicScoutMinApproachDistance` | 2000.0 | Scout minimum approach |

### Line-of-Sight Avoidance

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaLoSVisibilityTraceEndZOffset` | 80.0 | Eye height for LoS checks |
| `kArenaLoSVisibilityHitNearEndDistance` | 120.0 | Near-hit threshold |
| `kArenaLoSHalfConeCos` | 0.5 | cos(60°) for 120° view cone |

### NPC-Sees-Player Avoidance

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaAvoidNpcSeeingPlayer` | true | Enable this safety rule |
| `kArenaNoSeePlayerGridSize` | 5 | 5×5 probe grid |
| `kArenaNoSeePlayerGridCellSize` | 500.0 | Cell size in units |
| `kArenaNoSeePlayerMaxRepeats` | 10 | Retry attempts |
| `kArenaNoSeePlayerTraceStartZOffset` | 80.0 | NPC eye height |

### Repositioning

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaRepositionDistanceStep` | 600.0 | Distance step for retries |
| `kArenaRepositionAngleStepDeg` | 65.0 | Angle step for retries |
| `kArenaTeleportRetryDistanceStep` | 750.0 | Teleport distance step |
| `kArenaTeleportRetrySideStep` | 800.0 | Teleport lateral step |

### AI Pressure

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaMaxConcurrentAttackers` | 100 | Max simultaneous attackers |
| `kArenaNpcMaxSenseDistance` | 10000.0 | NPC perception range |
| `kArenaNpcSoundProofMultiplierDefault` | 0.0 | Hearing sensitivity |
| `kArenaNpcDelayToStartReduceDetectionScale` | 0.0 | Detection delay |
| `kArenaNpcDetectReductionTime` | 0.1 | Detection reduction time |
| `kArenaNpcSuspiciousActivityLevel` | 1.0 | Suspicion threshold |
| `kArenaMoveToPlayerAcceptanceRadius` | 1000.0 | Movement goal radius |

### Pressure Pathing

| Constant | Default | Description |
|----------|---------|-------------|
| `kArenaPressureBehindPlayerDistance` | 900.0 | Behind-player target distance |
| `kArenaPressureFlankOffset` | 2000.0 | Flanking offset |
| `kArenaPressureRingRadius` | 1300.0 | Encirclement ring radius |
| `kArenaPressureLeapfrogFraction` | 0.55 | Leapfrog step fraction |
| `kArenaPressureLeapfrogMinStep` | 1000.0 | Minimum leapfrog step |
| `kArenaPressureLeapfrogMaxStep` | 2500.0 | Maximum leapfrog step |
| `kArenaPressureTargetJitterXY` | 500.0 | Target position jitter |

---

## Cheats Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `kBulletTimeMinScale` | 0.001 | Minimum bullet time scale |
| `kBulletTimeMaxScale` | 1.0 | Maximum bullet time scale |

---

## Commands Configuration

| Constant | Default | Description |
|----------|---------|-------------|
| `kCommandDefaultListMaxLines` | 10 | Max lines for list commands |

---

## AI Subsystem

| Constant | Default | Description |
|----------|---------|-------------|
| `kAISpawnGroundTraceUp` | 5000.0 | Upward trace distance |
| `kAISpawnGroundTraceDown` | 5000.0 | Downward trace distance |
| `kAISpawnGroundZOffset` | 120.0 | Height above ground |

---

## Friend NPC Configuration

### Limits

| Constant | Default | Description |
|----------|---------|-------------|
| `kFriendMaxCount` | 3 | Maximum simultaneous friends |

### Following

| Constant | Default | Description |
|----------|---------|-------------|
| `kFriendFollowDistanceMax` | 2000.0 | Max distance before following |
| `kFriendFollowDistanceMin` | 600.0 | Target minimum distance |
| `kFriendFollowDistanceTarget` | 1000.0 | Ideal following distance |
| `kFriendFollowAcceptanceRadius` | 1000.0 | Movement acceptance radius |
| `kFriendFollowCooldownBase` | 3.0 | Base cooldown between follows |

### Timers

| Constant | Default | Description |
|----------|---------|-------------|
| `kFriendRepositionIntervalBase` | 30.0 | Random reposition interval |
| `kFriendAmbientSoundIntervalBase` | 30.0 | Ambient sound interval |
| `kFriendEnemySpotCheckIntervalBase` | 60.0 | Enemy proximity check interval |
| `kFriendIdleResetIntervalSeconds` | 10.0 | Idle state reset interval |
| `kFriendDeathCleanupDelaySeconds` | 3.0 | Delay before death cleanup |

### Enemy Spotting

| Constant | Default | Description |
|----------|---------|-------------|
| `kFriendEnemySpotChance` | 0.60 | 60% chance per check |
| `kFriendEnemySpotRangeUnits` | 4000.0 | Detection range |

### Other

| Constant | Default | Description |
|----------|---------|-------------|
| `kFriendGroupId` | 200 | Group ID (separate from arena) |
| `kFriendJitterFraction` | 0.50 | ±50% timer jitter |
| `kFriendStuckTeleportFraction` | 0.20 | Stuck teleport step fraction |
| `kFriendCatchupDistanceThreshold` | 4000.0 | Distance for catchup teleport |
| `kFriendFollowGraceSeconds` | 8.0 | Post-follow idle grace period |

---

## Sound System

| Constant | Default | Description |
|----------|---------|-------------|
| `kDefaultSoundsFolder` | "sounds" | Sounds folder path |
| `kSoundGroupRetryCount` | 3 | Retries on sound failure |
| `kFriendAmbientSoundGroup` | "ambient" | Ambient sound group name |
| `kFriendEnemySpottedSoundGroup` | "enemyspotted" | Enemy spotted group |
| `kFriendTragedySoundGroup` | "tragedy" | Friend death group |

---

## Spatial Audio

### Distance Model

| Constant | Default | Description |
|----------|---------|-------------|
| `kSpatialInnerRadius` | 1000.0 | Full volume radius |
| `kSpatialOuterRadius` | 8000.0 | Inaudible beyond this |

### Low-Pass Filter

| Constant | Default | Description |
|----------|---------|-------------|
| `kSpatialLpfStartRadius` | 500.0 | LPF starts here |
| `kSpatialLpfEndRadius` | 8000.0 | LPF fully applied here |
| `kSpatialLpfAlphaMin` | 0.05 | LPF alpha at max distance |
| `kSpatialLpfAlphaMax` | 1.0 | LPF alpha at min distance |

### Output

| Constant | Default | Description |
|----------|---------|-------------|
| `kSpatialOutputGain` | 4.0 | Master output gain |
| `kSpatialPositionUpdateHz` | 10.0 | Position update rate |
| `kSpatialOutputSampleRate` | 48000 | Output sample rate |
| `kSpatialOutputChannels` | 2 | Stereo output |
| `kSpatialOutputBitsPerSample` | 16 | Bits per sample |
| `kSpatialBufferCount` | 4 | Ring buffer count |
| `kSpatialBufferDurationMs` | 40 | Buffer duration |

---

## Modifying Tuning Values

All constants are defined as `inline constexpr` for compile-time evaluation:

```cpp
// In ModTuning.hpp
namespace Mod::Tuning
{
    inline constexpr int kArenaDefaultWaveSize = 30;
    // Change to:
    inline constexpr int kArenaDefaultWaveSize = 50;
}
```

After changing, rebuild the DLL.

### Runtime Override (Not Implemented)

Currently all tuning is compile-time. A config file system could be added for runtime tuning without recompilation.
