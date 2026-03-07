/*
AILEARNINGS:
- HandWidgetTestHarness is the mod's main integration of the portable HandWidget system.
- All 7 menu pages from the old VRMenuSubsystem are ported here as RegisterPage() calls.
- ShowNotification() is the public API that replaces ModFeedback::ShowOnRightWidget.
- Grip state tracking lives here (moved from VRMenuSubsystem) for combo input detection.
- Arena config state (enemy/wave counts) and friend class selection persist across menu close/reopen.
- On level change, Shutdown() + Initialize() is called from ModMain to clear stale UObject pointers.
  The persistent menu state (arenaEnemyCount_, etc.) is preserved because they're on the singleton.
*/

#include "HandWidgetTestHarness.hpp"
#include "HandWidget.hpp"
#include "HandWidgetNotification.hpp"
#include "HandWidgetMenu.hpp"
#include "HandWidgetMultiplexer.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "CommandHandler.hpp"
#include "Cheats.hpp"
#include "ArenaSubsystem.hpp"
#include "FriendSubsystem.hpp"
#include "LoadoutSubsystem.hpp"

#include "..\CppSDK\SDK.hpp"

#include <sstream>
#include <algorithm>

namespace Mod { class Cheats; Cheats* GetCheats(); }

namespace PortableWidget
{
    namespace Arena = Mod::Arena;  // Alias so Arena::ArenaSubsystem resolves

    // =========================================================================
    // Menu page IDs — match the old VRMenuSubsystem page structure
    // =========================================================================
    static constexpr int kPageMain          = 0;
    static constexpr int kPageCheats        = 1;
    static constexpr int kPageArena         = 2;
    static constexpr int kPageLoadouts      = 3;
    static constexpr int kPageLoadoutSelect = 4;
    static constexpr int kPageSpawnFriend   = 5;
    static constexpr int kPageFriendClass   = 6;

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
            LOG_INFO("[HWMenu] Already initialized");
            return;
        }

        LOG_INFO("[HWMenu] === Initializing HandWidget Menu System ===");

        // --- Left hand: menu + notifications via multiplexer ---
        leftMenuHW_ = std::make_unique<HandWidget>("LeftMenuHW");
        leftNotifHW_ = std::make_unique<HandWidget>("LeftNotifHW");

        leftMenu_ = std::make_unique<HandWidgetMenu>(leftMenuHW_.get(), "LeftMenu");
        leftNotif_ = std::make_unique<HandWidgetNotification>(leftNotifHW_.get(), "LeftNotif");

        leftMux_ = std::make_unique<HandWidgetMultiplexer>("LeftHand");

        // --- Right hand: notifications only (via multiplexer for extensibility) ---
        rightNotifHW_ = std::make_unique<HandWidget>("RightNotifHW");
        rightNotif_ = std::make_unique<HandWidgetNotification>(rightNotifHW_.get(), "RightNotif");

        rightMux_ = std::make_unique<HandWidgetMultiplexer>("RightHand");

        // --- Build all menu pages ---
        BuildMenuPages();

        initialized_ = true;
        LOG_INFO("[HWMenu] Initialization complete");
    }

    // =========================================================================
    // Build all menu pages
    // =========================================================================
    void HandWidgetTestHarness::BuildMenuPages()
    {
        if (!leftMenu_)
            return;

        LOG_INFO("[HWMenu] Building menu pages");

        BuildMainPage();
        BuildCheatsPage();
        BuildArenaPage();
        BuildLoadoutsPage();
        BuildLoadoutSelectPage();
        BuildSpawnFriendPage();
        BuildFriendClassPage();

        LOG_INFO("[HWMenu] Menu pages built (7 pages)");
    }

    // =========================================================================
    // MAIN PAGE
    // =========================================================================
    void HandWidgetTestHarness::BuildMainPage()
    {
        leftMenu_->RegisterPage(kPageMain, "MOD MENU", [this](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"Cheats >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(kPageCheats); },
                true
            });

            items.push_back({"Arena Quick Start",
                []() -> std::string {
                    auto* a = Arena::ArenaSubsystem::Get();
                    return (a && a->IsActive()) ? "ACTIVE" : "";
                },
                [this]() {
                    auto* a = Arena::ArenaSubsystem::Get();
                    if (!a) return;
                    if (a->IsActive())
                        a->Stop();
                    else
                        a->Start(arenaEnemyCount_, 15.0f);
                }
            });

            items.push_back({"Quick Spawn Friend",
                []() -> std::string {
                    auto* f = Mod::Friend::FriendSubsystem::Get();
                    return f ? std::to_string(f->ActiveFriendCount()) : "";
                },
                []() {
                    auto* f = Mod::Friend::FriendSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (f && w) f->SpawnFriend(w);
                }
            });

            items.push_back({"Loadouts >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(kPageLoadouts); },
                true
            });

            items.push_back({"Arena Config >",
                [this]() -> std::string {
                    return std::to_string(arenaEnemyCount_) + " enemies";
                },
                [&menu]() { menu.NavigateToPage(kPageArena); },
                true
            });

            items.push_back({"Spawn Friend >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(kPageSpawnFriend); },
                true
            });

            items.push_back({"Spawn Heal Item",
                []() -> std::string { return ""; },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (c && w) c->SpawnHealItem(w);
                }
            });
        });
    }

    // =========================================================================
    // CHEATS PAGE
    // =========================================================================
    void HandWidgetTestHarness::BuildCheatsPage()
    {
        leftMenu_->RegisterPage(kPageCheats, "CHEATS", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Combo Bypass",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    bool comboActive = c && c->IsAutoMagActive() && c->IsDurabilityBypassActive() && c->IsFatigueDisabledActive();
                    return comboActive ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) {
                        c->SetAutoMag(true);
                        c->ToggleDurabilityBypass();
                        c->ToggleFatigueDisabled();
                    }
                }
            });

            items.push_back({"No Anomalies",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsAnomaliesDisabledActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleAnomaliesDisabled();
                }
            });

            items.push_back({"Durability Bypass",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsDurabilityBypassActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleDurabilityBypass();
                }
            });

            items.push_back({"God Mode",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsGodModeActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleGodMode();
                }
            });

            items.push_back({"Unlimited Ammo",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsUnlimitedAmmoActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleUnlimitedAmmo();
                }
            });

            items.push_back({"No Hunger",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsHungerDisabledActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleHungerDisabled();
                }
            });

            items.push_back({"No Fatigue",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsFatigueDisabledActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleFatigueDisabled();
                }
            });

            items.push_back({"Bullet Time",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsBulletTimeActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleBulletTime();
                }
            });

            items.push_back({"No Clip",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsNoClipActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleNoClip();
                }
            });

            items.push_back({"Auto Mag",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c && c->IsAutoMagActive() ? "ON" : "OFF";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->ToggleAutoMag();
                }
            });

            items.push_back({"Light Intensity x2.0",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c ? std::to_string(c->GetPortableLightIntensityScale()) : "1.0";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->SetPortableLightIntensityScale(2.0f);
                }
            });

            items.push_back({"Light Intensity x0.5",
                []() -> std::string {
                    Mod::Cheats* c = Mod::GetCheats();
                    return c ? std::to_string(c->GetPortableLightIntensityScale()) : "1.0";
                },
                []() {
                    Mod::Cheats* c = Mod::GetCheats();
                    if (c) c->SetPortableLightIntensityScale(0.5f);
                }
            });
        });
    }

    // =========================================================================
    // ARENA PAGE
    // =========================================================================
    void HandWidgetTestHarness::BuildArenaPage()
    {
        leftMenu_->RegisterPage(kPageArena, "ARENA CONFIG", [this](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Toggle Arena",
                []() -> std::string {
                    auto* a = Arena::ArenaSubsystem::Get();
                    return (a && a->IsActive()) ? "ACTIVE" : "STOPPED";
                },
                [this]() {
                    auto* a = Arena::ArenaSubsystem::Get();
                    if (!a) return;
                    if (a->IsActive())
                        a->Stop();
                    else
                        a->Start(arenaEnemyCount_, 15.0f);
                }
            });

            items.push_back({"Enemies: +",
                [this]() -> std::string { return std::to_string(arenaEnemyCount_); },
                [this]() { arenaEnemyCount_ = (std::min)(200, arenaEnemyCount_ + 5); }
            });

            items.push_back({"Enemies: -",
                [this]() -> std::string { return std::to_string(arenaEnemyCount_); },
                [this]() { arenaEnemyCount_ = (std::max)(1, arenaEnemyCount_ - 5); }
            });

            items.push_back({"Waves: +",
                [this]() -> std::string { return std::to_string(arenaWaveCount_); },
                [this]() { arenaWaveCount_ = (std::min)(100, arenaWaveCount_ + 1); }
            });

            items.push_back({"Waves: -",
                [this]() -> std::string { return std::to_string(arenaWaveCount_); },
                [this]() { arenaWaveCount_ = (std::max)(1, arenaWaveCount_ - 1); }
            });
        });
    }

    // =========================================================================
    // LOADOUTS PAGE
    // =========================================================================
    void HandWidgetTestHarness::BuildLoadoutsPage()
    {
        leftMenu_->RegisterPage(kPageLoadouts, "LOADOUTS", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Capture Loadout",
                []() -> std::string { return ""; },
                []() {
                    auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (ls && w) ls->CaptureLoadout(w, "quick_capture");
                }
            });

            items.push_back({"Select Loadout >",
                []() -> std::string {
                    auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
                    return ls ? ls->GetSelectedLoadout() : "";
                },
                [&menu]() { menu.NavigateToPage(kPageLoadoutSelect); },
                true
            });

            items.push_back({"Apply Selected",
                []() -> std::string {
                    auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
                    return ls ? ls->GetSelectedLoadout() : "";
                },
                []() {
                    auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (ls && w) {
                        std::string selected = ls->GetSelectedLoadout();
                        if (!selected.empty())
                            ls->ApplyLoadout(w, selected);
                    }
                }
            });

            items.push_back({"Clear Equipment",
                []() -> std::string { return ""; },
                []() {
                    auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (ls && w) ls->ClearPlayerLoadout(w);
                }
            });
        });
    }

    // =========================================================================
    // LOADOUT SELECT PAGE (dynamic list of .loadout files)
    // =========================================================================
    void HandWidgetTestHarness::BuildLoadoutSelectPage()
    {
        leftMenu_->RegisterPage(kPageLoadoutSelect, "SELECT LOADOUT", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            auto* ls = Mod::Loadout::LoadoutSubsystem::Get();
            if (!ls) return;

            std::string loadoutList = ls->ListLoadouts();
            std::istringstream iss(loadoutList);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.size() > 2 && line[0] == '-' && line[1] == ' ')
                    line = line.substr(2);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                while (!line.empty() && line.front() == ' ')
                    line = line.substr(1);

                if (line.empty()) continue;

                std::string loadoutName = line;
                items.push_back({loadoutName,
                    [loadoutName]() -> std::string {
                        auto* ls2 = Mod::Loadout::LoadoutSubsystem::Get();
                        if (ls2 && ls2->GetSelectedLoadout() == loadoutName)
                            return "SELECTED";
                        return "";
                    },
                    [loadoutName]() {
                        auto* ls2 = Mod::Loadout::LoadoutSubsystem::Get();
                        if (ls2) ls2->SetSelectedLoadout(loadoutName);
                    }
                });
            }
        });
    }

    // =========================================================================
    // SPAWN FRIEND PAGE
    // =========================================================================
    void HandWidgetTestHarness::BuildSpawnFriendPage()
    {
        leftMenu_->RegisterPage(kPageSpawnFriend, "SPAWN FRIEND", [this](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Spawn Friend",
                []() -> std::string {
                    auto* f = Mod::Friend::FriendSubsystem::Get();
                    return f ? std::to_string(f->ActiveFriendCount()) + " active" : "";
                },
                []() {
                    auto* f = Mod::Friend::FriendSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (f && w) f->SpawnFriend(w);
                }
            });

            items.push_back({"Select Class >",
                [this]() -> std::string {
                    return selectedFriendClass_.empty() ? "Random" : selectedFriendClass_;
                },
                [&menu]() { menu.NavigateToPage(kPageFriendClass); },
                true
            });

            items.push_back({"Clear All Friends",
                []() -> std::string { return ""; },
                []() {
                    auto* f = Mod::Friend::FriendSubsystem::Get();
                    SDK::UWorld* w = Mod::GameContext::GetWorld();
                    if (f && w) f->ClearAll(w);
                }
            });
        });
    }

    // =========================================================================
    // FRIEND CLASS PAGE (dynamic list of discovered NPC classes)
    // =========================================================================
    void HandWidgetTestHarness::BuildFriendClassPage()
    {
        leftMenu_->RegisterPage(kPageFriendClass, "SELECT CLASS", [this](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Random",
                [this]() -> std::string {
                    return selectedFriendClass_.empty() ? "SELECTED" : "";
                },
                [this]() { selectedFriendClass_ = ""; }
            });

            auto* arena = Arena::ArenaSubsystem::Get();
            if (!arena) return;

            const auto& npcs = arena->GetDiscoveredNPCs();
            for (const auto& npcClass : npcs) {
                std::string shortName = npcClass;
                size_t lastSlash = shortName.rfind('/');
                if (lastSlash != std::string::npos)
                    shortName = shortName.substr(lastSlash + 1);
                if (shortName.size() > 3 && shortName.substr(0, 3) == "BP_")
                    shortName = shortName.substr(3);
                if (shortName.size() > 2 && shortName.substr(shortName.size() - 2) == "_C")
                    shortName = shortName.substr(0, shortName.size() - 2);

                std::string fullClass = npcClass;
                items.push_back({shortName,
                    [this, fullClass]() -> std::string {
                        return selectedFriendClass_ == fullClass ? "SELECTED" : "";
                    },
                    [this, fullClass]() { selectedFriendClass_ = fullClass; }
                });
            }
        });
    }

    // =========================================================================
    // Ensure bindings (handles level transitions, player respawns)
    // =========================================================================
    void HandWidgetTestHarness::EnsureBindings(SDK::UWorld* world)
    {
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
        if (!player || player->IsDefaultObject())
        {
            if (leftBound_ || rightBound_)
            {
                LOG_INFO("[HWMenu] Player lost, unbinding all");
                leftBound_ = false;
                rightBound_ = false;
            }
            return;
        }

        // --- Left hand ---
        SDK::UWidgetComponent* leftComp = player->W_GripDebug_L;
        if (leftComp && !leftBound_)
        {
            LOG_INFO("[HWMenu] Binding left hand to W_GripDebug_L");

            leftMux_->Bind(leftComp);

            auto& menuLayer = leftMux_->AddLayer("Menu", LayerPriority::Menu);
            menuLayer.handWidget = leftMenuHW_.get();
            menuLayer.isActiveQuery = [this]() { return leftMenu_ && leftMenu_->IsOpen(); };

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
            LOG_INFO("[HWMenu] Binding right hand to W_GripDebug_R");

            rightMux_->Bind(rightComp);

            auto& notifLayer = rightMux_->AddLayer("Notification", LayerPriority::Notification);
            notifLayer.handWidget = rightNotifHW_.get();
            notifLayer.isActiveQuery = [this]() {
                return rightNotif_ && (rightNotif_->IsShowing() || rightNotif_->HasQueued());
            };

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

        EnsureBindings(world);

        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);

        if (leftMux_ && leftBound_)
            leftMux_->Tick(world, pc);
        if (rightMux_ && rightBound_)
            rightMux_->Tick(world, pc);

        if (rightNotif_ && rightBound_)
            rightNotif_->Tick(world, pc);
        if (leftNotif_ && leftBound_)
            leftNotif_->Tick(world, pc);

        if (leftMenu_ && leftMenu_->IsOpen() && leftBound_)
            leftMenu_->Render(world, pc);
    }

    // =========================================================================
    // Public notification API (replaces ModFeedback::ShowOnRightWidget)
    // =========================================================================
    void HandWidgetTestHarness::ShowNotification(const std::wstring& text, float durationSeconds)
    {
        if (rightNotif_)
        {
            rightNotif_->Show(text, durationSeconds);
        }
        else
        {
            LOG_WARN("[HWMenu] ShowNotification called but rightNotif_ is null");
        }
    }

    void HandWidgetTestHarness::ShowNotificationWithTitle(const std::wstring& title, const std::wstring& text, float durationSeconds)
    {
        if (rightNotif_)
        {
            rightNotif_->ShowWithTitle(title, text, durationSeconds);
        }
        else
        {
            LOG_WARN("[HWMenu] ShowNotificationWithTitle called but rightNotif_ is null");
        }
    }

    // =========================================================================
    // Input routing
    // =========================================================================
    void HandWidgetTestHarness::OnGripPressed()
    {
        gripHeld_.store(true, std::memory_order_relaxed);
    }

    void HandWidgetTestHarness::OnGripReleased()
    {
        gripHeld_.store(false, std::memory_order_relaxed);
    }

    bool HandWidgetTestHarness::IsGripHeld() const
    {
        return gripHeld_.load(std::memory_order_relaxed);
    }

    bool HandWidgetTestHarness::OnToggleMenu(SDK::UWorld* world)
    {
        if (!initialized_ || !leftMenu_ || !leftBound_)
            return false;

        SDK::APlayerController* pc = world ? SDK::UGameplayStatics::GetPlayerController(world, 0) : nullptr;
        bool nowOpen = leftMenu_->Toggle(world, pc);

        LOG_INFO("[HWMenu] OnToggleMenu: " << (nowOpen ? "OPENED" : "CLOSED"));

        return true;
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

        LOG_INFO("[HWMenu] TCP commands registered (hwtest)");
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
            ShowNotification(wtext, 5.0f);
            return "Notification shown on right hand: " + text;
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
            oss << "HandWidget Menu System Status:\n";
            oss << "  initialized: " << (initialized_ ? "yes" : "no") << "\n";
            oss << "  leftBound: " << (leftBound_ ? "yes" : "no") << "\n";
            oss << "  rightBound: " << (rightBound_ ? "yes" : "no") << "\n";
            oss << "  gripHeld: " << (gripHeld_.load() ? "yes" : "no") << "\n";
            oss << "  leftMenu open: " << (leftMenu_ && leftMenu_->IsOpen() ? "yes" : "no") << "\n";
            oss << "  rightNotif showing: " << (rightNotif_ && rightNotif_->IsShowing() ? "yes" : "no") << "\n";
            oss << "  rightNotif queued: " << (rightNotif_ && rightNotif_->HasQueued() ? "yes" : "no") << "\n";
            oss << "  leftMux active: " << (leftMux_ ? leftMux_->GetActiveLayerName() : "null") << "\n";
            oss << "  rightMux active: " << (rightMux_ ? rightMux_->GetActiveLayerName() : "null") << "\n";
            oss << "  arenaEnemies: " << arenaEnemyCount_ << "\n";
            oss << "  arenaWaves: " << arenaWaveCount_ << "\n";
            oss << "  friendClass: " << (selectedFriendClass_.empty() ? "Random" : selectedFriendClass_) << "\n";
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
        LOG_INFO("[HWMenu] Shutting down");

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

        LOG_INFO("[HWMenu] Shutdown complete");
    }

} // namespace PortableWidget
