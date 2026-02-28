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

        // Approach flags (POC9 is the working approach; others disabled by default)
        bool debugStringEnabled_ = false;
        bool widgetEnabled_ = false;

        // Widget approach state
        SDK::UWidgetComponent* cachedWidgetComp_ = nullptr;
        SDK::UTextBlock* cachedTextBlock_ = nullptr;
        bool widgetSearched_ = false;
        bool widgetSearchFailed_ = false;

        // POC9 approach state (WBP_Confirmation on left hand)
        bool poc9Enabled_ = true;
        SDK::UWBP_Confirmation_C* cachedPoc9Widget_ = nullptr;
        bool poc9WidgetCreated_ = false;

        void CreatePoc9Widget();
        void DestroyPoc9Widget();
        void UpdatePoc9Widget();

        // Ring buffer for stable FStrings
        static SDK::FString MakeStableFString(const std::wstring& value);
    };
}
