#pragma once

#include "..\CppSDK\SDK.hpp"

namespace Mod::GameContext
{
    // Best-effort world fetch. Safe to call often.
    SDK::UWorld* GetWorld();

    // Best-effort world context object suitable for Kismet calls (PrintString, DrawDebugString, etc.).
    SDK::UObject* GetWorldContext();

    // Returns the local player pawn if available.
    SDK::ABP_RadiusPlayerCharacter_Gameplay_C* GetPlayerCharacter();
    // Returns the player pawn if available.
    SDK::APawn* GetPlayerPawn(SDK::UWorld* world = nullptr);

    // Gets the player view location and rotation.
    bool GetPlayerView(SDK::UWorld* world, SDK::FVector& outLocation, SDK::FRotator& outRotation);
    // Returns the local player controller if available.
    SDK::APlayerController* GetPlayerController();

    // Clears cached references (e.g. on level change)
    void ClearCache();
}

