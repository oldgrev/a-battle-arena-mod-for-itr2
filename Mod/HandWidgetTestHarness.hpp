#pragma once

/*
AILEARNINGS:
- HandWidgetTestHarness is the integration point that creates and wires up the new
  portable widget system for testing.
- It creates HandWidget instances for left and right hands, sets up menus and notifications,
  creates multiplexers, and provides input routing.
- It's designed to be initialized from ModMain and ticked from ModMain::OnTick.
- TCP commands are registered for remote testing (e.g. "hwtest notif Hello World").
- For initial testing this runs ALONGSIDE the existing VRMenuSubsystem/ModFeedback.
  Once confirmed working, the old code can be replaced.
*/

#include <string>
#include <memory>
#include <vector>

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

        // Initialize the test harness. Call once from ModMain.
        void Initialize();

        // Tick — call from ModMain::OnTick(world).
        void Tick(SDK::UWorld* world);

        // Register test TCP commands.
        void RegisterCommands(Mod::CommandHandlerRegistry& registry);

        // --- Input routing (called from HookManager hooks) ---

        // Grip+B/Y = toggle menu. Returns true to suppress input.
        bool OnToggleMenu(SDK::UWorld* world);

        // Thumbstick Y for menu navigation. Returns true to suppress.
        bool OnNavigate(float thumbstickY);

        // B/Y without grip (when menu open) = select. Returns true to suppress.
        bool OnSelect();

        // Is the menu currently open?
        bool IsMenuOpen() const;

        // --- Cleanup ---
        void Shutdown();

    private:
        HandWidgetTestHarness() = default;

        bool initialized_ = false;

        // Left hand - two HandWidgets: one for menu layer, one for notification layer
        std::unique_ptr<HandWidget> leftMenuHW_;       // HandWidget for menu layer
        std::unique_ptr<HandWidget> leftNotifHW_;      // HandWidget for notification layer
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

        // Helpers
        void EnsureBindings(SDK::UWorld* world);
        void BuildTestMenuPages();

        // Command handlers
        std::string HandleTestCommand(SDK::UWorld* world, const std::vector<std::string>& args);
    };

} // namespace PortableWidget
