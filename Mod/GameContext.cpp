

#include "GameContext.hpp"

#include "Logging.hpp"

#include <atomic>

namespace Mod::GameContext
{
    namespace
    {
        // Global cached references to avoid expensive lookups (like GObjects scanning) every frame.
        // We use UKismetSystemLibrary::IsValid to check if these are still valid before use.
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* gCachedPlayer = nullptr;
        SDK::APlayerController* gCachedController = nullptr;

        static std::atomic<int> gPlayerFallbackLogCount{0};

        /**
         * Checks if a UObject is valid using the engine's built-in validation.
         * This is safer than a raw null check as it handles pending-kill objects.
         */
        bool IsValidObject(const SDK::UObject* obj)
        {
            if (!obj)
                return false;

            // No WorldContext required for this specific Kismet call.
            return SDK::UKismetSystemLibrary::IsValid(obj);
        }

        bool IsValidNonDefaultObject(const SDK::UObject* obj)
        {
            if (!IsValidObject(obj))
                return false;

            // IMPORTANT: the class default object (CDO) is "valid" but not an in-world instance.
            // Calling gameplay events (like ShowSubtitles) on the CDO silently does nothing.
            return !obj->IsDefaultObject();
        }

        /**
         * Attempts to find the player character using standard Unreal gameplay statics.
         * This is the preferred "fast path" for finding the player.
         */
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* TryGetPlayerFromGameplayStatics(SDK::UWorld* world)
        {
            if (!world)
                return nullptr;

            SDK::APawn* pawn = SDK::UGameplayStatics::GetPlayerPawn(world, 0);
            if (!pawn)
                return nullptr;

            // Verify the pawn is actually the type we expect for ITR2.
            if (!pawn->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
                return nullptr;

            return static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C*>(pawn);
        }

        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* TryGetPlayerFromController(SDK::UWorld* world)
        {
            if (!world)
                return nullptr;

            SDK::APlayerController* controller = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!IsValidNonDefaultObject(controller))
                return nullptr;

            SDK::APawn* pawn = controller->K2_GetPawn();
            if (!IsValidNonDefaultObject(pawn))
                return nullptr;

            if (!pawn->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
                return nullptr;

            return static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C*>(pawn);
        }

        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* TryGetPlayerByActorOfClass(SDK::UWorld* world)
        {
            if (!world)
                return nullptr;

            SDK::AActor* actor = SDK::UGameplayStatics::GetActorOfClass(world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass());
            if (!IsValidNonDefaultObject(actor))
                return nullptr;

            return static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C*>(actor);
        }

        /**
         * Attempts to find the player controller using standard Unreal gameplay statics.
         */
        SDK::APlayerController* TryGetControllerFromGameplayStatics(SDK::UWorld* world)
        {
            if (!world)
                return nullptr;

            return SDK::UGameplayStatics::GetPlayerController(world, 0);
        }

        /**
         * A "slow path" fallback that scans every object in memory to find the player character.
         * Only used if normal gameplay statics fail (e.g. during specific loading states or if pointers are null).
         */
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* TryFindPlayerInGObjects(SDK::UWorld* world)
        {
            if (!SDK::UObject::GObjects)
                return nullptr;

            // If we don't have a world, do NOT scan: the first match is often the CDO (Default__)
            // which is "valid" but not usable for gameplay events.
            if (!IsValidObject(world))
                return nullptr;

            const std::string worldName = world->GetName();

            for (int32_t i = 0; i < SDK::UObject::GObjects->Num(); ++i)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj)
                    continue;

                if (!obj->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
                    continue;

                if (!IsValidNonDefaultObject(obj))
                    continue;

                // Heuristic: require the instance to belong to the current world.
                // Observed good instance full name contains: "L_Forest.L_Forest.PersistentLevel.BP_RadiusPlayerCharacter_Gameplay_C_..."
                if (!worldName.empty())
                {
                    const std::string full = obj->GetFullName();
                    if (full.find(worldName) == std::string::npos || full.find("PersistentLevel") == std::string::npos)
                        continue;
                }

                return static_cast<SDK::ABP_RadiusPlayerCharacter_Gameplay_C*>(obj);
            }

            return nullptr;
        }
    }

    SDK::UWorld* GetWorld()
    {
        // Wrapper for the Dumper-7 generated world getter.
        return SDK::UWorld::GetWorld();
    }

    /**
     * Gets the current player character, using a cached reference if possible.
     * If cache is invalid, it tries the fast path (GameplayStatics) before falling back to a global scan.
     */
    SDK::ABP_RadiusPlayerCharacter_Gameplay_C* GetPlayerCharacter()
    {
        if (IsValidNonDefaultObject(gCachedPlayer))
            return gCachedPlayer;

        SDK::UWorld* world = GetWorld();

        // Best path: controller -> pawn (matches how the engine routes local player ownership)
        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* fromController = TryGetPlayerFromController(world))
        {
            gCachedPlayer = fromController;
            return gCachedPlayer;
        }

        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* fromStatics = TryGetPlayerFromGameplayStatics(world))
        {
            gCachedPlayer = fromStatics;
            return gCachedPlayer;
        }

        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* fromActor = TryGetPlayerByActorOfClass(world))
        {
            int old = gPlayerFallbackLogCount.fetch_add(1);
            if (old < 10)
            {
                LOG_WARN("[GameContext] GetPlayerCharacter: GameplayStatics pawn lookup failed; recovered via GetActorOfClass. actor='" << fromActor->GetFullName() << "'");
            }
            gCachedPlayer = fromActor;
            return gCachedPlayer;
        }

        // Last resort: global scan (filtered to avoid the CDO and wrong-world instances)
        gCachedPlayer = TryFindPlayerInGObjects(world);
        if (IsValidNonDefaultObject(gCachedPlayer))
        {
            int old = gPlayerFallbackLogCount.fetch_add(1);
            if (old < 10)
            {
                LOG_WARN("[GameContext] GetPlayerCharacter: recovered via GObjects scan. player='" << gCachedPlayer->GetFullName() << "'");
            }
            return gCachedPlayer;
        }

        gCachedPlayer = nullptr;
        return nullptr;
    }

    /**
     * Gets the player controller, using a cached reference if possible.
     */
    SDK::APlayerController* GetPlayerController()
    {
        if (IsValidNonDefaultObject(gCachedController))
            return gCachedController;

        SDK::UWorld* world = GetWorld();
        gCachedController = TryGetControllerFromGameplayStatics(world);
        return IsValidNonDefaultObject(gCachedController) ? gCachedController : nullptr;
    }

    SDK::APawn* GetPlayerPawn(SDK::UWorld* world)
    {
        SDK::APlayerController* controller = GetPlayerController();
        if (controller)
        {
            return controller->K2_GetPawn();
        }

        if (!world) world = GetWorld();
        return world ? SDK::UGameplayStatics::GetPlayerPawn(world, 0) : nullptr;
    }

    bool GetPlayerView(SDK::UWorld* world, SDK::FVector& outLocation, SDK::FRotator& outRotation)
    {
        SDK::APlayerController* controller = GetPlayerController();
        if (controller)
        {
            controller->GetPlayerViewPoint(&outLocation, &outRotation);
            return true;
        }

        SDK::APawn* pawn = GetPlayerPawn(world);
        if (pawn)
        {
            pawn->GetActorEyesViewPoint(&outLocation, &outRotation);
            return true;
        }

        return false;
    }

    SDK::UObject* GetWorldContext()
    {
        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GetPlayerCharacter())
            return player;

        SDK::UWorld* world = GetWorld();
        return IsValidObject(world) ? world : nullptr;
    }

    void ClearCache()
    {
        gCachedPlayer = nullptr;
        gCachedController = nullptr;
    }
}
