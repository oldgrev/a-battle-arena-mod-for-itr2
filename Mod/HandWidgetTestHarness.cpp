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
#include "NVGSubsystem.hpp"

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
    static constexpr int kPageNVG            = 7;
    static constexpr int kPageNVGTuning      = 8;
    static constexpr int kPageNVGLens        = 9;

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
        // Postpone until the player controller is valid and widgets can be created. Will be called again on level change.
        if (!Mod::GameContext::GetPlayerController())
        {
            LOG_INFO("[HWMenu] PlayerController not ready, postponing initialization");
            return;
        }
        if (!Mod::GameContext::GetWorld())
        {
            LOG_INFO("[HWMenu] World not ready, postponing initialization");
            return;
        }
        if (!Mod::GetCheats())
        {
            LOG_INFO("[HWMenu] Cheats subsystem not ready, postponing initialization");
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
        BuildNVGPage();
        BuildNVGTuningPage();
        BuildNVGLensPage();

        LOG_INFO("[HWMenu] Menu pages built (10 pages)");
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

            items.push_back({"NVG >",
                []() -> std::string {
                    return Mod::NVGSubsystem::Get().IsEnabled() ? "ON" : "OFF";
                },
                [&menu]() { menu.NavigateToPage(kPageNVG); },
                true
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
    // NVG PAGE (hub — links to Tuning and Lens sub-pages)
    // =========================================================================
    void HandWidgetTestHarness::BuildNVGPage()
    {
        leftMenu_->RegisterPage(kPageNVG, "NIGHT VISION", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            // Toggle NVG on/off
            items.push_back({"NVG Toggle",
                []() -> std::string {
                    return Mod::NVGSubsystem::Get().IsEnabled() ? "ON" : "OFF";
                },
                []() {
                    Mod::NVGSubsystem::Get().Toggle();
                }
            });

            // Cycle mode: Fullscreen -> LensBlackout -> LensOverlay -> Fullscreen
            items.push_back({"Mode",
                []() -> std::string {
                    return Mod::NVGSubsystem::ModeToString(Mod::NVGSubsystem::Get().GetMode());
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    int next = (static_cast<int>(nvg.GetMode()) + 1) % static_cast<int>(Mod::NVGMode::COUNT);
                    nvg.SetMode(static_cast<Mod::NVGMode>(next));
                }
            });

            // Sub-page: tuning (intensity, grain, bloom, aberration, FOV)
            items.push_back({"Tuning >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(kPageNVGTuning); }
            });

            // Sub-page: lens (material, mesh, scale, distance, offsets, adjust)
            items.push_back({"Lens >",
                []() -> std::string { return ""; },
                [&menu]() { menu.NavigateToPage(kPageNVGLens); }
            });

            // Probe game NVG
            items.push_back({"Probe NVG",
                []() -> std::string {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    return nvg.IsProbeComplete() ? (nvg.IsProbeFound() ? "FOUND" : "NOT_FOUND") : "PENDING";
                },
                [&menu]() {
                    auto* world = Mod::GameContext::GetWorld();
                    if (world) {
                        auto result = Mod::NVGSubsystem::Get().ProbeGameNVG(world);
                        LOG_INFO("[NVG] HandWidget probe result:\n" << result);
                    }
                }
            });

            // Dump blendables
            items.push_back({"Blendables",
                []() -> std::string { return "Dump"; },
                [&menu]() {
                    auto* world = Mod::GameContext::GetWorld();
                    if (world) {
                        auto result = Mod::NVGSubsystem::Get().DumpBlendables(world);
                        LOG_INFO("[NVG] HandWidget blendables:\n" << result);
                    }
                }
            });
        });
    }

    // =========================================================================
    // NVG TUNING PAGE (intensity, grain, bloom, aberration, FOV)
    // =========================================================================
    void HandWidgetTestHarness::BuildNVGTuningPage()
    {
        leftMenu_->RegisterPage(kPageNVGTuning, "NVG TUNING", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            items.push_back({"Intensity +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetIntensity());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetIntensity(nvg.GetIntensity() + 0.01f);
                }
            });
            items.push_back({"Intensity -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetIntensity());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetIntensity(nvg.GetIntensity() - 0.01f);
                }
            });
            items.push_back({"Intensity ++",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetIntensity());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetIntensity(nvg.GetIntensity() + 0.1f);
                }
            });
            items.push_back({"Intensity --",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetIntensity());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetIntensity(nvg.GetIntensity() - 0.1f);
                }
            });

            // --- Grain (+/- 0.2, range 0-5) ---
            items.push_back({"Grain +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetGrain());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetGrain(nvg.GetGrain() + 0.2f);
                }
            });
            items.push_back({"Grain -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetGrain());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetGrain(nvg.GetGrain() - 0.2f);
                }
            });

            // --- Bloom (+/- 2.0, range 0-50) ---
            items.push_back({"Bloom +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetBloom());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetBloom(nvg.GetBloom() + 2.0f);
                }
            });
            items.push_back({"Bloom -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetBloom());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetBloom(nvg.GetBloom() - 2.0f);
                }
            });

            // --- Aberration (+/- 0.5, range 0-10) ---
            items.push_back({"Aberration +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetAberration());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetAberration(nvg.GetAberration() + 0.5f);
                }
            });
            items.push_back({"Aberration -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetAberration());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetAberration(nvg.GetAberration() - 0.5f);
                }
            });

            // --- FOV (+/- 5.0) ---
            items.push_back({"FOV +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.0f", Mod::NVGSubsystem::Get().GetLensFOV());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensFOV(nvg.GetLensFOV() + 5.0f);
                }
            });
            items.push_back({"FOV -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.0f", Mod::NVGSubsystem::Get().GetLensFOV());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensFOV(nvg.GetLensFOV() - 5.0f);
                }
            });
        });
    }

    // =========================================================================
    // NVG LENS PAGE (material, mesh, scale, distance, offsets, adjust mode)
    // =========================================================================
    void HandWidgetTestHarness::BuildNVGLensPage()
    {
        leftMenu_->RegisterPage(kPageNVGLens, "NVG LENS", [](std::vector<MenuItem>& items, HandWidgetMenu& menu) {
            items.push_back({"< Back",
                []() -> std::string { return ""; },
                [&menu]() { menu.GoBack(); }
            });

            // Cycle material type: 0=M_Lens, 1=M_Particle, 2=MI_Lens_Magnifer
            items.push_back({"Material",
                []() -> std::string {
                    static const char* names[] = {"M_Lens", "M_Particle", "MI_Magnif"};
                    int t = Mod::NVGSubsystem::Get().GetLensMatType();
                    if (t >= 0 && t <= 2) return names[t];
                    return "?";
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    int next = (nvg.GetLensMatType() + 1) % 3;
                    nvg.SetLensMatType(next);
                }
            });

            // Toggle mesh type: 0=Plane, 1=Cylinder disc
            items.push_back({"Mesh",
                []() -> std::string {
                    return Mod::NVGSubsystem::Get().GetLensMeshType() == 0 ? "Plane" : "Cylinder";
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensMeshType(nvg.GetLensMeshType() == 0 ? 1 : 0);
                }
            });

            // --- Scale (+/- 0.02) ---
            items.push_back({"Scale +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.2f", Mod::NVGSubsystem::Get().GetLensScale());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensScale(nvg.GetLensScale() + 0.02f);
                }
            });
            items.push_back({"Scale -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.2f", Mod::NVGSubsystem::Get().GetLensScale());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensScale(nvg.GetLensScale() - 0.02f);
                }
            });

            // --- Distance (+/- 1.0) ---
            items.push_back({"Dist +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensDistance());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensDistance(nvg.GetLensDistance() + 1.0f);
                }
            });
            items.push_back({"Dist -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensDistance());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensDistance(nvg.GetLensDistance() - 1.0f);
                }
            });

            // --- Offset Y (+/- 0.5) ---
            items.push_back({"Offset Y +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensOffsetY());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensOffset(nvg.GetLensOffsetY() + 0.5f, nvg.GetLensOffsetZ());
                }
            });
            items.push_back({"Offset Y -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensOffsetY());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensOffset(nvg.GetLensOffsetY() - 0.5f, nvg.GetLensOffsetZ());
                }
            });

            // --- Offset Z (+/- 0.5) ---
            items.push_back({"Offset Z +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensOffsetZ());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensOffset(nvg.GetLensOffsetY(), nvg.GetLensOffsetZ() + 0.5f);
                }
            });
            items.push_back({"Offset Z -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetLensOffsetZ());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensOffset(nvg.GetLensOffsetY(), nvg.GetLensOffsetZ() - 0.5f);
                }
            });

            // ----------------------------------------------------------------
            // Cylinder rotation controls — 90 degree increments per axis.
            //
            // UE axis convention (for reference while testing):
            //   Pitch = around Y (tilts forward/back)
            //   Yaw   = around Z (rotates left/right)
            //   Roll  = around X (tilts sideways)
            //
            // Current defaults that made the lens visible: P180 / Y270 / R270.
            // Use these buttons to find the orientation where the circular face
            // of the cylinder is pointing toward the camera (viewer-facing disc).
            // Each button press is logged via [NVG] StepLensRot* so you can
            // correlate button presses with log output if the visual result is
            // not obvious.
            // ----------------------------------------------------------------

            // --- Pitch (around Y) ---
            items.push_back({"Pitch+90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "P=%.0f", Mod::NVGSubsystem::Get().GetLensRotPitch());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotPitch(+90.0f);
                }
            });
            items.push_back({"Pitch-90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "P=%.0f", Mod::NVGSubsystem::Get().GetLensRotPitch());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotPitch(-90.0f);
                }
            });

            // --- Yaw (around Z) ---
            items.push_back({"Yaw+90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "Y=%.0f", Mod::NVGSubsystem::Get().GetLensRotYaw());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotYaw(+90.0f);
                }
            });
            items.push_back({"Yaw-90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "Y=%.0f", Mod::NVGSubsystem::Get().GetLensRotYaw());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotYaw(-90.0f);
                }
            });

            // --- Roll (around X) ---
            items.push_back({"Roll+90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "R=%.0f", Mod::NVGSubsystem::Get().GetLensRotRoll());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotRoll(+90.0f);
                }
            });
            items.push_back({"Roll-90",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "R=%.0f", Mod::NVGSubsystem::Get().GetLensRotRoll());
                    return buf;
                },
                []() {
                    Mod::NVGSubsystem::Get().StepLensRotRoll(-90.0f);
                }
            });

            // Reset all rotation to the last known working values (P0 Y0 R0 = identity,
            // P180/Y270/R270 = previous hand-tuned default).  Two quick-reset buttons
            // give fast recovery to a known state without typing commands.
            items.push_back({"Rot Reset0",
                []() -> std::string { return "P0 Y0 R0"; },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    LOG_INFO("[NVG] RotReset0: resetting PYR to 0/0/0");
                    nvg.SetLensRotation(0.0f, 0.0f, 0.0f);
                    LOG_INFO("[NVG] RotReset0 done: PYR="
                        << nvg.GetLensRotPitch() << "/" << nvg.GetLensRotYaw() << "/" << nvg.GetLensRotRoll());
                }
            });
            items.push_back({"Rot Reset90",
                []() -> std::string { return "P90 Y0 R0"; },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    LOG_INFO("[NVG] RotReset90: resetting PYR to 90/0/0");
                    nvg.SetLensRotation(90.0f, 0.0f, 0.0f);
                    LOG_INFO("[NVG] RotReset90 done: PYR="
                        << nvg.GetLensRotPitch() << "/" << nvg.GetLensRotYaw() << "/" << nvg.GetLensRotRoll());
                }
            });
            // Cylinder working rotation confirmed 2026-03-11: P270/Y180/R270
            items.push_back({"Rot Dflt",
                []() -> std::string { return "P270 Y180 R270"; },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    LOG_INFO("[NVG] RotDefault: resetting PYR to 270/180/270 (cylinder confirmed)");
                    nvg.SetLensRotation(270.0f, 180.0f, 270.0f);
                    LOG_INFO("[NVG] RotDefault done: PYR="
                        << nvg.GetLensRotPitch() << "/" << nvg.GetLensRotYaw() << "/" << nvg.GetLensRotRoll());
                }
            });
            // Alternative: P90/Y180/R270 (also works but slightly offset)
            items.push_back({"Rot Alt",
                []() -> std::string { return "P90 Y180 R270"; },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    LOG_INFO("[NVG] RotAlt: resetting PYR to 90/180/270 (alt cylinder)");
                    nvg.SetLensRotation(90.0f, 180.0f, 270.0f);
                    LOG_INFO("[NVG] RotAlt done: PYR="
                        << nvg.GetLensRotPitch() << "/" << nvg.GetLensRotYaw() << "/" << nvg.GetLensRotRoll());
                }
            });

            // Toggle lens adjust mode (thumbstick moves lens)
            items.push_back({"Stick Adjust",
                []() -> std::string {
                    return Mod::NVGSubsystem::Get().IsLensAdjustMode() ? "ON" : "OFF";
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetLensAdjustMode(!nvg.IsLensAdjustMode());
                }
            });

            // --- Camera Offset X (+/- 1.0) — shift NVG capture forward/back ---
            items.push_back({"CamOff X +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetX());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX() + 1.0f, nvg.GetCaptureOffsetY(), nvg.GetCaptureOffsetZ());
                }
            });
            items.push_back({"CamOff X -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetX());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX() - 1.0f, nvg.GetCaptureOffsetY(), nvg.GetCaptureOffsetZ());
                }
            });

            // --- Camera Offset Y (+/- 1.0) — shift NVG capture left/right ---
            items.push_back({"CamOff Y +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetY());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX(), nvg.GetCaptureOffsetY() + 1.0f, nvg.GetCaptureOffsetZ());
                }
            });
            items.push_back({"CamOff Y -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetY());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX(), nvg.GetCaptureOffsetY() - 1.0f, nvg.GetCaptureOffsetZ());
                }
            });

            // --- Camera Offset Z (+/- 1.0) — shift NVG capture up/down ---
            items.push_back({"CamOff Z +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetZ());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX(), nvg.GetCaptureOffsetY(), nvg.GetCaptureOffsetZ() + 1.0f);
                }
            });
            items.push_back({"CamOff Z -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetCaptureOffsetZ());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetCaptureOffset(nvg.GetCaptureOffsetX(), nvg.GetCaptureOffsetY(), nvg.GetCaptureOffsetZ() - 1.0f);
                }
            });

            // --- Render target / material projection controls ---
            // These control how the NVG "painting" maps onto the lens mesh.
            // Fixing dark edge encroachment by eliminating rim mask and auto-scaling image.

            items.push_back({"ImgScale +",
                []() -> std::string {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    float val = nvg.GetRTImageScale();
                    char buf[24];
                    if (val <= 0.01f)
                        snprintf(buf, sizeof(buf), "auto(%.1f)", nvg.ComputeAutoImageScale());
                    else
                        snprintf(buf, sizeof(buf), "%.1f", val);
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    float cur = nvg.GetRTImageScale();
                    if (cur <= 0.01f) cur = nvg.ComputeAutoImageScale();
                    nvg.SetRTImageScale(cur + 0.5f);
                }
            });
            items.push_back({"ImgScale -",
                []() -> std::string {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    float val = nvg.GetRTImageScale();
                    char buf[24];
                    if (val <= 0.01f)
                        snprintf(buf, sizeof(buf), "auto(%.1f)", nvg.ComputeAutoImageScale());
                    else
                        snprintf(buf, sizeof(buf), "%.1f", val);
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    float cur = nvg.GetRTImageScale();
                    if (cur <= 0.01f) cur = nvg.ComputeAutoImageScale();
                    float next = cur - 0.5f;
                    if (next < 0.5f) next = 0.0f;  // 0 = auto mode
                    nvg.SetRTImageScale(next);
                }
            });
            items.push_back({"ImgScale 0",
                []() -> std::string {
                    return "auto";
                },
                []() {
                    Mod::NVGSubsystem::Get().SetRTImageScale(0.0f);
                }
            });

            items.push_back({"RimScale +",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetRTRimScale());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetRTRimScale(nvg.GetRTRimScale() + 5.0f);
                }
            });
            items.push_back({"RimScale -",
                []() -> std::string {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.1f", Mod::NVGSubsystem::Get().GetRTRimScale());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetRTRimScale(nvg.GetRTRimScale() - 5.0f);
                }
            });

            items.push_back({"Rim On/Off",
                []() -> std::string {
                    float depth = Mod::NVGSubsystem::Get().GetRTRimDepth();
                    return depth < -0.1f ? "ON" : "OFF";
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    if (nvg.GetRTRimDepth() < -0.1f)
                        nvg.SetRTRimDepth(0.0f);  // Turn rim off
                    else
                        nvg.SetRTRimDepth(-100.0f);  // Turn rim on (sharp edge)
                }
            });

            items.push_back({"AutoFOV",
                []() -> std::string {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    char buf[24];
                    snprintf(buf, sizeof(buf), "%s(%.0f)", nvg.GetAutoFOV() ? "ON" : "OFF", nvg.ComputeAutoFOV());
                    return buf;
                },
                []() {
                    auto& nvg = Mod::NVGSubsystem::Get();
                    nvg.SetAutoFOV(!nvg.GetAutoFOV());
                }
            });
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
        {
            // Retry initialization — may have failed earlier due to null PC/World/Cheats
            // which can happen after level transitions or during loading screens.
            Initialize();
            if (!initialized_)
                return;
            LOG_INFO("[HandWidget] Late-initialized successfully on retry");
        }

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
        LOG_INFO("[HWMenu] Left menu closed");
        if (leftNotif_) leftNotif_->ClearAll();
        LOG_INFO("[HWMenu] Left notifications cleared");
        if (rightNotif_) rightNotif_->ClearAll();
        LOG_INFO("[HWMenu] Right notifications cleared");

        // leftMux_.reset();
        // LOG_INFO("[HWMenu] Left multiplexer reset");
        // rightMux_.reset();
        // LOG_INFO("[HWMenu] Right multiplexer reset");
        // leftMenu_.reset();
        // LOG_INFO("[HWMenu] Left menu reset");
        // leftNotif_.reset();
        // LOG_INFO("[HWMenu] Left notifications reset");
        // rightNotif_.reset();
        // LOG_INFO("[HWMenu] Right notifications reset");
        // leftMenuHW_.reset();
        // LOG_INFO("[HWMenu] Left menu HW reset");
        // leftNotifHW_.reset();
        // LOG_INFO("[HWMenu] Left notifications HW reset");
        // rightNotifHW_.reset();
        // LOG_INFO("[HWMenu] Right notifications HW reset");

        leftBound_ = false;
        LOG_INFO("[HWMenu] Left bound reset");
        rightBound_ = false;
        LOG_INFO("[HWMenu] Right bound reset");
        initialized_ = false;

        LOG_INFO("[HWMenu] Shutdown complete");
    }

} // namespace PortableWidget
