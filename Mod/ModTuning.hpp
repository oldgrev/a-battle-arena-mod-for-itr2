#pragma once

#include <cstdint>

namespace Mod::Tuning
{
    // ---------------------------------------------------------------------
    // TCP
    // ---------------------------------------------------------------------
    inline constexpr std::uint16_t kTcpDefaultPort = 7777;

    // ---------------------------------------------------------------------
    // Arena
    // ---------------------------------------------------------------------
    inline constexpr int   kArenaDefaultWaveSize = 100;
    inline constexpr float kArenaDefaultSpawnDistance = 10000.0f;
    inline constexpr float kArenaMinSpawnDistance = 8000.0f;

    // Pre-spawn happens during wave cooldown; we spawn far away to avoid hitching,
    // then reposition into the final ring at wave start.
    inline constexpr float kArenaPreSpawnExtraDistance = 8000.0f;

    inline constexpr float kArenaWaveCooldownSeconds = 15.0f;
    inline constexpr float kArenaNextWaveDelaySeconds = 15.0f;

    // Pre-spawn interval guard: never spawn faster than this regardless of wave size.
    inline constexpr float kArenaMinPreSpawnInterval = 0.05f; // 50ms

    inline constexpr float kArenaSpawnTickIntervalSeconds = 0.5f;
    inline constexpr float kArenaScanIntervalSeconds = 10.0f;
    inline constexpr float kArenaMoveToPlayerIntervalSeconds = 30.0f;

    inline constexpr float kArenaProximityNoticeStartSeconds = 30.0f;
    inline constexpr float kArenaProximityNoticeIntervalSeconds = 5.0f;

    inline constexpr int   kArenaMaxConcurrentAttackers = 100;

    // Grounding / traces
    inline constexpr float kArenaGroundTraceUp = 1000.0f;
    inline constexpr float kArenaGroundTraceDown = 3000.0f;
    inline constexpr float kArenaGroundSpawnZOffset = 100.0f;

    // Spawn selection
    inline constexpr int   kArenaSpawnMaxAttempts = 10;
    inline constexpr int   kArenaCandidateSearchAttempts = 6;
    inline constexpr float kArenaSpawnAngleFallbackDivisor = 10.0f; // used when enemiesPerWave is 0
    inline constexpr float kArenaSpawnJitterXY = 200.0f;
    inline constexpr int   kArenaNpcGroupId = 101;

    // Anti-stuck
    inline constexpr float kArenaStuckImmobileSeconds = 10.0f;
    inline constexpr float kArenaStuckTeleportCooldownSeconds = 3.0f;
    inline constexpr float kArenaOverdueWaveSeconds = 120.0f;
    inline constexpr float kArenaTeleportMinDistanceFromPlayer = 1000.0f;
    inline constexpr float kArenaTeleportDistanceFactor = 0.2f;
    inline constexpr float kArenaStuckMoveDistanceSq = 100.0f; // (10 units)^2

    // LoS avoidance tuning (spawn/teleport)
    inline constexpr float kArenaLoSVisibilityTraceEndZOffset = 80.0f;
    inline constexpr float kArenaLoSVisibilityHitNearEndDistance = 120.0f;

    // Extra safety rule: NPCs should not be placed where they have line-of-sight to the player.
    // When enabled, placement will probe a 5x5 grid *behind the candidate location relative to the player*
    // (500x500 cells) and, if needed, re-center on a rear corner up to 10 times.
    inline constexpr bool  kArenaAvoidNpcSeeingPlayer = true;
    inline constexpr int   kArenaNoSeePlayerGridSize = 5;           // 5x5
    inline constexpr float kArenaNoSeePlayerGridCellSize = 500.0f;  // 500 units per cell
    inline constexpr int   kArenaNoSeePlayerMaxRepeats = 10;
    inline constexpr float kArenaNoSeePlayerTraceStartZOffset = 80.0f;

    // cos(60deg) = 0.5 -> 120deg full cone
    inline constexpr double kArenaLoSHalfConeCos = 0.5;

    // Candidate search steps
    inline constexpr float kArenaRepositionDistanceStep = 600.0f;
    inline constexpr float kArenaRepositionAngleStepDeg = 65.0f;
    inline constexpr float kArenaTeleportRetryDistanceStep = 750.0f;
    inline constexpr float kArenaTeleportRetrySideStep = 800.0f;

    // AI pressure / awareness
    inline constexpr float kArenaNpcMaxSenseDistance = 10000.0f;
    inline constexpr float kArenaNpcSoundProofMultiplierDefault = 0.0f;
    inline constexpr float kArenaNpcDelayToStartReduceDetectionScale = 0.0f;
    inline constexpr float kArenaNpcDetectReductionTime = 0.1f;
    inline constexpr float kArenaNpcSuspiciousActivityLevel = 1.0f;
    inline constexpr float kArenaMoveToPlayerAcceptanceRadius = 100.0f;

    // Pressure pathing patterns (used by the periodic "pressure" instruction)
    inline constexpr float kArenaPressureBehindPlayerDistance = 900.0f;
    inline constexpr float kArenaPressureFlankOffset = 900.0f;
    inline constexpr float kArenaPressureRingRadius = 1300.0f;
    inline constexpr float kArenaPressureLeapfrogFraction = 0.55f;
    inline constexpr float kArenaPressureLeapfrogMinStep = 700.0f;
    inline constexpr float kArenaPressureLeapfrogMaxStep = 2500.0f;
    inline constexpr float kArenaPressureTargetJitterXY = 200.0f;

    // ---------------------------------------------------------------------
    // Cheats
    // ---------------------------------------------------------------------
    inline constexpr float kBulletTimeMinScale = 0.001f;
    inline constexpr float kBulletTimeMaxScale = 1.0f;

    // ---------------------------------------------------------------------
    // Commands / output
    // ---------------------------------------------------------------------
    inline constexpr std::size_t kCommandDefaultListMaxLines = 10;

    // ---------------------------------------------------------------------
    // AI subsystem spawn (generic, not arena-specific)
    // ---------------------------------------------------------------------
    inline constexpr float kAISpawnGroundTraceUp = 5000.0f;
    inline constexpr float kAISpawnGroundTraceDown = 5000.0f;
    inline constexpr float kAISpawnGroundZOffset = 120.0f;

    // ---------------------------------------------------------------------
    // Friend NPC
    // ---------------------------------------------------------------------
    inline constexpr int   kFriendMaxCount = 3;                   // max simultaneous friend NPCs
    inline constexpr float kFriendFollowDistanceMax = 2000.0f;    // if friend is farther than this, move it
    inline constexpr float kFriendFollowDistanceMin = 350.0f;     // target minimum distance from player
    inline constexpr float kFriendFollowDistanceTarget = 600.0f;  // ideal distance from player when repositioning
    inline constexpr float kFriendFollowAcceptanceRadius = 200.0f;
    // throttle how often a friend will be told to move when he's too far away.  Without this,
    // the NPC may receive movement commands every single tick while the player walks, which
    // shows up as repeated log lines and extra AI churn.
    inline constexpr float kFriendFollowCooldownBase     = 10.0f;  // seconds; jittered ±kFriendJitterFraction
    inline constexpr float kFriendRepositionIntervalBase = 30.0f; // seconds; jittered ±50%
    inline constexpr float kFriendAmbientSoundIntervalBase = 30.0f;
    inline constexpr float kFriendEnemySpotCheckIntervalBase = 10.0f; // how often we check for enemies
    inline constexpr float kFriendEnemySpotChance = 0.60f;        // 60% chance per check when enemy in range
    inline constexpr float kFriendEnemySpotRangeUnits = 4000.0f;
    inline constexpr int   kFriendGroupId = 200;                  // separate from arena enemies (101)
    inline constexpr float kFriendJitterFraction = 0.50f;         // ±50% on all interval timers
    inline constexpr float kFriendIdleResetIntervalSeconds = 10.0f; // how often we re-assert Idle state
    inline constexpr float kFriendDeathCleanupDelaySeconds = 3.0f;  // wait for tragedy sound before cleanup

    // Extra friend movement tuning
    inline constexpr float kFriendStuckTeleportFraction = 0.20f;      // teleport step as fraction of distance when stuck
    inline constexpr float kFriendCatchupDistanceThreshold = 4000.0f; // if farther than this on stuck, teleport within 4000

    // Sound system
    inline constexpr const char* kDefaultSoundsFolder = "sounds";   // relative to CWD
    inline constexpr int   kSoundGroupRetryCount = 3;               // retries when a sound fails to play
    inline constexpr const char* kFriendAmbientSoundGroup = "ambient";    // group for periodic idle sounds

    // -------------------------------------------------------------------------
    // SpatialAudio – 3D positional audio engine (waveOut)
    // -------------------------------------------------------------------------

    // Distance model (all distances in Unreal units, i.e. cm)
    inline constexpr float kSpatialInnerRadius      = 300.0f;   // full-volume sphere radius
    inline constexpr float kSpatialOuterRadius      = 5000.0f;  // inaudible beyond this

    // Low-pass filter distance thresholds
    inline constexpr float kSpatialLpfStartRadius   = 1000.0f;  // LPF starts rolling off here
    inline constexpr float kSpatialLpfEndRadius     = 5000.0f;  // LPF fully applied here

    // LPF coefficient range: 1.0 = passthrough, lower = more muffled.
    // One-pole IIR:  y[n] = alpha * x[n] + (1-alpha) * y[n-1]
    inline constexpr float kSpatialLpfAlphaMin      = 0.05f;    // at kSpatialLpfEndRadius
    inline constexpr float kSpatialLpfAlphaMax      = 1.0f;     // at kSpatialLpfStartRadius

    // Master output gain applied after all per-source mixing.
    // Increase this if playback is too quiet.  1.0 = no boost.
    // The constant-power pan law inherently attenuates centred sources by ~3 dB;
    // a value of 3.0-4.0 compensates for that and brings levels up.
    inline constexpr float kSpatialOutputGain       = 3.0f;

    // Position update rate (Hz).  10 = recompute spatial params every ~100ms.
    inline constexpr float kSpatialPositionUpdateHz = 10.0f;

    // waveOut buffer configuration
    inline constexpr int   kSpatialOutputSampleRate   = 48000;
    inline constexpr int   kSpatialOutputChannels     = 2;      // stereo
    inline constexpr int   kSpatialOutputBitsPerSample = 16;
    inline constexpr int   kSpatialBufferCount        = 4;      // ring buffer count
    inline constexpr int   kSpatialBufferDurationMs   = 40;     // ms per buffer

    // Legacy aliases so existing code (FriendSubsystem etc.) still compiles
    inline constexpr float kFriendAmbientInnerRadius      = kSpatialInnerRadius;
    inline constexpr float kFriendAmbientAttenuationRadius = kSpatialOuterRadius;
    inline constexpr float kFriendAmbientLpfStartRadius   = kSpatialLpfStartRadius;

    inline constexpr const char* kFriendEnemySpottedSoundGroup = "enemyspotted";
    inline constexpr const char* kFriendTragedySoundGroup = "tragedy";
}
