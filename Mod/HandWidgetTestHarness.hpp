#pragma once

/*
AILEARNINGS:
- HandWidgetTestHarness is the mod's integration point that wires the portable HandWidget
  system into the battle arena mod. It owns all HandWidget/Menu/Notification/Multiplexer
  instances, handles input routing, and provides a public ShowNotification() API that
  replaces the old ModFeedback::ShowOnRightWidget path.
- Initialization and Tick are called from ModMain. Input hooks route from HookManager.
- On level change, Shutdown() + Initialize() must be called to clear stale UObject pointers.
- Arena config state (enemy count, wave count) and friend class selection live here
  because they need to persist across menu open/close cycles.
*/

#include <string>
#include <memory>
#include <vector>
#include <atomic>

namespace SDK
{
    class UWorld;
    class APlayerController;
}

namespace Mod
{
    class CommandHandlerRegistry;
}

namespace PortableWidget
{
    class HandWidget;
    class HandWidgetNotification;
    class HandWidgetMenu;
    class HandWidgetMultiplexer;

    class HandWidgetTestHarness
    {
    public:
        static HandWidgetTestHarness* Get();

        // Initialize all subsystems. Call once from ModMain::Run().
        void Initialize();

        // Tick — call from ModMain::OnTick(world).
        void Tick(SDK::UWorld* world);

        // Register TCP commands (hwtest ...).
        void RegisterCommands(Mod::CommandHandlerRegistry& registry);

        // --- Public notification API (replaces ModFeedback::ShowOnRightWidget) ---
        void ShowNotification(const std::wstring& text, float durationSeconds = 3.0f);
        void ShowNotificationWithTitle(const std::wstring& title, const std::wstring& text, float durationSeconds = 3.0f);

        // --- Input routing (called from HookManager hooks) ---

        // Grip+B/Y = toggle menu. Returns true to suppress input.
        bool OnToggleMenu(SDK::UWorld* world);

        // Thumbstick Y for menu navigation. Returns true to suppress.
        bool OnNavigate(float thumbstickY);

        // B/Y without grip (when menu open) = select. Returns true to suppress.
        bool OnSelect();

        // Grip state tracking
        void OnGripPressed();
        void OnGripReleased();
        bool IsGripHeld() const;

        // Is the menu currently open?
        bool IsMenuOpen() const;

        // --- Cleanup ---
        void Shutdown();

    private:
        HandWidgetTestHarness() = default;

        bool initialized_ = false;

        // Left hand - two HandWidgets: one for menu layer, one for notification layer
        std::unique_ptr<HandWidget> leftMenuHW_;
        std::unique_ptr<HandWidget> leftNotifHW_;
        std::unique_ptr<HandWidgetMenu> leftMenu_;
        std::unique_ptr<HandWidgetNotification> leftNotif_;
        std::unique_ptr<HandWidgetMultiplexer> leftMux_;

        // Right hand - one HandWidget for notifications
        std::unique_ptr<HandWidget> rightNotifHW_;
        std::unique_ptr<HandWidgetNotification> rightNotif_;
        std::unique_ptr<HandWidgetMultiplexer> rightMux_;

        // State
        bool leftBound_ = false;
        bool rightBound_ = false;

        // Grip tracking for combo activation
        std::atomic<bool> gripHeld_{false};

        // --- Persistent menu state (survives menu close/reopen) ---
        int arenaEnemyCount_ = 200;
        int arenaWaveCount_ = 5;
        std::string selectedFriendClass_;

        // Helpers
        void EnsureBindings(SDK::UWorld* world);
        void BuildMenuPages();

        // Page builders
        void BuildMainPage();
        void BuildCheatsPage();
        void BuildArenaPage();
        void BuildLoadoutsPage();
        void BuildLoadoutSelectPage();
        void BuildSpawnFriendPage();
        void BuildFriendClassPage();
        void BuildNVGPage();
        void BuildNVGTuningPage();
        void BuildNVGLensPage();

        // Command handlers
        std::string HandleTestCommand(SDK::UWorld* world, const std::vector<std::string>& args);
    };

} // namespace PortableWidget
