

#include "ItemSubsystem.hpp"

#include <sstream>
#include <chrono>
#include <thread>

#include "..\CppSDK\SDK.hpp"
#include "Logging.hpp"

namespace Mod
{
    namespace Items
    {
        // Local mathematical and Unreal helper functions used for item spawning.
        namespace Helpers
        {
            /**
             * Retrieves the primary player controller.
             */
            SDK::APlayerController *GetPrimaryController(SDK::UWorld *world)
            {
                if (!world)
                {
                    return nullptr;
                }

                if (world->OwningGameInstance && world->OwningGameInstance->LocalPlayers.Num() > 0)
                {
                    SDK::ULocalPlayer* localPlayer = world->OwningGameInstance->LocalPlayers[0];
                    if (localPlayer)
                    {
                        return localPlayer->PlayerController;
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
             * Gets the player's current view location and rotation.
             * Used to determine where to spawn items relative to the player's perspective.
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
             * Safely parses a string into a float value.
             */
            bool TryParseFloat(const std::string &value, float &outValue)
            {
                char *end = nullptr;
                outValue = std::strtof(value.c_str(), &end);
                return end && end != value.c_str() && *end == '\0';
            }
        }

        void ItemSubsystem::Initialize()
        {
            LOG_INFO("[Items] ItemSubsystem initialized");
            initialized_ = true;
        }

        void ItemSubsystem::Shutdown()
        {
            LOG_INFO("[Items] ItemSubsystem shutdown");
            initialized_ = false;
        }

        /**
         * Calculates a spawn location based on the player's current view.
         * Useful for spawning items directly in front of the player.
         */
        SDK::FVector ItemSubsystem::CalculateSpawnLocation(SDK::UWorld *world, float distance, float lateralOffset, float heightOffset)
        {
            SDK::FVector viewLocation{};
            SDK::FRotator viewRotation{};

            if (!Helpers::GetPlayerView(world, viewLocation, viewRotation))
            {
                LOG_ERROR("[Items] Failed to get player view");
                return SDK::FVector{};
            }

            SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(viewRotation);
            SDK::FVector right = SDK::UKismetMathLibrary::GetRightVector(viewRotation);

            SDK::FVector spawnLocation = viewLocation;
            spawnLocation.X += forward.X * distance + right.X * lateralOffset;
            spawnLocation.Y += forward.Y * distance + right.Y * lateralOffset;
            spawnLocation.Z += forward.Z * distance + right.Z * lateralOffset + heightOffset;

            return spawnLocation;
        }

        SpawnResult ItemSubsystem::SpawnItem(SDK::UWorld *world, const SpawnParams &params)
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
            SDK::FVector spawnLocation = CalculateSpawnLocation(world, params.distance, params.lateralOffset, params.heightOffset);
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

            // Items typically spawn upright
            SDK::FRotator spawnRotation{0, viewRotation.Yaw, 0};
            SDK::FVector spawnScale{1.0f, 1.0f, 1.0f};
            SDK::FTransform spawnTransform = SDK::UKismetMathLibrary::MakeTransform(spawnLocation, spawnRotation, spawnScale);

            // Find the item class
            SDK::UClass *itemClass = params.useFullName
                                         ? SDK::UObject::FindClass(params.className)
                                         : SDK::UObject::FindClassFast(params.className);

            if (!itemClass)
            {
                result.message = "Item class not found: " + params.className;
                return result;
            }

            LOG_INFO("[Items] Spawning item: " + params.className + " at distance " + std::to_string(params.distance));

            // Spawn the item
            SDK::AActor *spawned = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                itemClass,
                spawnTransform,
                SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
                nullptr,
                SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            if (!spawned)
            {
                result.message = "Failed to spawn item actor";
                return result;
            }

            // Complete spawning
            SDK::UGameplayStatics::FinishSpawningActor(spawned, spawnTransform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

            result.success = true;
            result.message = "Spawned: " + spawned->GetFullName();
            result.actor = spawned;

            LOG_INFO("[Items] " + result.message);
            return result;
        }

        SpawnResult ItemSubsystem::SpawnToInventory(SDK::UWorld *world, const SpawnParams &params)
        {
            SpawnResult result;

            // Note: Spawning directly to inventory is more complex as it requires
            // interacting with the item container system. For now, we'll spawn
            // in the world and let the player pick it up.
            //
            // To implement proper inventory spawning, we would need to:
            // 1. Get the player's inventory container
            // 2. Create the item dynamically
            // 3. Add it to the container
            //
            // For now, spawn at a small distance in front of the player

            SpawnParams worldParams = params;
            worldParams.distance = 50.0f; // Very close to player for easy pickup

            return SpawnItem(world, worldParams);
        }

        int ItemSubsystem::ClearItems(SDK::UWorld *world)
        {
            if (!world)
            {
                return 0;
            }

            // Get all items of base class
            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusItemBase::StaticClass(), &actors);

            int removed = 0;
            for (SDK::AActor *actor : actors)
            {
                if (!actor)
                {
                    continue;
                }

                // Skip player character and other important actors
                if (actor->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
                {
                    continue;
                }

                actor->K2_DestroyActor();
                removed++;
            }

            LOG_INFO("[Items] Cleared items: " + std::to_string(removed));
            return removed;
        }

        std::string ItemSubsystem::ListItems(SDK::UWorld *world, size_t maxLines)
        {
            if (!world)
            {
                return "World not ready";
            }

            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusItemBase::StaticClass(), &actors);

            std::ostringstream response;
            response << "Items: " << actors.Num();

            size_t shown = 0;
            for (SDK::AActor *actor : actors)
            {
                if (!actor)
                {
                    continue;
                }

                // Skip player
                if (actor->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
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
                         << actor->GetName()
                         << " @ X=" << location.X
                         << " Y=" << location.Y
                         << " Z=" << location.Z;
                shown++;
            }

            return response.str();
        }

        int ItemSubsystem::GetItemCount(SDK::UWorld *world)
        {
            if (!world)
            {
                return 0;
            }

            SDK::TArray<SDK::AActor *> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusItemBase::StaticClass(), &actors);
            return actors.Num();
        }
    }
}
