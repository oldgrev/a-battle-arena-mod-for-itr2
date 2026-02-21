#pragma once

#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <map>

#include "..\CppSDK\SDK.hpp"
#include "AISubsystem.hpp"

namespace Mod::Arena
{
    class ArenaSubsystem
    {
    public:
        static ArenaSubsystem* Get();

        void Initialize(Mod::AI::AISubsystem *aiSubsystem);

        // One-time start: arena_start [count] [distance]
        void Start(int enemiesPerWave = 30, float distance = 5000.0f);
        void Stop();
        bool IsActive() const { return active_.load(); }

        std::string GetStatus() const;

        // Called when an NPC dies.
        void OnNPCDeath(SDK::AActor* actor);

        // Stats tracking
        void RecordPlayerDamageTaken(float damage);
        void RecordBulletFired();

        // New: Tick-based logic
        void Update(SDK::UWorld* world);

        const std::vector<std::string>& GetDiscoveredNPCs() const { return discoveredNPCs_; }

    private:
        void StartWave(SDK::UWorld* world);
        void SpawnOneNPC(SDK::UWorld* world, bool farAway = false);
        void CheckWaveCompletion(SDK::UWorld* world);
        void ResetWaveStats();

        void LoadNPCList();
        void SaveNPCList();
        void ScanForNPCs(SDK::UWorld* world);
        void InstructNPCsToMoveToPlayer(SDK::UWorld* world);
        void BuffNPCAwareness(SDK::APawn* pawn);
        void SetGroupAttackLimit(SDK::UWorld* world, int limit);
        void LockTimeOfDay(SDK::UWorld* world, bool lock);
        void CheckAntiStuck(SDK::UWorld* world);
        void CheckProximityNotification(SDK::UWorld* world);
        void ProcessPreSpawning(SDK::UWorld* world);
        void PruneInvalidEnemies(SDK::UWorld* world);

        Mod::AI::AISubsystem *aiSubsystem_ = nullptr;

        std::atomic<bool> active_{false};
        bool isPreSpawning_ = false;
        int wave_ = 0;
        int enemiesPerWave_ = 10;
        float distance_ = 5000.0f;

        int enemiesToSpawn_ = 0;
        int spawnedInCurrentWave_ = 0;

        bool wavePending_ = false;
        float waveCooldown_ = 5.0f;
        float nextWaveTimer_ = 0.0f;

        float lastScanTime_ = 0.0f;
        float lastMoveToPlayerTime_ = 0.0f;
        float lastSpawnTime_ = 0.0f;
        float lastPreSpawnTime_ = 0.0f;
        float waveStartTime_ = 0.0f;
        float lastProximityNoticeTime_ = 0.0f;

        struct EnemyState
        {
            SDK::FVector lastPos;
            float lastMoveTime;
            float lastTeleportTime;
        };

        std::vector<std::string> discoveredNPCs_;
        mutable std::recursive_mutex discoveredNPCsMutex_;

        mutable std::recursive_mutex statsMutex_;
        float waveAccumulatedDamage_ = 0.0f;
        int waveBulletsFired_ = 0;

        mutable std::recursive_mutex activeEnemiesMutex_;
        std::vector<SDK::AActor*> activeEnemies_;
        std::map<SDK::AActor*, EnemyState> enemyStates_;
    };
}
