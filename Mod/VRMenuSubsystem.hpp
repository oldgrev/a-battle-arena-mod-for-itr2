#pragma once

/*
AILEARNINGS:
- SDK (Dumper-7) does not expose NewObject, StaticConstructObject_Internal, or CreateDefaultSubobject.
  We cannot programmatically create UObjects from scratch.
- SDK does not expose UWidgetTree::ConstructWidget. We cannot build widget trees from scratch.
- UWidgetComponent::Widget is at offset 0x0630.
- DrawDebugString is UNVERIFIED in VR. ShowWorldText wraps it but has zero callers — never tested.
  Debug draw pipeline may be stripped in VR/shipping builds. Only subtitles are confirmed visible.
- FString is non-owning. Must use MakeStableFString ring buffer pattern.
- The gameplay character has GrabSphereLeft_BP (UGrabSphere*) with K2_GetComponentLocation().
- The gameplay character has PlayerDebugWidget (UWidgetComponent*) already attached.
- FInputActionValue is 0x20 bytes, opaque padding. For buttons, the bool/float is at byte offset 0.
  For 2D thumbstick, X and Y floats are somewhere in the first 8 bytes (need runtime logging to verify).
- InpActEvt_IA_Button2_Left has two ProcessEvent variants (_17 and _18) — likely Started and Completed.
  Must debounce with timestamp.
- The hook system keys on UFunction::GetName() short names.
*/

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>
#include <mutex>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    // Forward declarations
    class Cheats;
    Cheats* GetCheats();

    // -------------------------------------------------------------------------
    // Menu item definition
    // -------------------------------------------------------------------------
    struct VRMenuItem
    {
        std::string label;                                      // Display name (e.g. "God Mode")
        std::function<std::string()> statusFn;                  // Returns current state (e.g. "ON"/"OFF")
        std::function<void()> actionFn;                         // Execute on select
    };

    // -------------------------------------------------------------------------
    // VR Menu Subsystem — manages state + two rendering approaches
    // -------------------------------------------------------------------------
    class VRMenuSubsystem
    {
    public:
        static VRMenuSubsystem* Get();

        void Initialize();

        // Called from ModMain::OnTick, on the game thread.
        void Update(SDK::UWorld* world);

        // --- Input hooks (called from HookManager named hooks) ---
        // Return true = suppress original input
        bool OnToggleMenu();                            // Left Y/B button
        bool OnNavigate(float thumbstickY);             // Left thumbstick Y axis
        bool OnSelect();                                // Left trigger

        bool IsMenuOpen() const { return menuOpen_.load(std::memory_order_relaxed); }

        // --- Approach selection ---
        void SetDebugStringEnabled(bool enabled) { debugStringEnabled_ = enabled; }
        void SetWidgetEnabled(bool enabled) { widgetEnabled_ = enabled; }
        bool IsDebugStringEnabled() const { return debugStringEnabled_; }
        bool IsWidgetEnabled() const { return widgetEnabled_; }

    private:
        VRMenuSubsystem() = default;

        void BuildMenuItems();

        // --- Approach 1: DrawDebugString ---
        void RenderDebugString(SDK::UWorld* world);

        // --- Approach 2: Widget hijack ---
        void RenderWidget(SDK::UWorld* world);
        void TryFindWidgetComponent(SDK::UWorld* world);

        // --- Shared state ---
        std::atomic<bool> menuOpen_{false};
        int selectedIndex_ = 0;
        std::vector<VRMenuItem> items_;
        bool initialized_ = false;

        // Debounce state for toggle
        std::chrono::steady_clock::time_point lastToggleTime_{};

        // Debounce state for navigation
        std::chrono::steady_clock::time_point lastNavTime_{};

        // Approach flags (both enabled by default; disable one if it crashes)
        bool debugStringEnabled_ = true;
        bool widgetEnabled_ = true;

        // Widget approach state
        SDK::UWidgetComponent* cachedWidgetComp_ = nullptr;
        SDK::UTextBlock* cachedTextBlock_ = nullptr;
        bool widgetSearched_ = false;
        bool widgetSearchFailed_ = false;

        // Ring buffer for stable FStrings
        static SDK::FString MakeStableFString(const std::wstring& value);
    };
}
