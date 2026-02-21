#pragma once

#include "..\CppSDK\SDK.hpp"

namespace Mod::ModFeedback
{
    // Minimal VR-friendly feedback using UKismetSystemLibrary::PrintString.
    void ShowMessage(const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 1.0f, 1.0f});

    // World-space helper (optional; useful for debugging spawn points, arena zones, etc.).
    void ShowWorldText(const SDK::FVector& location, const wchar_t* text, float seconds = 3.0f, const SDK::FLinearColor& color = {0.0f, 1.0f, 0.0f, 1.0f});
}
