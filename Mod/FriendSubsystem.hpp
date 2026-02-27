#pragma once

/*
AILEARNINGS
- Friend NPC going hostile (FIXED): The first implementation only patched UNPCConfig memory fields
  (MaxSenseDistance etc.) but never touched the URadiusAIPerceptionComponent instance. The perception
  component runs its own stimulus processing independently of NPCConfig, so the NPC kept seeing the
  player. Fix applied (three layers):
    1. ZeroSenses now calls AIPerceptionComponent->SetAllSensesEnabled(false) -- disables all UE
       perception channels (sight, hearing, damage) at the subsystem level for this instance.
    2. ZeroSenses calls ForgetAll() to clear any stimuli that were already recorded before we disabled it.
    3. A named ProcessEvent hook intercepts OnPerceptionUpdated on ARadiusAIControllerBase. For controllers
       whose pawn is a registered friend, perception of non-AI targets (i.e. the player) is silently
       dropped. Perception of ARadiusAICharacterBase targets (other NPCs / enemies) is allowed through,
       which means the friend NPC WILL naturally fight enemies via the normal AI aggro system.
    4. TickIdleReset re-applies SetAllSensesEnabled(false) + ForgetAll() every 10 s as a safety net.
- Friend NPC: NPCConfig may be a shared Transient pointer (runtime-assigned per actor from a data asset).
  Modifying it changes ALL npcs that share the same config. We NO LONGER rely on NPCConfig zeroing as the
  primary mechanism -- SetAllSensesEnabled + hook is reliable per-instance.
- Sound grouping: 'tragedy' and 'enemyspotted' are expected group names in the sounds/ folder (or .txt
  files inside it). If the groups are absent, the friend NPC will silently skip those sounds -- no crash.
*/

#include <vector>
#include <string>
#include <mutex>
#include <random>
#include <atomic>

#include "..\CppSDK\SDK.hpp"
#include "ModTuning.hpp"

namespace Mod::Friend
{
    // Per-friend NPC runtime state.
    struct FriendEntry
    {
        SDK::AActor* Actor = nullptr;
        bool         IsDead = false;

        // Timers (compared to UGameplayStatics::GetTimeSeconds)
        float nextRepositionTime   = 0.0f;  // periodic hero-wander reposition
        float nextAmbientSoundTime = 0.0f;  // ambient random-sound trigger
        float nextEnemySpotTime    = 0.0f;  // next enemy-proximity check
        float nextIdleResetTime    = 0.0f;  // periodic re-assertion of Idle state
        float nextFollowTime       = 0.0f;  // throttle MoveToLocation calls (cooldown)

        // Unstuck detection
        SDK::FVector lastFollowPosition = {0.0f, 0.0f, 0.0f};
        int stuckFollowAttempts = 0;        // consecutive follow attempts with minimal movement
        float lastFollowCommandTime = -999.0f; // timestamp of last MoveToLocation issued by TickFollow

        // Death sequence timing
        float deathTime            = -1.0f;
        bool  deathSoundStarted    = false;
        bool  cleanedUp            = false;
    };

    class FriendSubsystem
    {
    public:
        static FriendSubsystem* Get();

        void Initialize();

        // Called every game tick (from ModMain::OnTick).
        void Update(SDK::UWorld* world);

        // Spawn a random mimic NPC near the player as a friendly companion.
        // Returns a status/result string.
        std::string SpawnFriend(SDK::UWorld* world);

        // Notify that an actor has died; used to intercept friend deaths.
        void OnActorDeath(SDK::AActor* actor);

        // Clear and destroy all tracked friends.
        void ClearAll(SDK::UWorld* world);

        // Returns the current number of active (living) friends.
        int ActiveFriendCount() const;

        // Returns true if actor is a tracked, non-cleaned-up friend NPC.
        bool IsFriendActor(SDK::AActor* actor) const;

        // Returns the first friend's actor pointer (or nullptr if none).
        SDK::AActor* GetFirstFriendActor() const;

    private:
        // Returns a jittered version of a base interval (±kFriendJitterFraction).
        float JitteredInterval(float base) const;

        // Find a ground-projected position near the player at a random angle,
        // within [minDist, maxDist] units. Returns false if no ground found.
        bool FindPositionNearPlayer(SDK::UWorld* world, float minDist, float maxDist, SDK::FVector& outPos) const;

        // Zero the NPC's perception config so it can't see or hear anything.
        void ZeroSenses(SDK::ARadiusAICharacterBase* aiChar) const;

        // Keep the NPC in Idle state so it doesn't engage.
        void ResetToIdle(SDK::ARadiusAICharacterBase* aiChar) const;

        // Per-entry per-tick behaviours.
        void TickFollow       (SDK::UWorld* world, float time, FriendEntry& e);
        void TickReposition   (SDK::UWorld* world, float time, FriendEntry& e);
        void TickAmbientSound (SDK::UWorld* world, float time, FriendEntry& e);
        void TickEnemySpot    (SDK::UWorld* world, float time, FriendEntry& e);
        void TickIdleReset    (SDK::UWorld* world, float time, FriendEntry& e);
        void TickDeathCleanup (SDK::UWorld* world, float time, FriendEntry& e);

        mutable std::mutex  friendsMutex_;
        std::vector<FriendEntry> friends_;
        mutable std::mt19937 rng_{ std::random_device{}() };
    };

} // namespace Mod::Friend
