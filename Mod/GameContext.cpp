

#include "GameContext.hpp"

#include "Logging.hpp"

namespace Mod::GameContext
{
    namespace
    {
        // Global cached references to avoid expensive lookups (like GObjects scanning) every frame.
        // We use UKismetSystemLibrary::IsValid to check if these are still valid before use.
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* gCachedPlayer = nullptr;
        SDK::APlayerController* gCachedController = nullptr;

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
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* TryFindPlayerInGObjects()
        {
            if (!SDK::UObject::GObjects)
                return nullptr;

            for (int32_t i = 0; i < SDK::UObject::GObjects->Num(); ++i)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj)
                    continue;

                if (!obj->IsA(SDK::ABP_RadiusPlayerCharacter_Gameplay_C::StaticClass()))
                    continue;

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
        if (IsValidObject(gCachedPlayer))
            return gCachedPlayer;

        SDK::UWorld* world = GetWorld();

        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* fromStatics = TryGetPlayerFromGameplayStatics(world))
        {
            gCachedPlayer = fromStatics;
            return gCachedPlayer;
        }

        // Fallback: slow scan (cached after success)
        gCachedPlayer = TryFindPlayerInGObjects();
        return IsValidObject(gCachedPlayer) ? gCachedPlayer : nullptr;
    }

    /**
     * Gets the player controller, using a cached reference if possible.
     */
    SDK::APlayerController* GetPlayerController()
    {
        if (IsValidObject(gCachedController))
            return gCachedController;

        SDK::UWorld* world = GetWorld();
        gCachedController = TryGetControllerFromGameplayStatics(world);
        return IsValidObject(gCachedController) ? gCachedController : nullptr;
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
