/*
AILEARNINGS:
- HandWidgetTestHarness integrates the portable widget system into the existing mod.
- It creates separate HandWidget instances for menu (left hand) and notifications (right hand).
- The left hand uses a multiplexer so both menu AND notifications can share it.
- The right hand also has a multiplexer for notifications only (extensible).
- EnsureBindings() is called every tick to handle level transitions/player respawns.
- Test commands: "hwtest notif <text>", "hwtest menu", "hwtest clear", etc.
- During initial testing, this runs ALONGSIDE the existing systems. Once verified,
  the old VRMenuSubsystem and ModFeedback right-hand code can be replaced.
*/

#include "HandWidgetTestHarness.hpp"
#include "HandWidget.hpp"
#include "HandWidgetNotification.hpp"
#include "HandWidgetMenu.hpp"
#include "HandWidgetMultiplexer.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "CommandHandler.hpp"

#include "..\CppSDK\SDK.hpp"

#include <sstream>
#include <algorithm>

namespace PortableWidget
{
    // =========================================================================
    // Singleton
    // =========================================================================
    HandWidgetTestHarness* HandWidgetTestHarness::Get()
    {
        static HandWidgetTestHarness instance;
        return &instance;
    }

    // =========================================================================
    // Initialize
    // =========================================================================
    void HandWidgetTestHarness::Initialize()
    {
        if (initialized_)
        {
            LOG_INFO("[HWTest] Already initialized");
            return;
        }

        LOG_INFO("[HWTest] === Initializing HandWidget Test Harness ===");

        // --- Left hand: menu + notifications via multiplexer ---
        // Each layer gets its own HandWidget because the multiplexer switches
        // which one is Bind()'d to the UWidgetComponent.
        leftMenuHW_ = std::make_unique<HandWidget>("LeftMenuHW");
        leftNotifHW_ = std::make_unique<HandWidget>("LeftNotifHW");

        leftMenu_ = std::make_unique<HandWidgetMenu>(leftMenuHW_.get(), "LeftMenu");
        leftNotif_ = std::make_unique<HandWidgetNotification>(leftNotifHW_.get(), "LeftNotif");

        leftMux_ = std::make_unique<HandWidgetMultiplexer>("LeftHand");

        // --- Right hand: notifications only (via multiplexer for extensibility) ---
        rightNotifHW_ = std::make_unique<HandWidget>("RightNotifHW");
        rightNotif_ = std::make_unique<HandWidgetNotification>(rightNotifHW_.get(), "RightNotif");

        rightMux_ = std::make_unique<HandWidgetMultiplexer>("RightHand");

        // --- Build menu pages ---
        BuildTestMenuPages();

        initialized_ = true;
        LOG_INFO("[HWTest] Initialization complete");
    }

    // =========================================================================
    // Build test menu pages
    // =========================================================================
    void HandWidgetTestHarness::BuildTestMenuPages()
    {
        if (!leftMenu_)
            return;

        LOG_INFO("[HWTest] Building test menu pages");

        // Page 0: Main test menu
        leftMenu_->RegisterPage(0, "TEST MENU", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"Test Item 1",
                []() -> std::string { return "OK"; },
                []() {
                    LOG_INFO("[HWTest] Test Item 1 selected!");
                    auto* harness = HandWidgetTestHarness::Get();
                    if (harness && harness->rightNotif_)
                    {
                        harness->rightNotif_->Show(L"Test Item 1 was selected!", 3.0f);
                    }
                }
            });

            items.push_back({"Test Item 2 (toggle)",
                []() -> std::string {
                    static bool state = false;
                    return state ? "ON" : "OFF";
                },
                []() {
                    static bool state = false;
                    state = !state;
                    LOG_INFO("[HWTest] Test Item 2 toggled to " << (state ? "ON" : "OFF"));
                }
            });

            items.push_back({"Sub-menu >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(1); },
                true
            });

            items.push_back({"Notif on Left Hand",
                []() -> std::string { return ""; },
                []() {
                    auto* harness = HandWidgetTestHarness::Get();
                    if (harness && harness->leftNotif_)
                    {
                        harness->leftNotif_->Enqueue({L"Notification from menu!", 3.0f, L"MENU NOTIF", 0});
                    }
                }
            });

            items.push_back({"Notif on Right Hand",
                []() -> std::string { return ""; },
                []() {
                    auto* harness = HandWidgetTestHarness::Get();
                    if (harness && harness->rightNotif_)
                    {
                        harness->rightNotif_->Show(L"Hello from the right hand!", 3.0f);
                    }
                }
            });

            items.push_back({"Close Menu",
                []() -> std::string { return ""; },
                [&menu]() { menu.Close(); }
            });
        });

        // Page 1: Sub-menu for testing navigation
        leftMenu_->RegisterPage(1, "SUB-MENU", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Sub Item A",
                []() -> std::string { return "A"; },
                []() { LOG_INFO("[HWTest] Sub Item A selected"); }
            });

            items.push_back({"Sub Item B",
                []() -> std::string { return "B"; },
                []() { LOG_INFO("[HWTest] Sub Item B selected"); }
            });

            items.push_back({"Sub Item C (queues 3 notifs)",
                []() -> std::string { return "C"; },
                []() {
                    auto* harness = HandWidgetTestHarness::Get();
                    if (harness && harness->rightNotif_)
                    {
                        harness->rightNotif_->Enqueue({L"Notification 1 of 3", 2.0f, L"QUEUE", 0});
                        harness->rightNotif_->Enqueue({L"Notification 2 of 3", 2.0f, L"QUEUE", 0});
                        harness->rightNotif_->Enqueue({L"Notification 3 of 3", 2.0f, L"QUEUE", 0});
                    }
                }
            });
        });

        LOG_INFO("[HWTest] Menu pages built");
    }

    // =========================================================================
    // Ensure bindings (handles level transitions, player respawns)
    // =========================================================================
    void HandWidgetTestHarness::EnsureBindings(SDK::UWorld* world)
    {
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
        if (!player || player->IsDefaultObject())
        {
            // Player not available yet
            if (leftBound_ || rightBound_)
            {
                LOG_INFO("[HWTest] Player lost, unbinding all");
                leftBound_ = false;
                rightBound_ = false;
                // Don't destroy widgets here — they'll be re-validated on next bind
            }
            return;
        }

        // --- Left hand ---
        SDK::UWidgetComponent* leftComp = player->W_GripDebug_L;
        if (leftComp && !leftBound_)
        {
            LOG_INFO("[HWTest] Binding left hand to W_GripDebug_L");

            // The multiplexer manages binding layers to the component
            leftMux_->Bind(leftComp);

            // Set up menu layer
            auto& menuLayer = leftMux_->AddLayer("Menu", LayerPriority::Menu);
            menuLayer.handWidget = leftMenuHW_.get();
            menuLayer.isActiveQuery = [this]() { return leftMenu_ && leftMenu_->IsOpen(); };

            // Set up notification layer on left hand
            auto& leftNotifLayer = leftMux_->AddLayer("Notification", LayerPriority::Notification);
            leftNotifLayer.handWidget = leftNotifHW_.get();
            leftNotifLayer.isActiveQuery = [this]() {
                return leftNotif_ && (leftNotif_->IsShowing() || leftNotif_->HasQueued());
            };
            leftNotifLayer.onPauseChanged = [this](bool paused) {
                if (leftNotif_) leftNotif_->SetPaused(paused);
            };

            leftBound_ = true;
        }

        // --- Right hand ---
        SDK::UWidgetComponent* rightComp = player->W_GripDebug_R;
        if (rightComp && !rightBound_)
        {
            LOG_INFO("[HWTest] Binding right hand to W_GripDebug_R");

            rightMux_->Bind(rightComp);

            // Set up notification layer
            auto& notifLayer = rightMux_->AddLayer("Notification", LayerPriority::Notification);
            notifLayer.handWidget = rightNotifHW_.get();
            notifLayer.isActiveQuery = [this]() {
                return rightNotif_ && (rightNotif_->IsShowing() || rightNotif_->HasQueued());
            };

            // Bind the HandWidget to the component
            rightNotifHW_->Bind(rightComp);

            rightBound_ = true;
        }
    }

    // =========================================================================
    // Tick
    // =========================================================================
    void HandWidgetTestHarness::Tick(SDK::UWorld* world)
    {
        if (!initialized_)
            return;

        if (!world)
            return;

        // Ensure bindings are valid
        EnsureBindings(world);

        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);

        // Tick multiplexers
        if (leftMux_ && leftBound_)
            leftMux_->Tick(world, pc);
        if (rightMux_ && rightBound_)
            rightMux_->Tick(world, pc);

        // Tick notifications
        if (rightNotif_ && rightBound_)
            rightNotif_->Tick(world, pc);
        if (leftNotif_ && leftBound_)
            leftNotif_->Tick(world, pc);

        // Tick menu render (if open)
        if (leftMenu_ && leftMenu_->IsOpen() && leftBound_)
            leftMenu_->Render(world, pc);
    }

    // =========================================================================
    // Input routing
    // =========================================================================
    bool HandWidgetTestHarness::OnToggleMenu(SDK::UWorld* world)
    {
        if (!initialized_ || !leftMenu_ || !leftBound_)
            return false;

        SDK::APlayerController* pc = world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
        bool nowOpen = leftMenu_->Toggle(world, pc);

        LOG_INFO("[HWTest] OnToggleMenu: " << (nowOpen ? "OPENED" : "CLOSED"));

        return true; // always consume when we handle it
    }

    bool HandWidgetTestHarness::OnNavigate(float thumbstickY)
    {
        if (!initialized_ || !leftMenu_)
            return false;

        if (!leftMenu_->IsOpen())
            return false;

        return leftMenu_->OnNavigate(thumbstickY);
    }

    bool HandWidgetTestHarness::OnSelect()
    {
        if (!initialized_ || !leftMenu_)
            return false;

        if (!leftMenu_->IsOpen())
            return false;

        bool consumed = leftMenu_->OnSelect();

        // Re-render after selection
        if (leftMenu_->IsOpen() && leftBound_)
        {
            SDK::UWorld* world = Mod::GameContext::GetWorld();
            SDK::APlayerController* pc = world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
            leftMenu_->Render(world, pc);
        }

        return consumed;
    }

    bool HandWidgetTestHarness::IsMenuOpen() const
    {
        return initialized_ && leftMenu_ && leftMenu_->IsOpen();
    }

    // =========================================================================
    // Register commands
    // =========================================================================
    void HandWidgetTestHarness::RegisterCommands(Mod::CommandHandlerRegistry& registry)
    {
        registry.Register("hwtest", [this](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string {
            return HandleTestCommand(world, args);
        });

        LOG_INFO("[HWTest] TCP commands registered (hwtest)");
    }

    std::string HandWidgetTestHarness::HandleTestCommand(SDK::UWorld* world, const std::vector<std::string>& args)
    {
        if (args.empty())
        {
            return "Usage: hwtest <subcommand> [args...]\n"
                   "Subcommands:\n"
                   "  notif <text>         - Show notification on right hand\n"
                   "  notif_left <text>    - Show notification on left hand\n"
                   "  notif_queue <text>   - Enqueue notification on right hand\n"
                   "  menu                 - Toggle left hand menu\n"
                   "  clear                - Clear all notifications\n"
                   "  status               - Show current state\n"
                   "  bind                 - Force rebind to hand widgets\n";
        }

        const std::string& subcmd = args[0];

        if (subcmd == "notif" || subcmd == "notification")
        {
            if (args.size() < 2)
                return "Usage: hwtest notif <text>";

            std::string text;
            for (size_t i = 1; i < args.size(); ++i)
            {
                if (i > 1) text += " ";
                text += args[i];
            }

            std::wstring wtext(text.begin(), text.end());

            if (rightNotif_)
            {
                rightNotif_->Show(wtext, 5.0f);
                return "Notification shown on right hand: " + text;
            }
            return "ERROR: rightNotif_ is null";
        }

        if (subcmd == "notif_left")
        {
            if (args.size() < 2)
                return "Usage: hwtest notif_left <text>";

            std::string text;
            for (size_t i = 1; i < args.size(); ++i)
            {
                if (i > 1) text += " ";
                text += args[i];
            }

            std::wstring wtext(text.begin(), text.end());

            if (leftNotif_)
            {
                leftNotif_->Show(wtext, 5.0f);
                return "Notification shown on left hand: " + text;
            }
            return "ERROR: leftNotif_ is null";
        }

        if (subcmd == "notif_queue" || subcmd == "nq")
        {
            if (args.size() < 2)
                return "Usage: hwtest notif_queue <text>";

            std::string text;
            for (size_t i = 1; i < args.size(); ++i)
            {
                if (i > 1) text += " ";
                text += args[i];
            }

            std::wstring wtext(text.begin(), text.end());

            NotificationMessage msg;
            msg.text = wtext;
            msg.durationSeconds = 3.0f;
            msg.title = L"QUEUED";

            if (rightNotif_)
            {
                rightNotif_->Enqueue(msg);
                return "Notification enqueued on right hand: " + text;
            }
            return "ERROR: rightNotif_ is null";
        }

        if (subcmd == "menu")
        {
            if (!leftMenu_)
                return "ERROR: leftMenu_ is null";

            SDK::APlayerController* pc = world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
            bool nowOpen = leftMenu_->Toggle(world, pc);
            return std::string("Menu ") + (nowOpen ? "OPENED" : "CLOSED");
        }

        if (subcmd == "clear")
        {
            if (rightNotif_) rightNotif_->ClearAll();
            if (leftNotif_) leftNotif_->ClearAll();
            return "All notifications cleared";
        }

        if (subcmd == "status")
        {
            std::ostringstream oss;
            oss << "HandWidget Test Harness Status:\n";
            oss << "  initialized: " << (initialized_ ? "yes" : "no") << "\n";
            oss << "  leftBound: " << (leftBound_ ? "yes" : "no") << "\n";
            oss << "  rightBound: " << (rightBound_ ? "yes" : "no") << "\n";
            oss << "  leftMenu open: " << (leftMenu_ && leftMenu_->IsOpen() ? "yes" : "no") << "\n";
            oss << "  rightNotif showing: " << (rightNotif_ && rightNotif_->IsShowing() ? "yes" : "no") << "\n";
            oss << "  rightNotif queued: " << (rightNotif_ && rightNotif_->HasQueued() ? "yes" : "no") << "\n";
            oss << "  leftMux active: " << (leftMux_ ? leftMux_->GetActiveLayerName() : "null") << "\n";
            oss << "  rightMux active: " << (rightMux_ ? rightMux_->GetActiveLayerName() : "null") << "\n";
            return oss.str();
        }

        if (subcmd == "bind")
        {
            leftBound_ = false;
            rightBound_ = false;
            EnsureBindings(world);
            return std::string("Rebind attempted. left=") + (leftBound_ ? "ok" : "fail")
                   + " right=" + (rightBound_ ? "ok" : "fail");
        }

        return "Unknown subcommand: " + subcmd + ". Try: hwtest";
    }

    // =========================================================================
    // Shutdown
    // =========================================================================
    void HandWidgetTestHarness::Shutdown()
    {
        LOG_INFO("[HWTest] Shutting down");

        if (leftMenu_) leftMenu_->Close();
        if (leftNotif_) leftNotif_->ClearAll();
        if (rightNotif_) rightNotif_->ClearAll();

        leftMux_.reset();
        rightMux_.reset();
        leftMenu_.reset();
        leftNotif_.reset();
        rightNotif_.reset();
        leftMenuHW_.reset();
        leftNotifHW_.reset();
        rightNotifHW_.reset();

        leftBound_ = false;
        rightBound_ = false;
        initialized_ = false;

        LOG_INFO("[HWTest] Shutdown complete");
    }

} // namespace PortableWidget
