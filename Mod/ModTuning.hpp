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
    inline constexpr int   kArenaDefaultWaveSize = 40;
    inline constexpr float kArenaDefaultSpawnDistance = 8000.0f;
    inline constexpr float kArenaMinSpawnDistance = 1000.0f;

    // Pre-spawn happens during wave cooldown; we spawn far away to avoid hitching,
    // then reposition into the final ring at wave start.
    inline constexpr float kArenaPreSpawnExtraDistance = 5000.0f;

    inline constexpr float kArenaWaveCooldownSeconds = 5.0f;
    inline constexpr float kArenaNextWaveDelaySeconds = 5.0f;

    inline constexpr float kArenaSpawnTickIntervalSeconds = 0.5f;
    inline constexpr float kArenaScanIntervalSeconds = 30.0f;
    inline constexpr float kArenaMoveToPlayerIntervalSeconds = 30.0f;

    inline constexpr float kArenaProximityNoticeStartSeconds = 30.0f;
    inline constexpr float kArenaProximityNoticeIntervalSeconds = 5.0f;

    inline constexpr int   kArenaMaxConcurrentAttackers = 10;

    // Anti-stuck
    inline constexpr float kArenaStuckImmobileSeconds = 10.0f;
    inline constexpr float kArenaStuckTeleportCooldownSeconds = 3.0f;
    inline constexpr float kArenaOverdueWaveSeconds = 120.0f;
    inline constexpr float kArenaTeleportMinDistanceFromPlayer = 1000.0f;
    inline constexpr float kArenaTeleportDistanceFactor = 0.5f;

    // LoS avoidance tuning (spawn/teleport)
    inline constexpr float kArenaLoSVisibilityTraceEndZOffset = 80.0f;
    inline constexpr float kArenaLoSVisibilityHitNearEndDistance = 120.0f;

    // cos(60deg) = 0.5 -> 120deg full cone
    inline constexpr double kArenaLoSHalfConeCos = 0.5;

    // Candidate search steps
    inline constexpr float kArenaRepositionDistanceStep = 600.0f;
    inline constexpr float kArenaRepositionAngleStepDeg = 65.0f;
    inline constexpr float kArenaTeleportRetryDistanceStep = 750.0f;
    inline constexpr float kArenaTeleportRetrySideStep = 800.0f;

    // ---------------------------------------------------------------------
    // Cheats
    // ---------------------------------------------------------------------
    inline constexpr float kBulletTimeMinScale = 0.001f;
    inline constexpr float kBulletTimeMaxScale = 1.0f;
}
