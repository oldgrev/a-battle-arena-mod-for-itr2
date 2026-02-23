

/*
AILEARNINGS
- Arena: Fixed major bug where StartWave failed to initialize enemiesToSpawn_, causing 0 enemies to spawn.
- Arena: Updated StartWave to correctly calculate remaining spawns based on pre-spawned count from activeEnemies_.
- Arena: Added safety sweep to prune invalid enemy refs before wave transitions/update to avoid crashes and log removals.
- Arena: "Enemies behind player" was caused by (1) spawn/reposition being centered on the current player location with only LoS avoidance (behind is always "not in LoS"), and (2) anti-stuck teleport using `playerLoc + dir * distance` which overshot to the far side of the player. Fix: capture a wave-start "escape direction" and reject any spawn/reposition/teleport candidates in that hemisphere; fix anti-stuck to place at `playerLoc - dir * distance` (NPC-side of player).
- Arena: NPC types are persisted to npcs.txt as full object paths to allow loading unloaded assets. File is preserved (no overwrite on empty/old-format) to retain historical discoveries across sessions.
- Arena: Fixed spawning issue where NPCs could not be spawned unless already loaded in memory by implementing SoftClassPath loading logic in SpawnOneNPC.
- Arena: SpawnOneNPC now builds paths via MakeSoftClassPath and loads with LoadClassAsset_Blocking for reliable Blueprint class loading from persisted paths.
- Arena: ScanForNPCs now captures full object paths via GetPathName (e.g. /Game/...) instead of just class names.
- Arena: Fixed hard lock by using std::recursive_mutex for activeEnemiesMutex_ and statsMutex_ to avoid deadlocks when SDK calls trigger ProcessEvent hooks.
- Arena: Added Mod::ScopedProcessEventGuard to all arena functions that call SDK/Engine functions to prevent infinite recursion and improve stability.
- Arena: TArray members are protected; use TArrayRaw helper for Num/Data access.
- Arena: Added comprehensive logging to StartWave and SpawnOneNPC to trace spawning failures.
- Arena: "Move to player" logic triggered every 30s for all active arena NPCs to maintain pressure.
- Arena: Buffed NPC awareness by setting MaxSenseDistance to 10000, reducing detection time, and group attack limit to 10.
- Arena: Auto-wave advancement enabled by default.
- Arena: Real-time kill notifications show the number of remaining enemies in the current wave.
- Arena: Simplified commands to arena_start [count] [distance] and arena_stop.
- Arena: Edge avoidance implemented by checking for ground and attempting retries in a shuffle pattern.
- Arena: Added anti-stuck teleport logic (10s immobility -> 10% move to player; overdue (>120s) stuck mimics are aggressively pulled near the player).
- Arena: Anti-stuck now rate-limited (>=10s immobile, >=3s between teleports) with min 10m offset; overdue (>120s) static mimics are culled instead of spam-teleported.
- Arena: Proximity notifications (nearest enemy distance) every 5s after 30s into a wave.
- Arena: Fixed compiler errors by explicitly using SDK::FLinearColor{} in LineTraceSingle call instead of ambiguous {} initializers (C2059/C2589).
- Arena: Resolved C2589/C2059 caused by Windows MAX macro expanding `std::max`; wrapped call as `(std::max)` to bypass macro.
- Arena: Staggered NPC pre-spawning (during 5s cooldown) implemented to eliminate wave-start hitching.
- Arena: Stalemate fix added to anti-stuck logic; far-away NPCs that aren't making progress are teleported closer.
- Arena: Wave start locks time-of-day to midday via RadiusTimeSubsystem/TimeController; restored on arena stop.
- Arena: Spawns/teleports now avoid the player's line-of-sight (view-cone + visibility trace) by retrying further away and/or to the side; teleports rotate NPCs to face the player.
- Arena: Anti-stuck will not mark an NPC as stuck, teleport it, or cull it if the NPC is currently in the player's view (prevents visible pop-outs).
- Arena: Core arena tuning values (defaults, intervals, stuck timers, distances) are centralized in ModTuning.hpp for faster iteration.
- Arena: Added optional placement rule to prevent spawning/teleporting NPCs at locations where they have line-of-sight to the player; uses a 5x5 behind-grid probe with rear-corner retries (toggle in ModTuning.hpp).
*/

#include "ArenaSubsystem.hpp"
#include "Logging.hpp"
#include "ModFeedback.hpp"
#include "HookManager.hpp"
#include "GameContext.hpp"
#include "ModTuning.hpp"
#include "LoadoutSubsystem.hpp"

#include <sstream>
#include <random>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cmath>
#include <unordered_set>

namespace Mod::Arena
{
    // Helper to access TArray members when the SDK headers are being difficult.
    template <typename ElementType>
    struct TArrayRaw
    {
        ElementType *Data;
        int32_t NumElements;
        int32_t MaxElements;
    };

    template <typename ElementType>
    static int32_t GetTArrayNum(const SDK::TArray<ElementType>& arr)
    {
        return reinterpret_cast<const TArrayRaw<ElementType>*>(&arr)->NumElements;
    }

    template <typename ElementType>
    static ElementType* GetTArrayData(const SDK::TArray<ElementType>& arr)
    {
        return reinterpret_cast<const TArrayRaw<ElementType>*>(&arr)->Data;
    }

    static ArenaSubsystem* g_Arena = nullptr;

    static bool IsActorValidForArena(SDK::AActor* actor)
    {
        if (!actor || !actor->Class)
        {
            return false;
        }

        if (!SDK::UKismetSystemLibrary::IsValid(actor))
        {
            return false;
        }

        return true;
    }

    static SDK::FVector AddZ(const SDK::FVector& v, float dz)
    {
        SDK::FVector out = v;
        out.Z += dz;
        return out;
    }

    static SDK::FVector FlattenRotation(const SDK::FRotator& rot)
    {
        SDK::FRotator flat = rot;
        flat.Pitch = 0.0f;
        flat.Roll = 0.0f;
        return SDK::UKismetMathLibrary::GetForwardVector(flat);
    }

    // Returns true if the location is likely visible in the player's current line-of-sight.
    // Heuristic:
    // 1) Must be in front of the player's view within a cone.
    // 2) A visibility trace from view point to target must be unobstructed (hit near end, or no hit).
    static bool IsLocationInPlayerLineOfSight(SDK::UWorld* world, const SDK::FVector& location)
    {
        if (!world)
        {
            return false;
        }

        SDK::FVector viewLoc{};
        SDK::FRotator viewRot{};
        if (!Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
        {
            return false;
        }

        SDK::FVector toTarget;
        toTarget.X = location.X - viewLoc.X;
        toTarget.Y = location.Y - viewLoc.Y;
        toTarget.Z = location.Z - viewLoc.Z;

        const double lenSq = (double)toTarget.X * (double)toTarget.X + (double)toTarget.Y * (double)toTarget.Y + (double)toTarget.Z * (double)toTarget.Z;
        if (lenSq < 1.0)
        {
            return true;
        }

        const double invLen = 1.0 / std::sqrt(lenSq);
        SDK::FVector dir;
        dir.X = (float)((double)toTarget.X * invLen);
        dir.Y = (float)((double)toTarget.Y * invLen);
        dir.Z = (float)((double)toTarget.Z * invLen);

        const SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(viewRot);
        const double dot = (double)forward.X * (double)dir.X + (double)forward.Y * (double)dir.Y + (double)forward.Z * (double)dir.Z;

        // Rough “on screen” cone. If the point is behind/outside cone, we consider it not in LoS.
        if (dot < Mod::Tuning::kArenaLoSHalfConeCos)
        {
            return false;
        }

        // Visibility trace: if the first blocking hit is near the end, the point is visible.
        SDK::FVector traceStart = viewLoc;
        SDK::FVector traceEnd = AddZ(location, Mod::Tuning::kArenaLoSVisibilityTraceEndZOffset);

        SDK::TArray<SDK::AActor*> ignore;
        if (auto* playerPawn = Mod::GameContext::GetPlayerPawn(world))
        {
            ignore.Add(playerPawn);
        }

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
        {
            return true;
        }

        const float distToEnd = SDK::UKismetMathLibrary::Vector_Distance(hit.ImpactPoint, traceEnd);
        return distToEnd < Mod::Tuning::kArenaLoSVisibilityHitNearEndDistance;
    }

    // Returns true if a hypothetical NPC at `location` would likely have an unobstructed visibility trace
    // to the player's current view point.
    static bool DoesLocationHaveLineOfSightToPlayer(SDK::UWorld* world, const SDK::FVector& location)
    {
        if (!world)
        {
            return false;
        }

        SDK::FVector viewLoc{};
        SDK::FRotator viewRot{};
        if (!Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
        {
            // If we can't determine player view, don't over-reject placements.
            return false;
        }

        SDK::FVector traceStart = AddZ(location, Mod::Tuning::kArenaNoSeePlayerTraceStartZOffset);
        SDK::FVector traceEnd = AddZ(viewLoc, Mod::Tuning::kArenaLoSVisibilityTraceEndZOffset);

        SDK::TArray<SDK::AActor*> ignore;
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
        {
            return true;
        }

        const float distToEnd = SDK::UKismetMathLibrary::Vector_Distance(hit.ImpactPoint, traceEnd);
        return distToEnd < Mod::Tuning::kArenaLoSVisibilityHitNearEndDistance;
    }

    static bool TryProjectToGround(SDK::UWorld* world, SDK::APawn* playerPawn, SDK::FVector& inOut)
    {
        if (!world)
        {
            return false;
        }

        SDK::FVector traceStart = { inOut.X, inOut.Y, inOut.Z + Mod::Tuning::kArenaGroundTraceUp };
        SDK::FVector traceEnd   = { inOut.X, inOut.Y, inOut.Z - Mod::Tuning::kArenaGroundTraceDown };
        SDK::FHitResult hit{};
        SDK::TArray<SDK::AActor*> ignore;
        if (playerPawn)
        {
            ignore.Add(playerPawn);
        }

        if (!SDK::UKismetSystemLibrary::LineTraceSingle(
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
                0.0f))
        {
            return false;
        }

        inOut.Z = hit.ImpactPoint.Z + Mod::Tuning::kArenaGroundSpawnZOffset;
        return true;
    }

    static bool IsInEscapeDirectionSafe(const SDK::FVector& playerLoc, const SDK::FVector& location)
    {
        if (g_Arena)
        {
            return g_Arena->IsInEscapeDirection(playerLoc, location);
        }
        return false;
    }

    static bool TryFindPlacementWhereNpcCannotSeePlayer(
        SDK::UWorld* world,
        SDK::APawn* playerPawn,
        const SDK::FVector& playerLoc,
        const SDK::FVector& baseLocation,
        SDK::FVector& outLocation)
    {
        outLocation = baseLocation;

        if (!Mod::Tuning::kArenaAvoidNpcSeeingPlayer)
        {
            return true;
        }

        // If the base location already blocks visibility to the player, accept it.
        if (!DoesLocationHaveLineOfSightToPlayer(world, baseLocation))
        {
            return true;
        }

        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<int> coin(0, 1);

        const int gridSize = Mod::Tuning::kArenaNoSeePlayerGridSize;
        const int half = (gridSize - 1) / 2;
        const float cell = Mod::Tuning::kArenaNoSeePlayerGridCellSize;

        SDK::FVector currentBase = baseLocation;

        // We repeat by re-centering on a rear corner when no grid point works.
        for (int repeat = 0; repeat < Mod::Tuning::kArenaNoSeePlayerMaxRepeats; ++repeat)
        {
            SDK::FVector rearDir;
            rearDir.X = currentBase.X - playerLoc.X;
            rearDir.Y = currentBase.Y - playerLoc.Y;
            rearDir.Z = 0.0f;

            const double lenSq = (double)rearDir.X * (double)rearDir.X + (double)rearDir.Y * (double)rearDir.Y;
            if (lenSq < 1e-6)
            {
                return false;
            }

            const double invLen = 1.0 / std::sqrt(lenSq);
            rearDir.X = (float)((double)rearDir.X * invLen);
            rearDir.Y = (float)((double)rearDir.Y * invLen);

            SDK::FVector rightDir;
            rightDir.X = -rearDir.Y;
            rightDir.Y = rearDir.X;
            rightDir.Z = 0.0f;

            // Lateral scan order: center -> outward.
            int lateralOrder[5] = { 0, -1, 1, -2, 2 };

            // Probe a 5x5 grid behind the base location relative to the player.
            for (int rearStep = 1; rearStep <= gridSize; ++rearStep)
            {
                for (int li = 0; li < gridSize; ++li)
                {
                    const int latStep = (gridSize == 5) ? lateralOrder[li] : (li - half);

                    SDK::FVector candidate = currentBase;
                    candidate.X += rearDir.X * (cell * (float)rearStep) + rightDir.X * (cell * (float)latStep);
                    candidate.Y += rearDir.Y * (cell * (float)rearStep) + rightDir.Y * (cell * (float)latStep);
                    candidate.Z = playerLoc.Z;

                    if (IsInEscapeDirectionSafe(playerLoc, candidate))
                    {
                        continue;
                    }

                    if (!TryProjectToGround(world, playerPawn, candidate))
                    {
                        continue;
                    }

                    if (IsLocationInPlayerLineOfSight(world, candidate))
                    {
                        continue;
                    }

                    if (DoesLocationHaveLineOfSightToPlayer(world, candidate))
                    {
                        continue;
                    }

                    outLocation = candidate;
                    return true;
                }
            }

            // No point worked: pick the furthest rear corner (left/right) and re-center, up to 10 times.
            const bool chooseRight = (coin(gen) == 1);
            const float rearDist = cell * (float)gridSize;
            const float latDist = cell * (float)half * (chooseRight ? 1.0f : -1.0f);

            SDK::FVector corner = currentBase;
            corner.X += rearDir.X * rearDist + rightDir.X * latDist;
            corner.Y += rearDir.Y * rearDist + rightDir.Y * latDist;
            corner.Z = playerLoc.Z;

            // Best-effort ground projection so subsequent repeats use reasonable Z.
            (void)TryProjectToGround(world, playerPawn, corner);
            currentBase = corner;

            // If the chosen corner itself already blocks LoS, accept it immediately.
            if (!IsInEscapeDirectionSafe(playerLoc, currentBase) &&
                !IsLocationInPlayerLineOfSight(world, currentBase) &&
                !DoesLocationHaveLineOfSightToPlayer(world, currentBase))
            {
                outLocation = currentBase;
                return true;
            }
        }

        return false;
    }

    ArenaSubsystem* ArenaSubsystem::Get()
    {
        return g_Arena;
    }

    struct RadiusAICharacterBase_OnDeath
    {
        const SDK::AController* DeathInstigator;
        const SDK::AActor* DiedPawn;
    };

    static bool Hook_OnDeath(SDK::UObject* object, SDK::UFunction* function, void* parms, Mod::HookManager::ProcessEventFn originalFn)
    {
        (void)function;
        if (g_Arena && g_Arena->IsActive() && parms && object && object->IsA(SDK::ARadiusAICharacterBase::StaticClass()))
        {
            Mod::ScopedProcessEventGuard guard;
            auto* params = static_cast<RadiusAICharacterBase_OnDeath*>(parms);
            if (params->DiedPawn)
            {
                g_Arena->OnNPCDeath(const_cast<SDK::AActor*>(params->DiedPawn));
            }
            else
            {
                g_Arena->OnNPCDeath(static_cast<SDK::AActor*>(object));
            }
        }
        return false;
    }

    void ArenaSubsystem::Initialize(Mod::AI::AISubsystem *aiSubsystem)
    {
        aiSubsystem_ = aiSubsystem;
        g_Arena = this;
        
        Mod::HookManager::Get().RegisterNamedHook("OnDeath", &Hook_OnDeath);
        
        LoadNPCList();
        LOG_INFO("[Arena] Initialized and registered OnDeath hook. Discovered types: " << discoveredNPCs_.size());
    }

    void ArenaSubsystem::LoadNPCList()
    {
        std::lock_guard<std::recursive_mutex> lock(discoveredNPCsMutex_);
        discoveredNPCs_.clear();
        std::ifstream file("npcs.txt");
        if (file.is_open())
        {
            std::string line;
            bool oldFormatDetected = false;
            while (std::getline(file, line))
            {
                if (!line.empty())
                {
                    // Filter out short names (old format)
                    if (line.find('/') == std::string::npos)
                    {
                        oldFormatDetected = true;
                        continue;
                    }
                    discoveredNPCs_.push_back(line);
                }
            }
            file.close();
            
            if (oldFormatDetected)
            {
                LOG_WARN("[Arena] Detected old NPC list format (short names). Clearing invalid entries and saving.");
                if (!discoveredNPCs_.empty())
                {
                    // We want to drop short-name entries from the file, so rewrite it directly
                    std::ofstream outfile("npcs.txt", std::ios::trunc);
                    if (outfile.is_open())
                    {
                        for (const auto& name : discoveredNPCs_)
                        {
                            outfile << name << "\n";
                        }
                        outfile.close();
                    }
                }
            }
            
            LOG_INFO("[Arena] Loaded " << discoveredNPCs_.size() << " NPC types from npcs.txt");
        }
        else
        {
            // Initial fallback list - use paths if known, otherwise empty to force discovery
            // We can't easily guess paths for these. Better to let discovery fill it.
            // Or provided known paths if we knew them.
            // For now, let's include the common ones with a guess path prefix if we can't discover?
            // Actually, without paths we can't load. So fallback list of short names is useless for unloaded spawning.
            // We'll leave it empty or comment it out if we want to rely on scan.
            // But to be safe, let's keep the user's fallback but maybe warn?
            // "BP_RadiusNPCCharacterMimicScout_C" -> Scan logic will pick these up if they spawn naturally.
            // But we can't spawn them manually if not loaded.
            
            // Let's rely on ScanForNPCs to populate valid paths.
             discoveredNPCs_ = {};
             // The user had a fallback list. Maybe I should try to construct paths for them?
             // e.g. /Game/IntoTheRadius2/Blueprints/AI/Job/BP_RadiusNPCCharacterMimicScout.BP_RadiusNPCCharacterMimicScout_C ?
             // We don't know the path.
            
            LOG_INFO("[Arena] npcs.txt not found, starting with empty list. Waiting for discovery.");
        }
    }

    void ArenaSubsystem::SaveNPCList()
    {
        std::vector<std::string> snapshot;
        {
            std::lock_guard<std::recursive_mutex> lock(discoveredNPCsMutex_);
            snapshot = discoveredNPCs_;
        }

        if (snapshot.empty())
        {
            LOG_INFO("[Arena] Skip saving npcs.txt (no discovered NPCs)");
            return;
        }

        // Read existing entries so we can merge without losing anything that might
        // no longer be present in discoveredNPCs_ for whatever reason.  The goal is
        // to treat the file as an append-only history of every unique NPC path
        // we've ever seen.
        std::unordered_set<std::string> allNPCs;
        {
            std::ifstream infile("npcs.txt");
            std::string line;
            while (std::getline(infile, line))
            {
                if (!line.empty())
                {
                    allNPCs.insert(line);
                }
            }
        }

        bool addedAny = false;
        for (const auto& name : snapshot)
        {
            if (allNPCs.insert(name).second)
            {
                addedAny = true; // this was a new NPC we haven't persisted yet
            }
        }

        if (!addedAny)
        {
            LOG_INFO("[Arena] npcs.txt already up to date");
            return;
        }

        // Rewrite the file with the merged set.  We truncate and dump the union so
        // that we don't accidentally grow the file with duplicates when appending.
        std::ofstream file("npcs.txt", std::ios::trunc);
        if (file.is_open())
        {
            for (const auto& name : allNPCs)
            {
                file << name << "\n";
            }
            file.close();
        }
    }

    void ArenaSubsystem::ScanForNPCs(SDK::UWorld* world)
    {
        if (!world) return;

        Mod::ScopedProcessEventGuard guard;

        SDK::TArray<SDK::AActor*> actors;
        SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);

        bool newFound = false;
        {
            const int32_t count = GetTArrayNum(actors);
            SDK::AActor** data = GetTArrayData(actors);
            std::lock_guard<std::recursive_mutex> lock(discoveredNPCsMutex_);
            for (int32_t i = 0; i < count; ++i)
            {
                SDK::AActor* actor = data[i];
                if (!actor || !actor->Class) continue;

                // Use GetPathName to obtain the package-qualified blueprint path (works even when GetFullName lacks /Game prefix)
                std::string className = SDK::UKismetSystemLibrary::GetPathName(actor->Class).ToString();
                if (className.empty())
                {
                    continue;
                }

                // Exclude fragments and mimicbase
                std::string lowerName = className;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find("fragment") != std::string::npos || lowerName.find("mimicbase") != std::string::npos)
                {
                    continue;
                }

                if (std::find(discoveredNPCs_.begin(), discoveredNPCs_.end(), className) == discoveredNPCs_.end())
                {
                    LOG_INFO("[Arena] Discovered and persisted new NPC type path: " << className);
                    discoveredNPCs_.push_back(className);
                    newFound = true;
                }
            }
        }

        if (newFound)
        {
            SaveNPCList();
        }
    }

    void ArenaSubsystem::Start(int enemiesPerWave, float distance)
    {
        enemiesPerWave_ = enemiesPerWave;
        distance_ = (std::max)(distance, Mod::Tuning::kArenaMinSpawnDistance);
        wave_ = 0;

        SDK::UWorld *world = SDK::UWorld::GetWorld();
        if (!world) return;

        // Apply selected loadout if set
        auto* loadoutSubsystem = Loadout::LoadoutSubsystem::Get();
        if (loadoutSubsystem)
        {
            std::string selectedLoadout = loadoutSubsystem->GetSelectedLoadout();
            if (!selectedLoadout.empty() && loadoutSubsystem->LoadoutExists(selectedLoadout))
            {
                LOG_INFO("[Arena] Applying loadout: " << selectedLoadout);
                std::string result = loadoutSubsystem->ApplyLoadout(world, selectedLoadout);
                LOG_INFO("[Arena] Loadout result: " << result);
            }
        }

        LOG_INFO("[Arena] Starting arena with " << enemiesPerWave_ << " enemies at " << distance_ << " units");
        StartWave(world);
    }

    void ArenaSubsystem::Stop()
    {
        SDK::UWorld *world = SDK::UWorld::GetWorld();
        if (world)
        {
            LockTimeOfDay(world, false);
        }

        active_.store(false);
        wavePending_ = false;
        enemiesToSpawn_ = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
            activeEnemies_.clear();
        }
        LOG_INFO("[Arena] Stopped");
        Mod::ModFeedback::ShowMessage(L"Arena Stopped", 2.0f, SDK::FLinearColor{1.0f, 0.4f, 0.4f, 1.0f});
    }

    void ArenaSubsystem::StartWave(SDK::UWorld* world)
    {
        if (!world || !aiSubsystem_ || !aiSubsystem_->IsInitialized()) 
        {
            LOG_WARN("[Arena] Cannot start wave - world null or AISubsystem not ready");
            return;
        }

        Mod::ScopedProcessEventGuard guard;

        ++wave_;
        active_.store(true);
        wavePending_ = false;
        isPreSpawning_ = false;

        // Capture the player's "escape direction" once per wave, so we can prevent spawns/teleports
        // from appearing on that side even if the player later turns around.
        hasWaveEscapeForward_ = false;
        {
            SDK::FVector viewLoc{};
            SDK::FRotator viewRot{};
            if (Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
            {
                SDK::FVector forward = FlattenRotation(viewRot);
                forward.Z = 0.0f;
                const double lenSq = (double)forward.X * (double)forward.X + (double)forward.Y * (double)forward.Y;
                if (lenSq > 1e-6)
                {
                    const double invLen = 1.0 / std::sqrt(lenSq);
                    waveEscapeForward_.X = (float)((double)forward.X * invLen);
                    waveEscapeForward_.Y = (float)((double)forward.Y * invLen);
                    waveEscapeForward_.Z = 0.0f;
                    hasWaveEscapeForward_ = true;
                    LOG_INFO("[Arena] Wave escape direction captured: X=" << waveEscapeForward_.X << " Y=" << waveEscapeForward_.Y);
                }
            }
        }

        // Defensive sweep to avoid stale pointers before we reuse pre-spawned enemies
        PruneInvalidEnemies(world);

        // Any pre-spawned enemies should now be moved to their final positions
        int preSpawnedCount = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
            SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
            
            preSpawnedCount = (int)activeEnemies_.size();
            
            if (player)
            {
                SDK::FVector playerLoc = player->K2_GetActorLocation();
                // Number of enemies already spawned in the pre-spawn phase
                size_t numEnemies = activeEnemies_.size();
                float angleStep = 360.0f / static_cast<float>(numEnemies > 0 ? numEnemies : (size_t)Mod::Tuning::kArenaSpawnAngleFallbackDivisor);
                
                for (size_t i = 0; i < numEnemies; ++i)
                {
                    SDK::AActor* actor = activeEnemies_[i];
                    if (!actor) continue;

                    SDK::FVector finalLoc = actor->K2_GetActorLocation();
                    bool foundNonVisible = false;

                    // Try a few offsets to avoid popping into the player's view at wave start.
                    for (int attempt = 0; attempt < Mod::Tuning::kArenaCandidateSearchAttempts; ++attempt)
                    {
                        const float dist = distance_ + (float)attempt * Mod::Tuning::kArenaRepositionDistanceStep;
                        const float angleOffsetDeg = (attempt == 0)
                            ? 0.0f
                            : (((attempt % 2) == 1) ? 1.0f : -1.0f) * Mod::Tuning::kArenaRepositionAngleStepDeg * (float)((attempt + 1) / 2);

                        const float angleRad = ((static_cast<float>(i) * angleStep) + angleOffsetDeg) * (3.14159265f / 180.0f);

                        SDK::FVector candidate = playerLoc;
                        candidate.X += std::cos(angleRad) * dist;
                        candidate.Y += std::sin(angleRad) * dist;
                        candidate.Z = playerLoc.Z;

                        if (IsInEscapeDirection(playerLoc, candidate))
                        {
                            continue;
                        }

                        // Ground trace for final position.
                        SDK::FVector traceStart = { candidate.X, candidate.Y, candidate.Z + Mod::Tuning::kArenaGroundTraceUp };
                        SDK::FVector traceEnd   = { candidate.X, candidate.Y, candidate.Z - Mod::Tuning::kArenaGroundTraceDown };
                        SDK::FHitResult hit{};
                        SDK::TArray<SDK::AActor*> ignore;
                        ignore.Add(player);

                        if (!SDK::UKismetSystemLibrary::LineTraceSingle(world, traceStart, traceEnd, SDK::ETraceTypeQuery::TraceTypeQuery1, false, ignore, SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f))
                        {
                            continue;
                        }

                        candidate.Z = hit.ImpactPoint.Z + Mod::Tuning::kArenaGroundSpawnZOffset;

                        if (IsLocationInPlayerLineOfSight(world, candidate))
                        {
                            continue;
                        }

                        SDK::FVector adjusted = candidate;
                        if (!TryFindPlacementWhereNpcCannotSeePlayer(world, player, playerLoc, candidate, adjusted))
                        {
                            continue;
                        }

                        candidate = adjusted;

                        finalLoc = candidate;
                        foundNonVisible = true;
                        break;
                    }

                    if (!foundNonVisible)
                    {
                        LOG_WARN("[Arena] Wave-start reposition: could not find LoS-safe spot for pre-spawned NPC " << actor->GetName() << "; leaving at pre-spawn location");
                    }
                    else
                    {
                        const SDK::FRotator lookRot = SDK::UKismetMathLibrary::FindLookAtRotation(finalLoc, playerLoc);
                        actor->K2_SetActorLocationAndRotation(finalLoc, lookRot, false, nullptr, true);
                    }
                    
                    // Ensure they are in combat state
                    if (actor->IsA(SDK::APawn::StaticClass()))
                    {
                        auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(actor);
                        if (aiChar->AIController)
                        {
                            aiChar->AIController->SetNPCState(SDK::ENPCState::Combat);
                        }
                    }
                }
            }
        }

        LOG_INFO("[Arena] StartWave: resetting stats");
        ResetWaveStats();
        
        // Ensure we have a fresh discovery pass when starting a wave (important right after level load)
        ScanForNPCs(world);

        float time = SDK::UGameplayStatics::GetTimeSeconds(world);
        waveStartTime_ = time;
        lastProximityNoticeTime_ = 0.0f;
        lastSpawnTime_ = 0.0f;
        lastScanTime_ = time; // Align scan timer with the immediate scan above

        LOG_INFO("[Arena] StartWave: setting attack limit");
        SetGroupAttackLimit(world, Mod::Tuning::kArenaMaxConcurrentAttackers);
        LockTimeOfDay(world, true);
        
        spawnedInCurrentWave_ = preSpawnedCount;
        enemiesToSpawn_ = enemiesPerWave_ - preSpawnedCount;

        LOG_INFO("[Arena] Wave " << wave_ << " initialized. Spawning " << enemiesToSpawn_ 
            << " enemies (PreSpawned: " << preSpawnedCount << ")...");
        Mod::ModFeedback::ShowMessage(L"Wave Started!", 3.0f, SDK::FLinearColor{0.4f, 0.8f, 1.0f, 1.0f});
    }

    void ArenaSubsystem::SpawnOneNPC(SDK::UWorld* world, bool farAway)
    {
        if (!world || enemiesToSpawn_ <= 0) return;

        Mod::ScopedProcessEventGuard guard;

        std::vector<std::string> pool;
        {
            std::lock_guard<std::recursive_mutex> lock(discoveredNPCsMutex_);
            pool = discoveredNPCs_;
        }
        if (pool.empty()) 
        {
            LOG_WARN("[Arena] No NPCs discovered yet, cannot spawn");
            return;
        }

        static std::random_device rd;
        static std::mt19937 gen(rd());

        SDK::FVector playerLoc{};
        SDK::FRotator playerRot{};
        if (!Mod::GameContext::GetPlayerView(world, playerLoc, playerRot)) 
        {
            LOG_WARN("[Arena] Could not get player view for spawning");
            return;
        }

        float angleStep = 360.0f / static_cast<float>(enemiesPerWave_ > 0 ? enemiesPerWave_ : 10);
        float baseAngleDeg = static_cast<float>(spawnedInCurrentWave_) * angleStep;

        float spawnDist = farAway ? (distance_ + Mod::Tuning::kArenaPreSpawnExtraDistance) : distance_;

        LOG_INFO("[Arena] Attempting to spawn NPC " << spawnedInCurrentWave_ + 1 << "/" << enemiesPerWave_ << (farAway ? " (Pre-spawn)" : ""));

        // Try multiple times to find a valid spawn location/type
        for (int attempt = 0; attempt < Mod::Tuning::kArenaSpawnMaxAttempts; ++attempt)
        {
            // Pick random class from pool
            std::uniform_int_distribution<> poolDis(0, (int)pool.size() - 1);
            std::string className = pool[poolDis(gen)];

            // Add some jitter to the location if previous attempts failed
            std::uniform_real_distribution<float> jitterDis(-Mod::Tuning::kArenaSpawnJitterXY, Mod::Tuning::kArenaSpawnJitterXY);
            const float attemptDist = spawnDist + (float)attempt * Mod::Tuning::kArenaRepositionDistanceStep;
            const float sideAngleOffsetDeg = (attempt == 0)
                ? 0.0f
                : (((attempt % 2) == 1) ? 1.0f : -1.0f) * Mod::Tuning::kArenaRepositionAngleStepDeg * (float)((attempt + 1) / 2);
            const float angleDeg = baseAngleDeg + sideAngleOffsetDeg + (attempt > 0 ? jitterDis(gen) : 0.0f);
            const float angleRad = angleDeg * (3.14159265f / 180.0f);

            const float offsetX = std::cos(angleRad) * attemptDist;
            const float offsetY = std::sin(angleRad) * attemptDist;

            SDK::FVector targetLoc;
            targetLoc.X = playerLoc.X + offsetX;
            targetLoc.Y = playerLoc.Y + offsetY;
            targetLoc.Z = playerLoc.Z;

            if (IsInEscapeDirection(playerLoc, targetLoc))
            {
                // Keep the wave pressure from one direction; never spawn into the wave's escape hemisphere.
                continue;
            }

            // Ground trace to ensure we aren't spawning in the void or deep underground
            SDK::FVector traceStart = { targetLoc.X, targetLoc.Y, targetLoc.Z + Mod::Tuning::kArenaGroundTraceUp };
            SDK::FVector traceEnd   = { targetLoc.X, targetLoc.Y, targetLoc.Z - Mod::Tuning::kArenaGroundTraceDown };
            SDK::FHitResult hit{};
            SDK::TArray<SDK::AActor*> ignore;
            SDK::APawn* playerPawn = Mod::GameContext::GetPlayerPawn(world);
            if (playerPawn) ignore.Add(playerPawn);

            bool hitGround = SDK::UKismetSystemLibrary::LineTraceSingle(world, traceStart, traceEnd, SDK::ETraceTypeQuery::TraceTypeQuery1, false, ignore, SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
            
            if (!hitGround)
            {
                continue;
            }

            targetLoc.Z = hit.ImpactPoint.Z + Mod::Tuning::kArenaGroundSpawnZOffset;

            if (IsLocationInPlayerLineOfSight(world, targetLoc))
            {
                // Try again: push further out or around to the side rather than spawning in front of the player.
                continue;
            }

            {
                SDK::FVector adjusted = targetLoc;
                if (!TryFindPlacementWhereNpcCannotSeePlayer(world, playerPawn, playerLoc, targetLoc, adjusted))
                {
                    continue;
                }
                targetLoc = adjusted;
            }

            SDK::FTransform transform = SDK::UKismetMathLibrary::MakeTransform(targetLoc, SDK::UKismetMathLibrary::FindLookAtRotation(targetLoc, playerLoc), {1, 1, 1});
            
            SDK::UClass* cls = SDK::UObject::FindClass(className);
            if (!cls && className.find('/') != std::string::npos)
            {
                LOG_INFO("[Arena] Class " << className << " not found in memory. Attempting to load via SoftClassPath (blocking)...");

                // Let the engine parse the path string instead of manual struct fill.
                std::wstring wPath(className.begin(), className.end());
                SDK::FSoftClassPath softPath = SDK::UKismetSystemLibrary::MakeSoftClassPath(SDK::FString(wPath.c_str()));
                auto softRef = SDK::UKismetSystemLibrary::Conv_SoftClassPathToSoftClassRef(softPath);
                cls = SDK::UKismetSystemLibrary::LoadClassAsset_Blocking(softRef);

                if (cls)
                {
                    LOG_INFO("[Arena] Successfully loaded class via LoadClassAsset_Blocking: " << className);
                }
                else
                {
                    LOG_WARN("[Arena] Failed to load class via LoadClassAsset_Blocking: " << className);
                }
            }

            if (!cls) 
            {
                LOG_WARN("[Arena] Could not find class: " << className);
                continue;
            }

            LOG_INFO("[Arena] Spawning " << className << " at attempt " << attempt + 1);

            SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, cls, transform, SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);
            if (actor)
            {
                LOG_INFO("[Arena] FinishSpawningActor for " << className);
                SDK::UGameplayStatics::FinishSpawningActor(actor, transform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);
                
                if (actor->IsA(SDK::APawn::StaticClass()))
                {
                    LOG_INFO("[Arena] SpawnDefaultController for " << className);
                    static_cast<SDK::APawn*>(actor)->SpawnDefaultController();
                    auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(actor);
                    aiChar->GroupID = Mod::Tuning::kArenaNpcGroupId;
                    if (aiChar->AIController)
                    {
                        // Set to Idle if pre-spawning, Combat if direct spawning
                        aiChar->AIController->SetNPCState(farAway ? SDK::ENPCState::Idle : SDK::ENPCState::Combat);
                    }
                    
                    LOG_INFO("[Arena] BuffNPCAwareness for " << className);
                    BuffNPCAwareness(static_cast<SDK::APawn*>(actor));
                }

                {
                    std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
                    activeEnemies_.push_back(actor);
                }
                
                spawnedInCurrentWave_++;
                enemiesToSpawn_--;
                LOG_INFO("[Arena] SUCCESS: Spawned " << className << " (" << spawnedInCurrentWave_ << "/" << enemiesPerWave_ << ")");
                return; // Success!
            }
            else
            {
                LOG_WARN("[Arena] BeginDeferredActorSpawnFromClass returned null for " << className);
            }
        }

        LOG_WARN("[Arena] Failed to spawn NPC after " << Mod::Tuning::kArenaSpawnMaxAttempts << " attempts (invalid class or location)");
        // If we failed after 10 attempts, we should still decrement enemiesToSpawn_ or decide what to do
        // Let's decrement it so we don't try forever if the locations are all bad
        enemiesToSpawn_--; 
    }

    void ArenaSubsystem::BuffNPCAwareness(SDK::APawn* pawn)
    {
        if (!pawn || !pawn->IsA(SDK::ARadiusAICharacterBase::StaticClass())) return;
        
        Mod::ScopedProcessEventGuard guard;

        auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(pawn);
        if (aiChar->NPCConfig)
        {
            aiChar->NPCConfig->MaxSenseDistance = Mod::Tuning::kArenaNpcMaxSenseDistance;
            aiChar->NPCConfig->SoundProofMultiplierDefault = Mod::Tuning::kArenaNpcSoundProofMultiplierDefault;
            aiChar->NPCConfig->DelayToStartReduceDetectionScale = Mod::Tuning::kArenaNpcDelayToStartReduceDetectionScale;
            aiChar->NPCConfig->DetectReductionTime = Mod::Tuning::kArenaNpcDetectReductionTime; // Fast detection loss recovery
            aiChar->NPCConfig->SuspiciousActivityLevel = Mod::Tuning::kArenaNpcSuspiciousActivityLevel;
        }
    }

    void ArenaSubsystem::SetGroupAttackLimit(SDK::UWorld* world, int limit)
    {
        Mod::ScopedProcessEventGuard guard;
        auto* coord = static_cast<SDK::URadiusAICoordinationSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::URadiusAICoordinationSubsystem::StaticClass()));
        if (coord && coord->NPCConfig)
        {
            coord->NPCConfig->MaxNPCNumCanAttackAtTime = limit; // not entirely sure this is working, i'll need some tracing and stuff later.
            LOG_INFO("[Arena] Set MaxNPCNumCanAttackAtTime to " << limit);
        }
    }

    void ArenaSubsystem::PruneInvalidEnemies(SDK::UWorld* world)
    {
        (void)world;

        std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
        const size_t before = activeEnemies_.size();
        size_t removed = 0;

        activeEnemies_.erase(std::remove_if(activeEnemies_.begin(), activeEnemies_.end(), [this, &removed](SDK::AActor* actor)
        {
            if (!IsActorValidForArena(actor))
            {
                enemyStates_.erase(actor);
                ++removed;
                return true;
            }
            return false;
        }), activeEnemies_.end());

        if (removed > 0)
        {
            LOG_WARN("[Arena] Pruned " << removed << " invalid enemy refs (remaining=" << activeEnemies_.size() << ")");
        }
    }

    void ArenaSubsystem::LockTimeOfDay(SDK::UWorld* world, bool lock)
    {
        if (!world) return;

        Mod::ScopedProcessEventGuard guard;

        auto* timeSubsystem = static_cast<SDK::URadiusTimeSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::URadiusTimeSubsystem::StaticClass()));
        if (timeSubsystem && lock)
        {
            timeSubsystem->DebugSetHour(12.0f);
            float hour = timeSubsystem->GetCurrentHour();
            LOG_INFO("[Arena] Time subsystem hour now " << hour);
            timeSubsystem->AddSeconds(0); // nudge refresh
        }
        else if (!timeSubsystem)
        {
            LOG_WARN("[Arena] Time subsystem not found; cannot adjust time-of-day");
        }

        SDK::TArray<SDK::AActor*> controllers;
        SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusTimeController::StaticClass(), &controllers);
        const int32_t count = GetTArrayNum(controllers);
        SDK::AActor** data = GetTArrayData(controllers);
        int foundControllers = 0;
        for (int32_t i = 0; i < count; ++i)
        {
            auto* ctrl = static_cast<SDK::ARadiusTimeController*>(data[i]);
            if (!ctrl) continue;
            foundControllers++;
            ctrl->SetClockEnable(!lock);
            ctrl->SetDebugTimeScale(lock ? 0.0f : 1.0f);
        }

        LOG_INFO("[Arena] Time-of-day " << (lock ? "locked to midday" : "restored (clock enabled)") << ", controllers=" << foundControllers);
    }

    void ArenaSubsystem::Update(SDK::UWorld* world)
    {
        if (!world) return;

        Mod::ScopedProcessEventGuard guard;

        PruneInvalidEnemies(world);

        float time = SDK::UGameplayStatics::GetTimeSeconds(world);

        if (active_.load() && enemiesToSpawn_ > 0 && (time - lastSpawnTime_ > Mod::Tuning::kArenaSpawnTickIntervalSeconds))
        {
            SpawnOneNPC(world, false);
            lastSpawnTime_ = time;
        }

        if (time - lastScanTime_ > Mod::Tuning::kArenaScanIntervalSeconds)
        {
            ScanForNPCs(world);
            lastScanTime_ = time;
        }

        if (active_.load())
        {
            if (time - lastMoveToPlayerTime_ > Mod::Tuning::kArenaMoveToPlayerIntervalSeconds)
            {
                InstructNPCsToMoveToPlayer(world);
                lastMoveToPlayerTime_ = time;
            }

            CheckAntiStuck(world);
            CheckProximityNotification(world);
        }

        if (wavePending_)
        {
            ProcessPreSpawning(world);

            nextWaveTimer_ -= SDK::UGameplayStatics::GetWorldDeltaSeconds(world);
            if (nextWaveTimer_ <= 0)
            {
                StartWave(world);
            }
        }
    }

    void ArenaSubsystem::ProcessPreSpawning(SDK::UWorld* world)
    {
        if (!world || !isPreSpawning_ || enemiesToSpawn_ <= 0) return;

        float time = SDK::UGameplayStatics::GetTimeSeconds(world);
        // Stagger pre-spawning over the cooldown window
        // Spread the pre-spawns across a period that is derived from both the
        // arena cooldown and the number of enemies we intend to spawn.  Previously
        // we subtracted a fixed 0.5f which caused the total pre‑spawn window to
        // always equal the cooldown; for very large waves that meant the user had
        // to wait the full cooldown before all pre‑spawns finished.  By making the
        // interval a direct function of the count we can shrink the window when the
        // wave is big (more enemies => smaller spacing).
        float spawnInterval = Mod::Tuning::kArenaWaveCooldownSeconds /
            static_cast<float>(enemiesPerWave_ > 0 ? enemiesPerWave_ : 1);

        // Never let it get dangerously small; this avoids hammering SpawnOneNPC in
        // case of extreme enemy counts.
        if (spawnInterval < Mod::Tuning::kArenaMinPreSpawnInterval)
            spawnInterval = Mod::Tuning::kArenaMinPreSpawnInterval;
        
        if (time - lastPreSpawnTime_ > spawnInterval)
        {
            SpawnOneNPC(world, true);
            lastPreSpawnTime_ = time;
        }
    }

    void ArenaSubsystem::CheckAntiStuck(SDK::UWorld* world)
    {
        float time = SDK::UGameplayStatics::GetTimeSeconds(world);
        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return;
        SDK::FVector playerLoc = player->K2_GetActorLocation();

        SDK::FVector viewLoc{};
        SDK::FRotator viewRot{};
        (void)Mod::GameContext::GetPlayerView(world, viewLoc, viewRot);

        SDK::FRotator flatRot = viewRot;
        flatRot.Pitch = 0.0f;
        flatRot.Roll = 0.0f;
        const SDK::FVector viewRight = SDK::UKismetMathLibrary::GetRightVector(flatRot);

        bool waveOverdue = active_.load() && (time - waveStartTime_ > Mod::Tuning::kArenaOverdueWaveSeconds);

        std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
        std::vector<SDK::AActor*> stale;
        for (auto* actor : activeEnemies_)
        {
            if (!IsActorValidForArena(actor))
            {
                stale.push_back(actor);
                continue;
            }
            SDK::FVector currentPos = actor->K2_GetActorLocation();
            
            auto it = enemyStates_.find(actor);
            if (it == enemyStates_.end())
            {
                enemyStates_[actor] = { currentPos, time, 0.0f };
                continue;
            }

            // Never teleport or even accumulate “stuck” time for an NPC the player can currently see.
            if (IsLocationInPlayerLineOfSight(world, currentPos))
            {
                it->second.lastPos = currentPos;
                it->second.lastMoveTime = time;
                continue;
            }

            float distSq = SDK::UKismetMathLibrary::Vector_DistanceSquared(currentPos, it->second.lastPos);
            float distToPlayer = SDK::UKismetMathLibrary::Vector_Distance(currentPos, playerLoc);

            bool isImmobile = distSq < Mod::Tuning::kArenaStuckMoveDistanceSq; // (10 units)^2 movement threshold
            bool immobileFor10s = isImmobile && ((time - it->second.lastMoveTime) >= Mod::Tuning::kArenaStuckImmobileSeconds);

            // Overdue wave: treat static mimics as blockers and cull them to advance the wave.
            bool looksLikeMimic = false;
            {
                std::string name = actor->GetName();
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                looksLikeMimic = name.find("mimic") != std::string::npos;
            }

            if (immobileFor10s && waveOverdue && looksLikeMimic)
            {
                LOG_WARN("[Arena] Overdue static mimic culled: " << actor->GetName());
                OnNPCDeath(actor);
                actor->K2_DestroyActor();
                continue;
            }

            if (immobileFor10s)
            {
                // Rate-limit teleports per NPC to avoid spam.
                if ((time - it->second.lastTeleportTime) < Mod::Tuning::kArenaStuckTeleportCooldownSeconds)
                {
                    continue;
                }

                SDK::FVector dir = playerLoc - currentPos;
                double len = SDK::UKismetMathLibrary::VSize(dir);
                if (len < 1.0)
                {
                    dir = {1.0f, 0.0f, 0.0f};
                    len = 1.0;
                }
                dir = dir * static_cast<float>(1.0 / len);

                // Keep at least 10m from player; bias closer but not inside.
                // Windows headers define a problematic macro named "max" that can
                // turn "std::max" into "std.(" and crash the parser.  Wrap the
                // call in parentheses to suppress macro expansion.
                float targetDistance = (std::max)(Mod::Tuning::kArenaTeleportMinDistanceFromPlayer, distance_ * Mod::Tuning::kArenaTeleportDistanceFactor);

                SDK::FVector newPos{};
                bool foundNonVisible = false;

                // Try a few candidate positions, moving further away and/or to the side if the spot is in LoS.
                for (int attempt = 0; attempt < Mod::Tuning::kArenaCandidateSearchAttempts; ++attempt)
                {
                    const float attemptDistance = targetDistance + (float)attempt * Mod::Tuning::kArenaTeleportRetryDistanceStep;
                    const float sideAmount = (attempt == 0)
                        ? 0.0f
                        : (((attempt % 2) == 1) ? 1.0f : -1.0f) * Mod::Tuning::kArenaTeleportRetrySideStep * (float)((attempt + 1) / 2);

                    // Place on the NPC-side of the player (between NPC and player direction), not past the player.
                    SDK::FVector candidate = playerLoc - (dir * attemptDistance);
                    candidate.X += viewRight.X * sideAmount;
                    candidate.Y += viewRight.Y * sideAmount;
                    candidate.Z = playerLoc.Z;

                    // Ground trace to ensure we aren't teleporting into the void or deep underground
                    SDK::FVector traceStart = { candidate.X, candidate.Y, candidate.Z + Mod::Tuning::kArenaGroundTraceUp };
                    SDK::FVector traceEnd   = { candidate.X, candidate.Y, candidate.Z - Mod::Tuning::kArenaGroundTraceDown };
                    SDK::FHitResult hit{};
                    SDK::TArray<SDK::AActor*> ignore;
                    if (player) ignore.Add(player);

                    bool hitGround = SDK::UKismetSystemLibrary::LineTraceSingle(world, traceStart, traceEnd, SDK::ETraceTypeQuery::TraceTypeQuery1, false, ignore, SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
                    if (!hitGround)
                    {
                        continue;
                    }

                    candidate.Z = hit.ImpactPoint.Z + Mod::Tuning::kArenaGroundSpawnZOffset;

                    if (IsInEscapeDirection(playerLoc, candidate))
                    {
                        continue;
                    }

                    if (IsLocationInPlayerLineOfSight(world, candidate))
                    {
                        continue;
                    }

                    {
                        SDK::FVector adjusted = candidate;
                        if (!TryFindPlacementWhereNpcCannotSeePlayer(world, player, playerLoc, candidate, adjusted))
                        {
                            continue;
                        }
                        candidate = adjusted;
                    }

                    newPos = candidate;
                    foundNonVisible = true;
                    break;
                }

                if (!foundNonVisible)
                {
                    LOG_WARN("[Arena] Anti-stuck: skipping teleport for " << actor->GetName() << " (could not find non-visible destination)");
                    continue;
                }

                const SDK::FRotator lookRot = SDK::UKismetMathLibrary::FindLookAtRotation(newPos, playerLoc);
                actor->K2_SetActorLocationAndRotation(newPos, lookRot, false, nullptr, true);
                LOG_INFO("[Arena] Anti-stuck: Teleported NPC " << actor->GetName() << " towards player (LoS-safe, facing player)");
                
                it->second.lastPos = newPos;
                it->second.lastMoveTime = time;
                it->second.lastTeleportTime = time;
            }
            else
            {
                it->second.lastPos = currentPos;
                // only reset move timer when actor has actually moved
                if (!isImmobile)
                {
                    it->second.lastMoveTime = time;
                }
            }
        }

        if (!stale.empty())
        {
            activeEnemies_.erase(std::remove_if(activeEnemies_.begin(), activeEnemies_.end(), [&stale, this](SDK::AActor* actor)
            {
                if (std::find(stale.begin(), stale.end(), actor) != stale.end())
                {
                    enemyStates_.erase(actor);
                    return true;
                }
                return false;
            }), activeEnemies_.end());
            LOG_WARN("[Arena] Anti-stuck pruned " << stale.size() << " invalid enemy refs mid-loop");
        }
    }

    void ArenaSubsystem::CheckProximityNotification(SDK::UWorld* world)
    {
        float time = SDK::UGameplayStatics::GetTimeSeconds(world);
        if (time - waveStartTime_ < Mod::Tuning::kArenaProximityNoticeStartSeconds) return;

        // Only bother when the enemy count is already low (<3); otherwise the
        // player shouldn't be spammed with distance notices.
        {
            std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
            if (activeEnemies_.size() >= 3) return;
            if (activeEnemies_.empty()) return;
        }

        if (time - lastProximityNoticeTime_ < Mod::Tuning::kArenaProximityNoticeIntervalSeconds) return;

        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return;
        SDK::FVector playerLoc = player->K2_GetActorLocation();

        float minRadialDist = 999999.0f;
        
        {
            std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
            for (auto* actor : activeEnemies_)
            {
                if (!actor) continue;
                float d = SDK::UKismetMathLibrary::Vector_Distance(playerLoc, actor->K2_GetActorLocation());
                if (d < minRadialDist) minRadialDist = d;
            }
        }

        lastProximityNoticeTime_ = time;
        
        wchar_t msg[64]{};
        _snwprintf_s(msg, _TRUNCATE, L"Nearest Enemy: %.0fm", minRadialDist / 100.0f);
        Mod::ModFeedback::ShowMessage(msg, 2.0f, SDK::FLinearColor{1.0f, 0.8f, 0.2f, 1.0f});
    }

    void ArenaSubsystem::InstructNPCsToMoveToPlayer(SDK::UWorld* world)
    {
        SDK::APawn* player = Mod::GameContext::GetPlayerPawn(world);
        if (!player) return;

        Mod::ScopedProcessEventGuard guard;
        SDK::FVector playerLoc = player->K2_GetActorLocation();

        std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
        for (auto* actor : activeEnemies_)
        {
            if (!actor || !actor->IsA(SDK::ARadiusAICharacterBase::StaticClass())) continue;
            auto* aiChar = static_cast<SDK::ARadiusAICharacterBase*>(actor);
            if (aiChar->AIController)
            {
                    aiChar->AIController->MoveToLocation(playerLoc, Mod::Tuning::kArenaMoveToPlayerAcceptanceRadius, true, true, true, true, nullptr, true);
            }
        }
        LOG_INFO("[Arena] Instructed all NPCs to move to player position");
    }

    void ArenaSubsystem::OnNPCDeath(SDK::AActor* actor)
    {
        if (!active_.load() || !actor) return;

        Mod::ScopedProcessEventGuard guard;

        size_t remaining = 0;
        bool found = false;

        {
            std::lock_guard<std::recursive_mutex> lock(activeEnemiesMutex_);
            auto it = std::find(activeEnemies_.begin(), activeEnemies_.end(), actor);
            if (it != activeEnemies_.end())
            {
                activeEnemies_.erase(it);
                found = true;
            }
            
            auto itState = enemyStates_.find(actor);
            if (itState != enemyStates_.end())
            {
                enemyStates_.erase(itState);
            }

            remaining = activeEnemies_.size();
        }

        if (found)
        {
            // Only display a kill/remaining message at 5‑enemy intervals or when the
            // count has dropped into the final 3.  This cuts down on spam while still
            // giving the player a sense of progress.
            if (remaining <= 3 || (remaining % 5) == 0)
            {
                wchar_t msg[64]{};
                _snwprintf_s(msg, _TRUNCATE, L"Kill! Enemies left: %d", (int)remaining);
                Mod::ModFeedback::ShowMessage(msg, 1.5f, SDK::FLinearColor{1.0f, 0.2f, 0.2f, 1.0f});
            }

            if (remaining == 0 && enemiesToSpawn_ == 0)
            {
                CheckWaveCompletion(SDK::UWorld::GetWorld());
            }
        }
        else
        {
            LOG_WARN("[Arena] OnNPCDeath received for untracked actor " << actor->GetName());
        }
    }

    void ArenaSubsystem::CheckWaveCompletion(SDK::UWorld* world)
    {
        Mod::ScopedProcessEventGuard guard;

        float dmg = 0.0f;
        int bullets = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(statsMutex_);
            dmg = waveAccumulatedDamage_;
            bullets = waveBulletsFired_;
        }

        LOG_INFO("[Arena] Wave clear! Stats — dmg=" << dmg << ", bullets=" << bullets << ", wave=" << wave_ << ", enemiesPerWave=" << enemiesPerWave_);
        wchar_t msg[128]{};
        _snwprintf_s(msg, _TRUNCATE, L"Wave Cleared! Next wave in %.0fs...", Mod::Tuning::kArenaNextWaveDelaySeconds);
        Mod::ModFeedback::ShowMessage(msg, Mod::Tuning::kArenaNextWaveDelaySeconds, SDK::FLinearColor{0.4f, 1.0f, 0.4f, 1.0f});
        
        active_.store(false);
        wavePending_ = true;
        isPreSpawning_ = true;
        enemiesToSpawn_ = enemiesPerWave_;
        spawnedInCurrentWave_ = 0;
        nextWaveTimer_ = Mod::Tuning::kArenaNextWaveDelaySeconds;
    }

    void ArenaSubsystem::RecordPlayerDamageTaken(float damage)
    {
        std::lock_guard<std::recursive_mutex> lock(statsMutex_);
        waveAccumulatedDamage_ += damage;
    }

    void ArenaSubsystem::RecordBulletFired()
    {
        std::lock_guard<std::recursive_mutex> lock(statsMutex_);
        waveBulletsFired_++;
    }

    void ArenaSubsystem::ResetWaveStats()
    {
        std::lock_guard<std::recursive_mutex> lock(statsMutex_);
        waveAccumulatedDamage_ = 0.0f;
        waveBulletsFired_ = 0;
    }

    std::string ArenaSubsystem::GetStatus() const
    {
        std::ostringstream ss;
        ss << "Arena: Wave " << wave_ << " | Active: " << (active_.load() ? "Yes" : "No");
        return ss.str();
    }

    bool ArenaSubsystem::IsInEscapeDirection(const SDK::FVector& playerLoc, const SDK::FVector& location) const
    {
        if (!hasWaveEscapeForward_)
        {
            return false;
        }

        SDK::FVector to;
        to.X = location.X - playerLoc.X;
        to.Y = location.Y - playerLoc.Y;
        to.Z = 0.0f;

        const double dot = (double)to.X * (double)waveEscapeForward_.X + (double)to.Y * (double)waveEscapeForward_.Y;
        return dot > 0.0;
    }
}

