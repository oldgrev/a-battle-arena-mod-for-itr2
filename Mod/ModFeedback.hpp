#pragma once

#include <string>
#include <cstdint>

#include "..\CppSDK\SDK.hpp"

namespace Mod::ModFeedback
{
    // Minimal VR-friendly feedback using UKismetSystemLibrary::PrintString.
    void ShowMessage(const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 1.0f, 1.0f});

    // Diagnostics: start/stop a staggered subtitle-format test sequence.
    // Triggered via TCP to explore length/linebreak/markup limits in one VR run.
    void StartSubtitleTestSequence(uint32_t intervalMs = 900);
    void StopSubtitleTestSequence();

    // Replays any queued messages that were suppressed during map load / main menu.
    // Called from ModMain::OnTick when the world is ready.
    void DrainPending();

    // World-space helper (optional; useful for debugging spawn points, arena zones, etc.).
    void ShowWorldText(const SDK::FVector& location, const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 0.0f, 1.0f});

    // Play a UI/2D sound for the local player.
    void PlaySound2D(SDK::USoundBase* sound, float volume = 1.0f, float pitch = 1.0f, bool isUiSound = true);

    // Play a 3D sound at a world location.
    void PlaySoundAtLocation(SDK::USoundBase* sound, const SDK::FVector& location, float volume = 1.0f, float pitch = 1.0f);
}
