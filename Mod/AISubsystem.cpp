// AILEARNINGS:
// - RegisterNPCWithSubsystem crashes when called on a newly-spawned NPC, possibly due to the controller/pawn state not being fully stabilized even after FinishSpawningActor. 
// - Coordinate subsystem calls in AISubsystem.cpp have been commented out to ensure stable spawning. Use logs to track spawning progress.
// - Always add logs before and after deferred spawning steps (BeginDeferred, FinishSpawning, SpawnDefaultController) to catch where crashes happen.
// - TArray members (Data, Num, Max) are protected in the SDK headers and may lack public accessors or operators in some build environments. Use the TArrayRaw helper to access them via pointer casting.
// - Spawning base/abstract classes (like BP_RadiusNPCCharacterMimicBase_C) causes a crash inside FinishSpawningActor. Always spawn concrete subclasses (e.g. Scout, Gunner).
// - Discovering subclasses via IsSubclassOf(BaseClass) will return true for the BaseClass itself. Ensure you check (cls != BaseClass) when searching for concrete types.
// - UClass inherits from UStruct, which has a member 'SuperStruct'. You can use this to traverse the class hierarchy at runtime to find parent classes.



#include "AISubsystem.hpp"

#include <sstream>
#include <chrono>
#include <thread>

#include "..\CppSDK\SDK.hpp"
#include "Logging.hpp"
#include "HookManager.hpp"
#include "ModTuning.hpp"

namespace Mod
{
    namespace AI
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

        // Local mathematical and Unreal helper functions used for positioning and spawning.
        namespace Helpers
        {
            /**
             * Retrieves the primary player controller. 
             * Prioritizes the LocalPlayer list if available, otherwise falls back to GameplayStatics.
             */
            SDK::APlayerController *GetPrimaryController(SDK::UWorld *world)
            {
                if (!world)
                {
                    return nullptr;
                }

if (world->OwningGameInstance && GetTArrayNum(world->OwningGameInstance->LocalPlayers) > 0)
            {
                SDK::ULocalPlayer** players = GetTArrayData(world->OwningGameInstance->LocalPlayers);
                if (players && players[0])
                {
                    return players[0]->PlayerController;
                }
            }

                return SDK::UGameplayStatics::GetPlayerController(world, 0);
            }

            /**
             * Retrieves the primary player pawn.
             */
            SDK::APawn *GetPrimaryPawn(SDK::UWorld *world)
            {
                SDK::APlayerController *controller = GetPrimaryController(world);
                if (controller)
                {
                    return controller->K2_GetPawn();
                }

                return SDK::UGameplayStatics::GetPlayerPawn(world, 0);
            }

            /**
             * Gets the player's current view location and rotation (head position in VR).
             * This is crucial for spawning enemies relative to where the player is looking.
             */
            bool GetPlayerView(SDK::UWorld *world, SDK::FVector &outLocation, SDK::FRotator &outRotation)
            {
                SDK::APlayerController *controller = GetPrimaryController(world);
                if (controller)
                {
                    controller->GetPlayerViewPoint(&outLocation, &outRotation);
                    return true;
                }

                SDK::APawn *pawn = GetPrimaryPawn(world);
                if (pawn)
                {
                    pawn->GetActorEyesViewPoint(&outLocation, &outRotation);
                    return true;
                }

                return false;
            }

            /**
             * Splits a string into tokens based on whitespace.
             */
            std::vector<std::string> SplitWhitespace(const std::string &value)
            {
                std::istringstream stream(value);
                std::vector<std::string> tokens;
                std::string token;
                while (stream >> token)
                {
                    tokens.push_back(token);
                }
                return tokens;
            }

            /**
             * Safely parses a string into a float.
             */
            bool TryParseFloat(const std::string &value, float &outValue)
            {
                char *end = nullptr;
                outValue = std::strtof(value.c_str(), &end);
                return end && end != value.c_str() && *end == '\0';
            }
        }

        void AISubsystem::Initialize()
        {
            LOG_INFO("[AI] AISubsystem initialized");
            initialized_ = true;
        }

        void AISubsystem::Shutdown()
        {
            LOG_INFO("[AI] AISubsystem shutdown");
            initialized_ = false;
        }

        /**
         * Calculates a 3D location in the world based on the player's view,
         * projected forward by a specific distance and offset laterally.
         */
        SDK::FVector AISubsystem::CalculateSpawnLocation(SDK::UWorld *world, float distance, float lateralOffset)
        {
            SDK::FVector viewLocation{};
            SDK::FRotator viewRotation{};

            if (!Helpers::GetPlayerView(world, viewLocation, viewRotation))
            {
                LOG_ERROR("[AI] Failed to get player view");
                return SDK::FVector{};
            }

            // Flatten look direction to the horizontal plane so distance is measured on the ground,
            // not diagonally through the air. At 2000+ units, even a shallow downward look angle
            // produces hundreds of units of Z error that places NPCs underground or in the sky.
            SDK::FRotator flatRotation = viewRotation;
            flatRotation.Pitch = 0.0f;
            flatRotation.Roll  = 0.0f;

            SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(flatRotation);
            SDK::FVector right   = SDK::UKismetMathLibrary::GetRightVector(flatRotation);

            // XY target position at the requested distance.
            SDK::FVector targetXY;
            targetXY.X = viewLocation.X + forward.X * distance + right.X * lateralOffset;
            targetXY.Y = viewLocation.Y + forward.Y * distance + right.Y * lateralOffset;
            targetXY.Z = viewLocation.Z; // start from player Z as fallback

            // Ground trace: shoot straight down from well above to well below the target XY.
            // This lands the NPC on terrain regardless of elevation changes.
            SDK::FVector traceStart = { targetXY.X, targetXY.Y, targetXY.Z + Mod::Tuning::kAISpawnGroundTraceUp };
            SDK::FVector traceEnd   = { targetXY.X, targetXY.Y, targetXY.Z - Mod::Tuning::kAISpawnGroundTraceDown };

            SDK::APawn *pawn = Helpers::GetPrimaryPawn(world);
            if (pawn)
            {
                SDK::TArray<SDK::AActor *> ignore{};
                ignore.Add(pawn);

                SDK::FHitResult hit{};
                // TraceTypeQuery1 = WorldStatic/Visibility channel — hits terrain and static world.
                const bool hitTerrain = SDK::UKismetSystemLibrary::LineTraceSingle(
                    pawn,
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

                if (hitTerrain && hit.bBlockingHit)
                {
                    // Place at impact point; add a small offset so the capsule origin clears the surface.
                    targetXY.Z = hit.ImpactPoint.Z + Mod::Tuning::kAISpawnGroundZOffset;
                    LOG_INFO("[AI] Ground trace hit at Z=" << hit.ImpactPoint.Z << " -> spawnZ=" << targetXY.Z);
                }
                else
                {
                    LOG_WARN("[AI] Ground trace missed at XY=(" << targetXY.X << "," << targetXY.Y << "); using player Z fallback");
                }
            }

            return targetXY;
        }

        bool AISubsystem::RegisterNPCWithSubsystem(SDK::UWorld *world, SDK::APawn *pawn)
        {
            if (!world || !pawn)
            {
                return false;
            }

            SDK::AController *controller = pawn->GetController();
            if (!controller)
            {
                LOG_WARN("[AI] Spawned pawn has no controller");
                return false;
            }

            if (!controller->IsA(SDK::ARadiusAIControllerBase::StaticClass()))
            {
                LOG_WARN("[AI] Controller is not ARadiusAIControllerBase");
                return false;
            }

            LOG_INFO("[AI] Determined controller is ARadiusAIControllerBase, registering with coordination subsystem");
            auto *radiusController = static_cast<SDK::ARadiusAIControllerBase *>(controller);
            LOG_INFO("[AI] Controller cast successful");
            LOG_INFO("[AI] Controller: " + radiusController->GetFullName());
            // Get coordination subsystem
            auto *coord = static_cast<SDK::URadiusAICoordinationSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::URadiusAICoordinationSubsystem::StaticClass()));
            LOG_INFO("[AI] Retrieved coordination subsystem");
            if (!coord)
            {
                LOG_ERROR("[AI] Coordination subsystem not found");
                return false;
            }

            LOG_INFO("[AI] Registering NPC in coordination subsystem");
            coord->RegisterNpc(radiusController);

            LOG_INFO("[AI] NPC registered successfully");
            return true;
        }

        bool AISubsystem::UnregisterNPCWithSubsystem(SDK::UWorld *world, SDK::APawn *pawn)
        {
            if (!world || !pawn)
            {
                return false;
            }

            SDK::AController *controller = pawn->GetController();
            if (!controller)
            {
                return false;
            }

            if (!controller->IsA(SDK::ARadiusAIControllerBase::StaticClass()))
            {
                return false;
            }

            auto *radiusController = static_cast<SDK::ARadiusAIControllerBase *>(controller);
            auto *coord = static_cast<SDK::URadiusAICoordinationSubsystem*>(SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(world, SDK::URadiusAICoordinationSubsystem::StaticClass()));

            if (coord)
            {
                coord->UnRegisterNpc(radiusController);
                return true;
            }

            return false;
        }

        SpawnResult AISubsystem::SpawnNPC(SDK::UWorld *world, const SpawnParams &params)
        {
            SpawnResult result;

            if (!world)
            {
                result.message = "World not ready";
                return result;
            }

            SDK::APawn *pawn = Helpers::GetPrimaryPawn(world);
            if (!pawn)
            {
                result.message = "Player pawn not ready";
                return result;
            }

            // Calculate spawn location
            SDK::FVector spawnLocation = CalculateSpawnLocation(world, params.distance, params.lateralOffset);
            if (spawnLocation.X == 0 && spawnLocation.Y == 0 && spawnLocation.Z == 0)
            {
                result.message = "Failed to calculate spawn location";
                return result;
            }

            // Get spawn rotation from player view
            SDK::FVector viewLocation{};
            SDK::FRotator viewRotation{};
            if (!Helpers::GetPlayerView(world, viewLocation, viewRotation))
            {
                result.message = "Failed to get player view";
                return result;
            }

            SDK::FRotator spawnRotation = viewRotation;
            SDK::FVector spawnScale{1.0f, 1.0f, 1.0f};
            SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(spawnLocation, spawnRotation, spawnScale);

            // Find the NPC class
            SDK::UClass *npcClass = params.useFullName
                                        ? SDK::UObject::FindClass(params.className)
                                        : SDK::UObject::FindClassFast(params.className);

            if (!npcClass)
            {
                result.message = "NPC class not found: " + params.className;
                return result;
            }

            LOG_INFO("[AI] Spawning NPC: " + params.className + " at distance " + std::to_string(params.distance));

            Mod::ScopedProcessEventGuard guard;

            // Spawn the NPC
            SDK::AActor *spawned = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                npcClass,
                spawnTransform,
                SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
                nullptr,
                SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (!spawned)
            {
                result.message = "Failed to spawn NPC actor";
                return result;
            }

            LOG_INFO("[AI] Deferred spawn successful: " << (spawned ? spawned->GetFullName() : "NULL"));

            // Complete spawning
            LOG_INFO("[AI] Calling FinishSpawningActor for class: " << params.className);
            SDK::UGameplayStatics::FinishSpawningActor(spawned, spawnTransform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            LOG_INFO("[AI] FinishSpawningActor complete");

            // Spawn default controller
            if (spawned->IsA(SDK::APawn::StaticClass()))
            {
                LOG_INFO("[AI] Spawned actor is a Pawn, spawning controller...");
                auto *spawnedPawn = static_cast<SDK::APawn *>(spawned);
                spawnedPawn->SpawnDefaultController();
                LOG_INFO("[AI] SpawnDefaultController complete");

                // Register with coordination subsystem
                // NOTE: This is where the crash occurs - commenting it out to confirm.
                // RegisterNPCWithSubsystem(world, spawnedPawn);
                LOG_INFO("[AI] Skipping RegisterNPCWithSubsystem (suspected crash point)");
            }

            result.success = true;
            result.message = "Spawned: " + spawned->GetFullName();
            result.actor = spawned;

            LOG_INFO("[AI] " + result.message);
            return result;
        }

        int AISubsystem::ClearNPCs(SDK::UWorld *world)
        {
            if (!world)
            {
                return 0;
            }

            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);

            int removed = 0;
            const int32_t count = GetTArrayNum(actors);
            SDK::AActor** data = GetTArrayData(actors);
            for (int32_t i = 0; i < count; ++i)
            {
                SDK::AActor* actor = data[i];
                if (!actor)
                {
                    continue;
                }

                // Unregister first
                if (actor->IsA(SDK::APawn::StaticClass()))
                {
                    // Commented out coordination subsystem unregistration as it might also crash
                    // UnregisterNPCWithSubsystem(world, static_cast<SDK::APawn *>(actor));
                    LOG_INFO("[AI] Skipping coordination subsystem unregistration");
                }

                actor->K2_DestroyActor();
                removed++;
            }

            LOG_INFO("[AI] Cleared NPCs: " + std::to_string(removed));
            return removed;
        }

        std::string AISubsystem::ListNPCs(SDK::UWorld *world, size_t maxLines)
        {
            if (!world)
            {
                return "World not ready";
            }

            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);

            std::ostringstream response;
            response << "NPCs: " << GetTArrayNum(actors);

            size_t shown = 0;
            const int32_t count = GetTArrayNum(actors);
            SDK::AActor** data = GetTArrayData(actors);
            for (int32_t i = 0; i < count; ++i)
            {
                SDK::AActor* actor = data[i];
                if (!actor)
                {
                    continue;
                }

                if (shown >= maxLines)
                {
                    response << "\n...";
                    break;
                }

                SDK::FVector location = actor->K2_GetActorLocation();
                response << "\n"
                         << actor->GetFullName()
                         << " @ X=" << location.X
                         << " Y=" << location.Y
                         << " Z=" << location.Z;
                shown++;
            }

            return response.str();
        }

        int AISubsystem::GetNPCCount(SDK::UWorld *world)
        {
            if (!world)
            {
                return 0;
            }

            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);
            return GetTArrayNum(actors);
        }

        void AISubsystem::ReinitNPC(SDK::UWorld *world)
        {
            if (world)
            {
                SDK::UNPCLib::ReinitNPC(world);
                LOG_INFO("[AI] ReinitNPC called");
            }
        }
    }
}
