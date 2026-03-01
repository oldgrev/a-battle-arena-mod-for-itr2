/*
AILEARNINGS
- Friend NPC: NPCConfig may be shared across all NPCs with the same data asset. Zeroing MaxSenseDistance
  may affect ALL mimics on the map (including arena enemies). The two-layer mitigation is: (1) zero senses
  so the NPC has no targets; (2) keep perpetual Idle state in TickIdleReset. If arena enemies suddenly lose
  awareness after spawning a friend, this shared-config issue is the root cause.
- Friend follow: MoveToLocation is called when distance > kFriendFollowDistanceMax. The acceptance radius
  is kFriendFollowAcceptanceRadius; the NPC will stop and resume walking naturally once it reaches that radius.
- Sound groups: 'ambient', 'enemyspotted', 'tragedy' must exist in the sounds/ folder (or via .txt files).
  Missing groups cause a skip + LOG_WARN -- no crash.
- TickEnemySpot: scans GetAllActorsOfClass every kFriendEnemySpotCheckIntervalBase seconds; this is
  O(n) over all AI characters. If there are 100+ NPCs and multiple friends, this could get expensive.
  Consider reducing check frequency or caching if performance is an issue.
- Death sequence: on detecting death via OnActorDeath, the friend is marked IsDead. On the next tick:
  current media is stopped, tragedy sound is started. After kFriendDeathCleanupDelaySeconds the
  entry is marked cleanedUp and removed from the list in the Update sweep.
- ScopedProcessEventGuard: all code paths that touch SDK gameplay functions are guarded to prevent
  infinite recursion through the ProcessEvent hook.
- CRASH FIX (2026-02-26): Friend NPC crashes were caused by media components attached to actors becoming
  invalid when the actor was destroyed or became invalid. The media system did not clean up stale media
  instances. Fix: added CleanupStaleMedia() function called from DrainPending() that removes media instances
  with invalid actors or finished playback, properly closes media players and destroys sound components.
- SPATIAL AUDIO FIX (2026-02-26): 3D audio attached to friend NPCs was not spatializing correctly -- audio
  sounded centered on player even at 3000 units distance. Root cause: SpawnAttachedMediaSoundComponent enabled
  bAllowSpatialization but did not configure attenuation settings. Fix: added bOverrideAttenuation = true and
  configured AttenuationOverrides with proper distance falloff (5000 units max), dB attenuation (-60dB at max),
  and low-pass filtering at distance for realism.
- PATHFINDING STUCK FIX (2026-02-26): Friend NPCs would get stuck unable to path to the player, repeatedly
  spamming MoveToLocation commands. Root cause: no unstuck detection. Fix: track NPC position between follow
  attempts; if NPC moves less than 100 units across 3 consecutive follow attempts, take corrective action:
    * if moderately close, step a fraction of the distance toward the player (kFriendStuckTeleportFraction)
    * if very far (> kFriendCatchupDistanceThreshold) perform a catch‑up teleport inside that radius, avoiding
      direct line-of-sight when possible
    * always reset stuck counter and update lastFollowPosition after teleport
  Additionally, set the NPCController state to Combat before issuing MoveToLocation so the command is honored.
- FOLLOW BROKEN FIX (2026-02-27): Friend NPCs were not following the player beyond a few steps.
  Root cause: TickIdleReset ran every 10s and reset the NPC to Idle state, which cancelled the
  MoveToLocation commands issued by TickFollow. Meanwhile, kFriendFollowCooldownBase was 20s,
  so the NPC only got a new movement command every 20s but was reset to Idle after 10s.
  Fix: (1) reduced kFriendFollowCooldownBase from 20s to 3s so follow commands are reissued faster.
  (2) added lastFollowCommandTime timestamp to FriendEntry; TickIdleReset now skips the Idle
  reset if a follow command was issued within kFriendFollowGraceSeconds (8s). Perception disable
  is still re-applied regardless. (3) added kFriendFollowGraceSeconds constant to ModTuning.
*/

#include "FriendSubsystem.hpp"

#include <sstream>
#include <algorithm>
#include <cmath>

#include "..\CppSDK\SDK.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"
#include "HookManager.hpp"
#include "ArenaSubsystem.hpp"
#include "ModTuning.hpp"

namespace Mod::Friend
{
    // TArray access helpers (mirrors pattern from ArenaSubsystem).
    template <typename T>
    struct TArrayRaw { T* Data; int32_t Num; int32_t Max; };
    template <typename T>
    static int32_t GetTArrayNum(const SDK::TArray<T>& a)
    {
        return reinterpret_cast<const TArrayRaw<T>*>(&a)->Num;
    }
    template <typename T>
    static T* GetTArrayData(const SDK::TArray<T>& a)
    {
        return reinterpret_cast<const TArrayRaw<T>*>(&a)->Data;
    }

    // File-scope singleton.
    static FriendSubsystem g_FriendInstance;

    FriendSubsystem* FriendSubsystem::Get()
    {
        return &g_FriendInstance;
    }

    // -------------------------------------------------------------------------
    // OnPerceptionUpdated hook
    // -------------------------------------------------------------------------
    // pThis is the ARadiusAIControllerBase. We intercept perception events for
    // friend NPCs and block them when the perceived Target is the player.
    // Perception of other AI characters (enemies) is let through so the friend
    // naturally engages enemies via the normal AI aggro system.
    // -------------------------------------------------------------------------
    struct OnPerceptionUpdatedParams
    {
        SDK::AActor* Target;        // first param
        // FAIStimulus Stimulus follows -- we don't read it
    };

    static bool Hook_OnPerceptionUpdated(
        SDK::UObject* object,
        SDK::UFunction* /*function*/,
        void* parms,
        Mod::HookManager::ProcessEventFn /*originalFn*/)
    {
        if (!object || !parms) return false;
        if (!object->IsA(SDK::ARadiusAIControllerBase::StaticClass())) return false;

        auto* controller = static_cast<SDK::ARadiusAIControllerBase*>(object);
        SDK::APawn* controlledPawn = controller->Pawn;
        if (!controlledPawn) return false;

        // Only intercept for friend NPCs.
        if (!g_FriendInstance.IsFriendActor(static_cast<SDK::AActor*>(controlledPawn)))
            return false;

        auto* params = static_cast<OnPerceptionUpdatedParams*>(parms);
        SDK::AActor* target = params->Target;
        if (!target) return true; // null target -- block

        // Allow perception of other AI characters (enemies) so the friend can fight them.
        if (target->IsA(SDK::ARadiusAICharacterBase::StaticClass()))
            return false; // allow -- call original

        // Block perception of anything else (the player pawn, etc.).
        return true; // skip original -- NPC ignores the player
    }

    void FriendSubsystem::Initialize()
    {
        LOG_INFO("[Friend] FriendSubsystem initialized");
        // Hook OnPerceptionUpdated to selectively block player perception for friends.
        Mod::HookManager::Get().RegisterNamedHook("OnPerceptionUpdated", &Hook_OnPerceptionUpdated);
    }

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------

    float FriendSubsystem::JitteredInterval(float base) const
    {
        const float jitter = Mod::Tuning::kFriendJitterFraction;
        std::uniform_real_distribution<float> dist(base * (1.0f - jitter), base * (1.0f + jitter));
        return dist(rng_);
    }

    bool FriendSubsystem::FindPositionNearPlayer(SDK::UWorld* world, float minDist, float maxDist, SDK::FVector& outPos) const
    {
        if (!world) return false;

        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return false;

        SDK::FVector playerLoc = player->K2_GetActorLocation();

        std::uniform_real_distribution<float> angleDist(0.0f, 6.28318530f);   // 0 - 2π
        std::uniform_real_distribution<float> distDist(minDist, maxDist);

        const float angle = angleDist(rng_);
        const float dist  = distDist(rng_);

        SDK::FVector candidate;
        candidate.X = playerLoc.X + std::cosf(angle) * dist;
        candidate.Y = playerLoc.Y + std::sinf(angle) * dist;
        candidate.Z = playerLoc.Z;

        // Ground trace.
        SDK::FVector traceStart = { candidate.X, candidate.Y, candidate.Z + Mod::Tuning::kArenaGroundTraceUp };
        SDK::FVector traceEnd   = { candidate.X, candidate.Y, candidate.Z - Mod::Tuning::kArenaGroundTraceDown };
        SDK::FHitResult hit{};
        SDK::TArray<SDK::AActor*> ignore;
        ignore.Add(player);

        const bool hitGround = SDK::UKismetSystemLibrary::LineTraceSingle(
            world, traceStart, traceEnd,
            SDK::ETraceTypeQuery::TraceTypeQuery1,
            false, ignore,
            SDK::EDrawDebugTrace::None,
            &hit, true,
            SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);

        if (!hitGround || !hit.bBlockingHit)
            return false;

        candidate.Z = hit.ImpactPoint.Z + Mod::Tuning::kArenaGroundSpawnZOffset;
        outPos = candidate;
        return true;
    }

    void FriendSubsystem::ZeroSenses(SDK::ARadiusAICharacterBase* aiChar) const
    {
        if (!aiChar) return;

        // --- Layer 1: NPCConfig fields (may be shared, but belt-and-suspenders) ---
        if (aiChar->NPCConfig)
        {
            aiChar->NPCConfig->MaxSenseDistance = 0.01f;
            aiChar->NPCConfig->SoundProofMultiplierDefault = 9999.0f;
            aiChar->NPCConfig->SuspiciousActivityLevel = 99999.0f;
            aiChar->NPCConfig->DelayToStartReduceDetectionScale = 999.0f;
            aiChar->NPCConfig->DetectReductionTime = 0.001f;
        }

        // --- Layer 2: Disable the perception component entirely for this instance ---
        // This is the reliable fix: SetAllSensesEnabled(false) stops the UE perception
        // subsystem from delivering ANY new stimuli to this NPC's perception component.
        // ForgetAll() clears anything it had already detected.
        if (aiChar->AIController)
        {
            SDK::URadiusAIPerceptionComponent* perc = aiChar->AIController->AIPerceptionComponent;
            if (perc)
            {
                perc->SetAllSensesEnabled(false);
                perc->ForgetAll();
                LOG_INFO("[Friend] ZeroSenses: perception disabled for " << aiChar->GetName());
            }
            else
            {
                LOG_WARN("[Friend] ZeroSenses: AIPerceptionComponent null for " << aiChar->GetName());
            }

            // Clear any accumulated aggro.
            SDK::UAggroComponent* aggro = aiChar->AIController->AggroComponent;
            if (aggro)
            {
                aggro->ReduceAggro();
            }

            // Also patch the controller's own NPCConfig reference.
            if (aiChar->AIController->NPCConfig)
            {
                aiChar->AIController->NPCConfig->MaxSenseDistance = 0.01f;
                aiChar->AIController->NPCConfig->SoundProofMultiplierDefault = 9999.0f;
                aiChar->AIController->NPCConfig->SuspiciousActivityLevel = 99999.0f;
            }
        }
        LOG_INFO("[Friend] ZeroSenses applied to " << aiChar->GetName());
    }

    void FriendSubsystem::ResetToIdle(SDK::ARadiusAICharacterBase* aiChar) const
    {
        if (!aiChar) return;
        if (aiChar->AIController)
        {
            aiChar->AIController->SetNPCState(SDK::ENPCState::Idle);
        }
    }

    // helper: determine whether a location is currently within the player's view/line-of-sight.
    // copied/adapted from ArenaSubsystem::IsLocationInPlayerLineOfSight.
    static bool IsLocationInPlayerLineOfSight(SDK::UWorld* world, const SDK::FVector& location)
    {
        if (!world)
            return false;

        SDK::FVector viewLoc{};
        SDK::FRotator viewRot{};
        if (!Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
            return false;

        SDK::FVector toTarget;
        toTarget.X = location.X - viewLoc.X;
        toTarget.Y = location.Y - viewLoc.Y;
        toTarget.Z = location.Z - viewLoc.Z;

        const double lenSq = (double)toTarget.X * (double)toTarget.X +
                             (double)toTarget.Y * (double)toTarget.Y +
                             (double)toTarget.Z * (double)toTarget.Z;
        if (lenSq < 1.0)
            return true;

        const double invLen = 1.0 / std::sqrt(lenSq);
        SDK::FVector dir;
        dir.X = (float)((double)toTarget.X * invLen);
        dir.Y = (float)((double)toTarget.Y * invLen);
        dir.Z = (float)((double)toTarget.Z * invLen);

        const SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(viewRot);
        const double dot = (double)forward.X * (double)dir.X +
                           (double)forward.Y * (double)dir.Y +
                           (double)forward.Z * (double)dir.Z;

        if (dot < Mod::Tuning::kArenaLoSHalfConeCos)
            return false;

        SDK::FVector traceStart = viewLoc;
        SDK::FVector traceEnd = location;
        traceEnd.Z += Mod::Tuning::kArenaLoSVisibilityTraceEndZOffset;

        SDK::TArray<SDK::AActor*> ignore;
        if (auto* pawn = Mod::GameContext::GetPlayerPawn(world))
            ignore.Add(pawn);

        SDK::FHitResult hit{};
        const bool hitSomething = SDK::UKismetSystemLibrary::LineTraceSingle(
            world,
            traceStart,
            traceEnd,
            SDK::ETraceTypeQuery::TraceTypeQuery1,
            false,
            ignore,
            SDK::EDrawDebugTrace::None,
            &hit,
            true,
            SDK::FLinearColor{},
            SDK::FLinearColor{},
            0.0f);

        if (!hitSomething || !hit.bBlockingHit)
            return true;

        const float distToEnd = SDK::UKismetMathLibrary::Vector_Distance(hit.ImpactPoint, traceEnd);
        return distToEnd < Mod::Tuning::kArenaLoSVisibilityHitNearEndDistance;
    }

    // -------------------------------------------------------------------------
    // SpawnFriend
    // -------------------------------------------------------------------------

    std::string FriendSubsystem::SpawnFriend(SDK::UWorld* world)
    {
        if (!world)
            return "world is null";

        Mod::ScopedProcessEventGuard guard;

        // Cap at max friends.
        {
            std::lock_guard<std::mutex> lock(friendsMutex_);
            int alive = 0;
            for (const auto& e : friends_)
                if (!e.cleanedUp && !e.IsDead) ++alive;
            if (alive >= Mod::Tuning::kFriendMaxCount)
            {
                return std::string("max friends reached (")
                    + std::to_string(alive) + "/" + std::to_string(Mod::Tuning::kFriendMaxCount) + ")";
            }
        }

        // Get discovered NPC class list from ArenaSubsystem.
        std::vector<std::string> pool;
        {
            Arena::ArenaSubsystem* arena = Arena::ArenaSubsystem::Get();
            if (!arena || arena->GetDiscoveredNPCs().empty())
            {
                return "no NPCs discovered yet -- run arena_start to scan, then retry";
            }
            // Copy under the implicit assumption that we're on the game thread.
            const auto& ref = arena->GetDiscoveredNPCs();
            pool.assign(ref.begin(), ref.end());
        }

        // Shuffle pool for random selection.
        std::shuffle(pool.begin(), pool.end(), rng_);

        // Find player position for spawn anchor.
        SDK::FVector playerLoc{};
        SDK::FRotator playerRot{};
        if (!Mod::GameContext::GetPlayerView(world, playerLoc, playerRot))
            return "cannot get player view";

        // Try up to 5 random classes and positions.
        for (int attempt = 0; attempt < 5 && !pool.empty(); ++attempt)
        {
            const std::string& className = pool[attempt % pool.size()];

            // Pick a spawn position near the player.
            SDK::FVector spawnLoc{};
            if (!FindPositionNearPlayer(world,
                    Mod::Tuning::kFriendFollowDistanceMin,
                    Mod::Tuning::kFriendFollowDistanceTarget,
                    spawnLoc))
            {
                LOG_WARN("[Friend] SpawnFriend: could not find ground near player (attempt " << attempt + 1 << ")");
                continue;
            }

            // Resolve NPC class.
            SDK::UClass* cls = SDK::UObject::FindClass(className);
            if (!cls && className.find('/') != std::string::npos)
            {
                std::wstring wPath(className.begin(), className.end());
                SDK::FSoftClassPath softPath = SDK::UKismetSystemLibrary::MakeSoftClassPath(SDK::FString(wPath.c_str()));
                auto softRef = SDK::UKismetSystemLibrary::Conv_SoftClassPathToSoftClassRef(softPath);
                cls = SDK::UKismetSystemLibrary::LoadClassAsset_Blocking(softRef);
            }
            if (!cls)
            {
                LOG_WARN("[Friend] SpawnFriend: class not found: " << className);
                continue;
            }

            // Face the player on spawn.
            SDK::FRotator lookRot = SDK::UKismetMathLibrary::FindLookAtRotation(spawnLoc, playerLoc);
            SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(spawnLoc, lookRot, {1, 1, 1});

            SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world, cls, spawnTransform,
                SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
                nullptr, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (!actor)
            {
                LOG_WARN("[Friend] SpawnFriend: BeginDeferredActorSpawnFromClass returned null for " << className);
                continue;
            }

            SDK::UGameplayStatics::FinishSpawningActor(actor, spawnTransform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (actor->IsA(SDK::ARadiusAICharacterBase::StaticClass()))
            {
                static_cast<SDK::APawn*>(actor)->SpawnDefaultController();

                auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(actor);
                aiChar->GroupID = Mod::Tuning::kFriendGroupId;

                // Zero all perception so it won't detect/react to anything.
                ZeroSenses(aiChar);

                // Start in Idle state.
                //ResetToIdle(aiChar);

                // Set up the FriendEntry with jittered timers.
                float timeNow = SDK::UGameplayStatics::GetTimeSeconds(world);
                FriendEntry entry;
                entry.Actor = actor;
                entry.IsDead = false;
                entry.nextRepositionTime   = timeNow + JitteredInterval(Mod::Tuning::kFriendRepositionIntervalBase);
                entry.nextAmbientSoundTime = timeNow + JitteredInterval(Mod::Tuning::kFriendAmbientSoundIntervalBase);
                entry.nextEnemySpotTime    = timeNow + JitteredInterval(Mod::Tuning::kFriendEnemySpotCheckIntervalBase);
                entry.nextIdleResetTime    = timeNow + Mod::Tuning::kFriendIdleResetIntervalSeconds;
                entry.nextFollowTime       = timeNow; // allow follow check immediately

                {
                    std::lock_guard<std::mutex> lock(friendsMutex_);
                    friends_.push_back(entry);
                }

                std::ostringstream oss;
                oss << "Friend spawned: " << actor->GetName() << " (" << className << ") near player";
                LOG_INFO("[Friend] " << oss.str());
                Mod::ModFeedback::ShowMessage(L"A friendly mimic joins you!", 3.0f,
                    SDK::FLinearColor{0.4f, 1.0f, 0.4f, 1.0f});
                return oss.str();
            }
            else
            {
                // Not an AI character -- destroy and skip.
                actor->K2_DestroyActor();
                LOG_WARN("[Friend] SpawnFriend: spawned actor is not ARadiusAICharacterBase: " << className);
            }
        }

        return "friend spawn failed after multiple attempts (check npcs.txt has valid paths)";
    }

    // -------------------------------------------------------------------------
    // OnActorDeath
    // -------------------------------------------------------------------------

    void FriendSubsystem::OnActorDeath(SDK::AActor* actor)
    {
        if (!actor) return;
        std::lock_guard<std::mutex> lock(friendsMutex_);
        for (auto& e : friends_)
        {
            if (e.Actor == actor && !e.IsDead)
            {
                e.IsDead = true;
                e.deathTime = -1.0f; // will be set on next tick when we process it
                LOG_INFO("[Friend] Friend NPC died: " << actor->GetName());
                return;
            }
        }
    }

    // -------------------------------------------------------------------------
    // ClearAll
    // -------------------------------------------------------------------------

    void FriendSubsystem::ClearAll(SDK::UWorld* world)
    {
        std::lock_guard<std::mutex> lock(friendsMutex_);
        Mod::ScopedProcessEventGuard guard;
        for (auto& e : friends_)
        {
            if (e.Actor && SDK::UKismetSystemLibrary::IsValid(e.Actor))
            {
                Mod::ModFeedback::StopMediaForActor(e.Actor);
                e.Actor->K2_DestroyActor();
            }
        }
        friends_.clear();
        LOG_INFO("[Friend] All friends cleared");
    }

    // -------------------------------------------------------------------------
    // ActiveFriendCount
    // -------------------------------------------------------------------------

    int FriendSubsystem::ActiveFriendCount() const
    {
        std::lock_guard<std::mutex> lock(friendsMutex_);
        int count = 0;
        for (const auto& e : friends_)
            if (!e.cleanedUp && !e.IsDead) ++count;
        return count;
    }

    bool FriendSubsystem::IsFriendActor(SDK::AActor* actor) const
    {
        if (!actor) return false;
        std::lock_guard<std::mutex> lock(friendsMutex_);
        for (const auto& e : friends_)
            if (!e.cleanedUp && e.Actor == actor) return true;
        return false;
    }

    SDK::AActor* FriendSubsystem::GetFirstFriendActor() const
    {
        std::lock_guard<std::mutex> lock(friendsMutex_);
        for (const auto& e : friends_)
            if (!e.cleanedUp && !e.IsDead && e.Actor && SDK::UKismetSystemLibrary::IsValid(e.Actor))
                return e.Actor;
        return nullptr;
    }

    // -------------------------------------------------------------------------
    // Per-entry tick behaviours
    // -------------------------------------------------------------------------

    void FriendSubsystem::TickFollow(SDK::UWorld* world, float /*time*/, FriendEntry& e)
    {
// Continuous: if friend is too far from player, instruct movement.  throttle with cooldown
    if (!e.Actor || !SDK::UKismetSystemLibrary::IsValid(e.Actor)) return;

    // cooldown period prevents spamming MoveToLocation every tick when the player is far away
    float currentTime = SDK::UGameplayStatics::GetTimeSeconds(world);
    if (currentTime < e.nextFollowTime) return;

        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return;

        const SDK::FVector friendLoc = e.Actor->K2_GetActorLocation();
        const SDK::FVector playerLoc = player->K2_GetActorLocation();

        const float dist = SDK::UKismetMathLibrary::Vector_Distance(friendLoc, playerLoc);
        if (dist <= Mod::Tuning::kFriendFollowDistanceMax) 
        {
            // Close enough -- reset stuck counter
            e.stuckFollowAttempts = 0;
            return;
        }

        // Check if NPC is stuck (hasn't moved much since last follow attempt)
        const float dx = friendLoc.X - e.lastFollowPosition.X;
        const float dy = friendLoc.Y - e.lastFollowPosition.Y;
        const float dz = friendLoc.Z - e.lastFollowPosition.Z;
        const float movementSinceLastFollow = std::sqrtf(dx * dx + dy * dy + dz * dz);
        
        const float kStuckThreshold = 100.0f; // If moved less than 100 units, consider it stuck
        const int kMaxStuckAttempts = 3;     // After 3 stuck attempts, corrective teleport/movement
        
        if (movementSinceLastFollow < kStuckThreshold && e.stuckFollowAttempts > 0)
        {
            e.stuckFollowAttempts++;
            LOG_INFO("[Friend] TickFollow: NPC appears stuck (moved only " << movementSinceLastFollow 
                << " units, attempt " << e.stuckFollowAttempts << "/" << kMaxStuckAttempts << ")");
            
            if (e.stuckFollowAttempts >= kMaxStuckAttempts)
            {
                // Calculate a teleport correction depending on distance
                float dist = SDK::UKismetMathLibrary::Vector_Distance(friendLoc, playerLoc);
                SDK::FVector teleportPos{};

                if (dist > Mod::Tuning::kFriendCatchupDistanceThreshold)
                {
                    // Catch-up: pick a spot within threshold radius; avoid direct LoS
                    if (FindPositionNearPlayer(world,
                            Mod::Tuning::kFriendFollowDistanceMin,
                            Mod::Tuning::kFriendCatchupDistanceThreshold,
                            teleportPos))
                    {
                        if (IsLocationInPlayerLineOfSight(world, teleportPos))
                        {
                            teleportPos.X = playerLoc.X - (teleportPos.X - playerLoc.X);
                            teleportPos.Y = playerLoc.Y - (teleportPos.Y - playerLoc.Y);
                        }
                    }
                }
                else
                {
                    // Incremental step toward player
                    SDK::FVector dir;
                    dir.X = playerLoc.X - friendLoc.X;
                    dir.Y = playerLoc.Y - friendLoc.Y;
                    dir.Z = playerLoc.Z - friendLoc.Z;
                    const float inv = dist > 0.0f ? 1.0f / dist : 0.0f;
                    dir.X *= inv;
                    dir.Y *= inv;
                    dir.Z *= inv;
                    float step = dist * Mod::Tuning::kFriendStuckTeleportFraction;
                    teleportPos = friendLoc;
                    teleportPos.X += dir.X * step;
                    teleportPos.Y += dir.Y * step;
                    teleportPos.Z += dir.Z * step;
                }

                if (teleportPos.X == 0 && teleportPos.Y == 0 && teleportPos.Z == 0)
                {
                    teleportPos = playerLoc;
                    teleportPos.Z += 50.0f;
                }

                // simple ground projection
                SDK::FHitResult hit{};
                SDK::UKismetSystemLibrary::LineTraceSingle(
                    world,
                    {teleportPos.X, teleportPos.Y, teleportPos.Z + 1000.0f},
                    {teleportPos.X, teleportPos.Y, teleportPos.Z - 3000.0f},
                    SDK::ETraceTypeQuery::TraceTypeQuery1,
                    false, {},
                    SDK::EDrawDebugTrace::None,
                    &hit, true,
                    SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
                if (hit.bBlockingHit)
                    teleportPos.Z = hit.ImpactPoint.Z + 100.0f;

                e.Actor->K2_SetActorLocation(teleportPos, false, nullptr, true);
                LOG_INFO("[Friend] TickFollow: teleported stuck NPC to " << teleportPos.X << "," << teleportPos.Y << "," << teleportPos.Z);

                e.stuckFollowAttempts = 0;
                e.lastFollowPosition = teleportPos;
                e.nextFollowTime = currentTime + JitteredInterval(Mod::Tuning::kFriendFollowCooldownBase);
                return;
            }
        }
        else
        {
            // NPC has moved or this is the first follow attempt -- reset counter
            e.stuckFollowAttempts = movementSinceLastFollow < kStuckThreshold ? 1 : 0;
        }
        
        e.lastFollowPosition = friendLoc;

        // Too far -- find a close position and move there.
        SDK::FVector target{};
        if (!FindPositionNearPlayer(world,
                Mod::Tuning::kFriendFollowDistanceMin,
                Mod::Tuning::kFriendFollowDistanceTarget,
                target))
        {
            // Fallback: move directly toward player at half-distance.
            target.X = playerLoc.X + (friendLoc.X - playerLoc.X) * 0.4f;
            target.Y = playerLoc.Y + (friendLoc.Y - playerLoc.Y) * 0.4f;
            target.Z = playerLoc.Z;
        }

        if (!e.Actor->IsA(SDK::ARadiusAICharacterBase::StaticClass()))
        {
            return;
        }

        auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(e.Actor);
        if (aiChar->AIController)
        {
            // ensure controller is in combat state so movement commands are honored
            aiChar->AIController->SetNPCState(SDK::ENPCState::Combat);

            aiChar->AIController->MoveToLocation(
                target,
                Mod::Tuning::kFriendFollowAcceptanceRadius,
                true, true, true, true, nullptr, true);
            LOG_INFO("[Friend] TickFollow: moving friend to follow player (dist=" << dist << ")");
            // schedule next allowed follow command
            e.nextFollowTime = currentTime + JitteredInterval(Mod::Tuning::kFriendFollowCooldownBase);
            e.lastFollowCommandTime = currentTime; // record so TickIdleReset won't cancel movement
        }
    }

    void FriendSubsystem::TickReposition(SDK::UWorld* world, float time, FriendEntry& e)
    {
        if (time < e.nextRepositionTime) return;
        e.nextRepositionTime = time + JitteredInterval(Mod::Tuning::kFriendRepositionIntervalBase);

        if (!e.Actor || !SDK::UKismetSystemLibrary::IsValid(e.Actor)) return;

        SDK::FVector target{};
        if (!FindPositionNearPlayer(world,
                Mod::Tuning::kFriendFollowDistanceMin,
                Mod::Tuning::kFriendFollowDistanceTarget * 1.3f,
                target))
        {
            LOG_WARN("[Friend] TickReposition: could not find ground position near player");
            return;
        }

        if (!e.Actor->IsA(SDK::ARadiusAICharacterBase::StaticClass()))
        {
            return;
        }

        auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(e.Actor);
        if (aiChar->AIController)
        {
            aiChar->AIController->MoveToLocation(
                target,
                Mod::Tuning::kFriendFollowAcceptanceRadius,
                true, true, true, true, nullptr, true);
            LOG_INFO("[Friend] TickReposition: friend moved to new nearby position");
        }
    }

    void FriendSubsystem::TickAmbientSound(SDK::UWorld* world, float time, FriendEntry& e)
    {
        (void)world;
        if (time < e.nextAmbientSoundTime) return;
        e.nextAmbientSoundTime = time + JitteredInterval(Mod::Tuning::kFriendAmbientSoundIntervalBase);

        if (!e.Actor || !SDK::UKismetSystemLibrary::IsValid(e.Actor)) return;

        // Only play if nothing is currently playing for this friend.
        if (Mod::ModFeedback::IsMediaPlayingForActor(e.Actor))
        {
            LOG_INFO("[Friend] TickAmbientSound: skipping (media already playing for friend)");
            return;
        }

        std::string chosen;
        std::string err;
        if (!Mod::ModFeedback::PlayRandomSoundGroupAttachedToActor(
                e.Actor, Mod::Tuning::kFriendAmbientSoundGroup,
                false, 1.0f, &chosen, &err))
        {
            LOG_WARN("[Friend] TickAmbientSound: group '" << Mod::Tuning::kFriendAmbientSoundGroup
                << "' play failed: " << err);
        }
        else
        {
            LOG_INFO("[Friend] TickAmbientSound: playing '" << chosen << "'");
        }
    }

    void FriendSubsystem::TickEnemySpot(SDK::UWorld* world, float time, FriendEntry& e)
    {
        if (time < e.nextEnemySpotTime) return;
        e.nextEnemySpotTime = time + JitteredInterval(Mod::Tuning::kFriendEnemySpotCheckIntervalBase);

        if (!e.Actor || !SDK::UKismetSystemLibrary::IsValid(e.Actor)) return;

        // Only react if not currently playing.
        if (Mod::ModFeedback::IsMediaPlayingForActor(e.Actor)) return;

        // 60% chance gate.
        std::uniform_real_distribution<float> chanceDist(0.0f, 1.0f);
        if (chanceDist(rng_) >= Mod::Tuning::kFriendEnemySpotChance) return;

        // Find nearest AI character that is not this friend (i.e., a real enemy).
        SDK::TArray<SDK::AActor*> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(
            world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);

        const int32_t count = GetTArrayNum(actors);
        SDK::AActor** data = GetTArrayData(actors);
        if (!data || count <= 0) return;

        const SDK::FVector friendLoc = e.Actor->K2_GetActorLocation();
        SDK::AActor* nearestEnemy = nullptr;
        float nearestDistSq = Mod::Tuning::kFriendEnemySpotRangeUnits * Mod::Tuning::kFriendEnemySpotRangeUnits;

        for (int i = 0; i < count; ++i)
        {
            SDK::AActor* candidate = data[i];
            if (!candidate || !SDK::UKismetSystemLibrary::IsValid(candidate)) continue;
            if (candidate == e.Actor) continue;  // skip self

            const SDK::FVector loc = candidate->K2_GetActorLocation();
            const float dx = loc.X - friendLoc.X;
            const float dy = loc.Y - friendLoc.Y;
            const float dz = loc.Z - friendLoc.Z;
            const float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq < nearestDistSq)
            {
                nearestDistSq = distSq;
                nearestEnemy  = candidate;
            }
        }

        if (!nearestEnemy) return;

        // Face the enemy direction.
        const SDK::FVector enemyLoc = nearestEnemy->K2_GetActorLocation();
        const SDK::FRotator lookRot = SDK::UKismetMathLibrary::FindLookAtRotation(friendLoc, enemyLoc);
        e.Actor->K2_SetActorRotation(lookRot, false);

        // Play enemyspotted sound.
        std::string chosen;
        std::string err;
        if (!Mod::ModFeedback::PlayRandomSoundGroupAttachedToActor(
                e.Actor, Mod::Tuning::kFriendEnemySpottedSoundGroup,
                false, 1.0f, &chosen, &err))
        {
            LOG_WARN("[Friend] TickEnemySpot: group '" << Mod::Tuning::kFriendEnemySpottedSoundGroup
                << "' play failed: " << err);
        }
        else
        {
            LOG_INFO("[Friend] TickEnemySpot: spotted enemy at dist="
                << std::sqrtf(nearestDistSq) << " playing '" << chosen << "'");
        }
    }

    void FriendSubsystem::TickIdleReset(SDK::UWorld* /*world*/, float time, FriendEntry& e)
    {
        if (time < e.nextIdleResetTime) return;
        e.nextIdleResetTime = time + Mod::Tuning::kFriendIdleResetIntervalSeconds;

        if (!e.Actor || !SDK::UKismetSystemLibrary::IsValid(e.Actor)) return;
        if (!e.Actor->IsA(SDK::ARadiusAICharacterBase::StaticClass())) return;
        auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(e.Actor);

        // FOLLOW-FIX: Only reset to Idle when not actively following.
        // TickFollow sets lastFollowCommandTime when it issues MoveToLocation.
        // If we reset to Idle while the NPC is executing a movement order, the
        // movement gets cancelled and the NPC stands still forever.
        const bool recentlyFollowing = (time - e.lastFollowCommandTime) < Mod::Tuning::kFriendFollowGraceSeconds;
        if (!recentlyFollowing)
        {
            ResetToIdle(aiChar);
        }

        // Re-apply perception disable as a safety net (e.g. level reload may re-enable it).
        // This is independent of the Idle reset -- we ALWAYS want the friend's perception off.
        if (aiChar->AIController)
        {
            SDK::URadiusAIPerceptionComponent* perc = aiChar->AIController->AIPerceptionComponent;
            if (perc)
            {
                perc->SetAllSensesEnabled(false);
                perc->ForgetAll();
            }
        }
    }

    void FriendSubsystem::TickDeathCleanup(SDK::UWorld* /*world*/, float time, FriendEntry& e)
    {
        if (!e.IsDead || e.cleanedUp) return;

        // First tick after death: record death time, stop current sound, play tragedy.
        if (e.deathTime < 0.0f)
        {
            e.deathTime = time;
            if (e.Actor && SDK::UKismetSystemLibrary::IsValid(e.Actor))
            {
                Mod::ModFeedback::StopMediaForActor(e.Actor);
            }
        }

        // Start tragedy sound after stopping (same tick or next).
        if (!e.deathSoundStarted)
        {
            e.deathSoundStarted = true;
            if (e.Actor && SDK::UKismetSystemLibrary::IsValid(e.Actor))
            {
                std::string chosen;
                std::string err;
                if (!Mod::ModFeedback::PlayRandomSoundGroupAttachedToActor(
                        e.Actor, Mod::Tuning::kFriendTragedySoundGroup,
                        false, 1.0f, &chosen, &err))
                {
                    LOG_WARN("[Friend] TickDeathCleanup: tragedy sound failed: " << err);
                }
                else
                {
                    LOG_INFO("[Friend] TickDeathCleanup: playing tragedy '" << chosen << "'");
                }
            }
        }

        // After delay, stop tragedy sound and mark cleaned up.
        if (time - e.deathTime >= Mod::Tuning::kFriendDeathCleanupDelaySeconds)
        {
            if (e.Actor && SDK::UKismetSystemLibrary::IsValid(e.Actor))
            {
                Mod::ModFeedback::StopMediaForActor(e.Actor);
            }
            e.cleanedUp = true;
            LOG_INFO("[Friend] TickDeathCleanup: friend cleaned up");
        }
    }

    // -------------------------------------------------------------------------
    // Update (main tick)
    // -------------------------------------------------------------------------

    void FriendSubsystem::Update(SDK::UWorld* world)
    {
        if (!world) return;

        Mod::ScopedProcessEventGuard guard;

        float time = SDK::UGameplayStatics::GetTimeSeconds(world);

        std::lock_guard<std::mutex> lock(friendsMutex_);

        // Process each friend entry.
        for (auto& e : friends_)
        {
            if (e.cleanedUp) continue;

            // Check if actor has become invalid unexpectedly (not via OnActorDeath).
            if (!e.IsDead && e.Actor && !SDK::UKismetSystemLibrary::IsValid(e.Actor))
            {
                LOG_WARN("[Friend] Update: friend actor became invalid without death notification: marking dead");
                e.IsDead = true;
            }

            if (e.IsDead)
            {
                TickDeathCleanup(world, time, e);
                continue;
            }

            // Living friend ticks.
            TickFollow      (world, time, e);
            TickReposition  (world, time, e);
            TickAmbientSound(world, time, e);
            TickEnemySpot   (world, time, e);
            TickIdleReset   (world, time, e);
        }

        // Prune fully cleaned-up entries.
        friends_.erase(
            std::remove_if(friends_.begin(), friends_.end(),
                [](const FriendEntry& e) { return e.cleanedUp; }),
            friends_.end());
    }

} // namespace Mod::Friend
