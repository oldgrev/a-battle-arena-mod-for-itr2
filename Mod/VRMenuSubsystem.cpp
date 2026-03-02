/*
AILEARNINGS:
- DrawDebugString is UNVERIFIED in VR. ShowWorldText wraps it but has zero callers — never tested.
  The debug draw pipeline may be stripped in VR/shipping builds. Only subtitles are confirmed visible.
  We keep Approach 1 as a diagnostic experiment with extensive logging.
- DrawDebugString duration of 0.0f means one frame in UE. We use ~0.15s to bridge frame gaps.
- FInputActionValue is 0x20 bytes of opaque data. For a boolean action, the first byte is likely
  the bool (or first 4 bytes are a float 0.0/1.0). For a 2D axis, expect two floats.
  We log the raw bytes on first intercept to verify.
- Menu toggle: Grip+B/Y combo. Track left grip via IA_Grip_Left_26 / IA_UnGrip_Left_4.
  B/Y button (_17 Started): if grip held => toggle menu, if no grip + menu open => select item.
  This prevents accidental activation and uses the proven-reliable B/Y event for selection.
- Navigation uses InpActEvt_IA_Movement (left thumbstick). We only consume this when menu is open.
  When closed, we return false so normal movement works.
- Stick rebound fix: thumbstick flick caused opposite-direction bounce. Fixed with direction latch:
  track lastNavDirection_ (CENTER/UP/DOWN), require return to deadzone before accepting new direction.
- Thumbstick dominant-axis gate: reading only axisValues[0] caused left/right stick movement to
  spuriously trigger vertical navigation. Fix: read both axisValues[0] and axisValues[1], use the
  one with 1.4x larger magnitude. If neither dominates (diagonal), suppress navigation entirely.
  This is applied in Hook_VRMenu_Movement (HookManager.cpp), not in OnNavigate itself.
- IA_Run_Toggle (_31) and IA_Trigger_Left (_24) were unreliable for selection — events may not fire or
  value format is wrong. Replaced with B/Y dual-purpose approach.
- Menu header duplication: title AND description both showed "=== MOD MENU ===". Fixed: only title has it.
- For widget hijack: we scan GObjects for UTextBlock instances and try to repurpose one on the
  player's PlayerDebugWidget. This is highly experimental.
- VR pointer: scan GObjects for URadiusWidgetInteractionComponent at runtime. If found on gameplay
  character, reuse it for pointer interaction with our menu widget. Game may not spawn one in gameplay.
- Menu height clipping: W_GripDebug_L widget anchors at the hand and (by default, pivot 0.5,0.5)
  extends equally up and down. As menu grows taller, bottom clips into hand. Fix: SetPivot(0.5, 0.95)
  anchors near-bottom at the attachment point so menu extends upward. Redundant K2_SetRelativeLocation
  adds a per-item Z lift (cm) for additional margin. Both are applied in UpdateWidgetDrawSize().
- Menu jitter: W_GripDebug_L is rigidly attached to the player's hand bone with no smoothing applied.
  All VR controller tracking jitter (optical noise, occlusion) passes 1:1 to the menu. No fix here
  without detaching the menu from the hand and implementing a lerped/smoothed world-space anchor.
*/

#include "VRMenuSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "Cheats.hpp"
#include "HookManager.hpp"
#include "ArenaSubsystem.hpp"
#include "FriendSubsystem.hpp"
#include "LoadoutSubsystem.hpp"
#include "ModFeedback.hpp"

#include <cmath>
#include <array>
#include <sstream>
#include <algorithm>

namespace Mod
{
    // =========================================================================
    // Singleton
    // =========================================================================
    VRMenuSubsystem* VRMenuSubsystem::Get()
    {
        static VRMenuSubsystem instance;
        return &instance;
    }

    // =========================================================================
    // Stable FString ring buffer (same pattern as ModFeedback)
    // =========================================================================
    SDK::FString VRMenuSubsystem::MakeStableFString(const std::wstring& value)
    {
        static thread_local std::array<std::wstring, 64> ring;
        static thread_local uint32_t ringIndex = 0;

        std::wstring& slot = ring[ringIndex++ % ring.size()];
        slot = value;
        return SDK::FString(slot.c_str());
    }

    // =========================================================================
    // Page navigation helpers
    // =========================================================================
    void VRMenuSubsystem::NavigateToPage(MenuPage page)
    {
        // Push current page to stack for back navigation (unless going back)
        if (page != currentPage_)
        {
            pageStack_.push_back(currentPage_);
        }
        currentPage_ = page;
        selectedIndex_ = 0;
        BuildMenuItems();
        UpdateWidgetDrawSize();
        if (poc9WidgetCreated_)
            UpdatePoc9Widget();
    }

    std::wstring VRMenuSubsystem::GetPageTitle() const
    {
        switch (currentPage_)
        {
            case MenuPage::Main:         return L"=== MOD MENU ===";
            case MenuPage::Cheats:       return L"--- CHEATS ---";
            case MenuPage::Arena:        return L"--- ARENA CONFIG ---";
            case MenuPage::Loadouts:     return L"--- LOADOUTS ---";
            case MenuPage::LoadoutSelect: return L"--- SELECT LOADOUT ---";
            case MenuPage::SpawnFriend:  return L"--- SPAWN FRIEND ---";
            case MenuPage::FriendClass:  return L"--- SELECT CLASS ---";
            default:                     return L"=== MENU ===";
        }
    }

    void VRMenuSubsystem::UpdateWidgetDrawSize()
    {
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
        if (!player || !player->W_GripDebug_L)
            return;

        int itemCount = static_cast<int>(items_.size());
        float height = (std::max)(600.0f, static_cast<float>(itemCount * 100));
        
        SDK::FVector2D newSize{600.0, height};
        player->W_GripDebug_L->SetDrawSize(newSize);

        // Pivot adjustment: anchor the BOTTOM of the widget at the hand attachment point
        // so the menu extends UPWARD from the hand rather than clipping downward into it.
        // Pivot(0.5, 1.0) = bottom-center at component origin.
        // Adjust toward 0.9 to leave a small margin below the hand for readability.
        SDK::FVector2D pivot{0.5, 0.95};
        player->W_GripDebug_L->SetPivot(pivot);

        // Redundant position offset: shift the widget component upward so the bottom
        // edge clears the hand grip even if the game blueprint resets the pivot.
        // Scale with item count so taller menus float higher.
        // Units are UE cm. Each item is ~5 cm, we lift by 1 cm per item as a safety margin.
        const float kLiftPerItemCm = 2.0f;
        float zLiftCm = static_cast<float>(itemCount) * kLiftPerItemCm;
        SDK::FHitResult dummyHit{};
        player->W_GripDebug_L->K2_SetRelativeLocation(
            SDK::FVector{zLiftCm, 0.0f, 0.0f}, false, &dummyHit, true);
        // frame of reference is X forward along it's axis, Y is right, Z is up back towards the player, so we shift forward in X by zLiftCm to lift the menu up away from the hand

        LOG_INFO("[VRMenu] DrawSize=(" << newSize.X << "x" << height
                 << ") pivot=(" << pivot.X << "," << pivot.Y
                 << ") zLift=" << zLiftCm << "cm");
    }

    // =========================================================================
    // Menu items - page-based building
    // =========================================================================
    void VRMenuSubsystem::BuildMenuItems()
    {
        items_.clear();

        switch (currentPage_)
        {
            case MenuPage::Main:         BuildMainPage(); break;
            case MenuPage::Cheats:       BuildCheatsPage(); break;
            case MenuPage::Arena:        BuildArenaPage(); break;
            case MenuPage::Loadouts:     BuildLoadoutsPage(); break;
            case MenuPage::LoadoutSelect: BuildLoadoutSelectPage(); break;
            case MenuPage::SpawnFriend:  BuildSpawnFriendPage(); break;
            case MenuPage::FriendClass:  BuildFriendClassPage(); break;
        }
    }

    // =========================================================================
    // MAIN PAGE - Quick actions and navigation to sub-menus
    // =========================================================================
    void VRMenuSubsystem::BuildMainPage()
    {

        

        // Navigation: Cheats
        items_.push_back({"Cheats >",
            []() -> std::string { return ""; },
            [this]() { NavigateToPage(MenuPage::Cheats); },
            true
        });
        // Quick action: Arena Quick Start (start arena with current settings)
        items_.push_back({"Arena Quick Start",
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

        // Quick action: Spawn Friend (single friend with current class)
        items_.push_back({"Quick Spawn Friend",
            []() -> std::string {
                auto* f = Friend::FriendSubsystem::Get();
                return f ? std::to_string(f->ActiveFriendCount()) : "";
            },
            []() {
                auto* f = Friend::FriendSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (f && w) f->SpawnFriend(w);
            }
        });

        // Navigation: Loadouts
        items_.push_back({"Loadouts >",
            []() -> std::string { return ""; },
            [this]() { NavigateToPage(MenuPage::Loadouts); },
            true
        });

        // Navigation: Arena Config
        items_.push_back({"Arena Config >",
            [this]() -> std::string {
                return std::to_string(arenaEnemyCount_) + " enemies";
            },
            [this]() { NavigateToPage(MenuPage::Arena); },
            true
        });

        // Navigation: Spawn Friend Config
        items_.push_back({"Spawn Friend >",
            []() -> std::string { return ""; },
            [this]() { NavigateToPage(MenuPage::SpawnFriend); },
            true
        });

        // Quick action: Spawn Heal
        items_.push_back({"Spawn Heal Item",
            []() -> std::string { return ""; },
            []() {
                Cheats* c = GetCheats();
                SDK::UWorld* w = GameContext::GetWorld();
                if (c && w) c->SpawnHealItem(w);
            }
        });
    }

    // =========================================================================
    // CHEATS PAGE - All cheat toggles
    // =========================================================================
    void VRMenuSubsystem::BuildCheatsPage()
    {
        // Back button first
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });
        
        items_.push_back({"Combo Bypass",
            []() -> std::string {
                Cheats* c = GetCheats();
                // automag, durability bypass, and fatigue activate
                bool comboActive = c && c->IsAutoMagActive() && c->IsDurabilityBypassActive() && c->IsFatigueDisabledActive();
                return comboActive ? "ON" : "OFF";

            },
            []() {
                Cheats* c = GetCheats();
                if (c) {
                    // Toggle all three cheats together for a powerful combo
                    c->SetAutoMag(true);
                    c->ToggleDurabilityBypass();
                    c->ToggleFatigueDisabled();
                }
                
            }
        });

        items_.push_back({"No Anomalies",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsAnomaliesDisabledActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleAnomaliesDisabled();
            }
        });

        items_.push_back({"Durability Bypass",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsDurabilityBypassActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleDurabilityBypass();
            }
        });

        items_.push_back({"God Mode",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsGodModeActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleGodMode();
            }
        });

        items_.push_back({"Unlimited Ammo",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsUnlimitedAmmoActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleUnlimitedAmmo();
            }
        });

        items_.push_back({"No Hunger",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsHungerDisabledActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleHungerDisabled();
            }
        });

        items_.push_back({"No Fatigue",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsFatigueDisabledActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleFatigueDisabled();
            }
        });

        items_.push_back({"Bullet Time",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsBulletTimeActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleBulletTime();
            }
        });

        items_.push_back({"No Clip",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsNoClipActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleNoClip();
            }
        });

        items_.push_back({"Auto Mag",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c && c->IsAutoMagActive() ? "ON" : "OFF";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->ToggleAutoMag();
            }
        });

        // light scaling!
        items_.push_back({"Portable Light Intensity x2.0",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c ? std::to_string(c->GetPortableLightIntensityScale()) : "1.0";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->SetPortableLightIntensityScale(2.0f);
            }
        });
        items_.push_back({ "Portable Light Intensity x0.5",
            []() -> std::string {
                Cheats* c = GetCheats();
                return c ? std::to_string(c->GetPortableLightIntensityScale()) : "10.0";
            },
            []() {
                Cheats* c = GetCheats();
                if (c) c->SetPortableLightIntensityScale(0.5f);
            }
			});
            

    }

    // =========================================================================
    // ARENA PAGE - Arena configuration
    // =========================================================================
    void VRMenuSubsystem::BuildArenaPage()
    {
        // Back button
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });

        // Toggle Arena
        items_.push_back({"Toggle Arena",
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

        // Enemy count adjustment
        items_.push_back({"Enemies: +",
            [this]() -> std::string { return std::to_string(arenaEnemyCount_); },
            [this]() {
                arenaEnemyCount_ = (std::min)(200, arenaEnemyCount_ + 5);
            }
        });

        items_.push_back({"Enemies: -",
            [this]() -> std::string { return std::to_string(arenaEnemyCount_); },
            [this]() {
                arenaEnemyCount_ = (std::max)(1, arenaEnemyCount_ - 5);
            }
        });

        // Wave count adjustment
        items_.push_back({"Waves: +",
            [this]() -> std::string { return std::to_string(arenaWaveCount_); },
            [this]() {
                arenaWaveCount_ = (std::min)(100, arenaWaveCount_ + 1);
            }
        });

        items_.push_back({"Waves: -",
            [this]() -> std::string { return std::to_string(arenaWaveCount_); },
            [this]() {
                arenaWaveCount_ = (std::max)(1, arenaWaveCount_ - 1);
            }
        });
    }

    // =========================================================================
    // LOADOUTS PAGE - Loadout management
    // =========================================================================
    void VRMenuSubsystem::BuildLoadoutsPage()
    {
        // Back button
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });

        // Capture current loadout
        items_.push_back({"Capture Loadout",
            []() -> std::string { return ""; },
            []() {
                auto* ls = Loadout::LoadoutSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (ls && w) {
                    std::string result = ls->CaptureLoadout(w, "quick_capture");
                }
            }
        });

        // Select loadout to apply
        items_.push_back({"Select Loadout >",
            []() -> std::string {
                auto* ls = Loadout::LoadoutSubsystem::Get();
                return ls ? ls->GetSelectedLoadout() : "";
            },
            [this]() { NavigateToPage(MenuPage::LoadoutSelect); },
            true
        });

        // Apply selected loadout
        items_.push_back({"Apply Selected",
            []() -> std::string {
                auto* ls = Loadout::LoadoutSubsystem::Get();
                return ls ? ls->GetSelectedLoadout() : "";
            },
            []() {
                auto* ls = Loadout::LoadoutSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (ls && w) {
                    std::string selected = ls->GetSelectedLoadout();
                    if (!selected.empty()) {
                        ls->ApplyLoadout(w, selected);
                    }
                }
            }
        });

        // Clear loadout
        items_.push_back({"Clear Equipment",
            []() -> std::string { return ""; },
            []() {
                auto* ls = Loadout::LoadoutSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (ls && w) {
                    ls->ClearPlayerLoadout(w);
                }
            }
        });
    }

    // =========================================================================
    // LOADOUT SELECT PAGE - Select which loadout to use
    // =========================================================================
    void VRMenuSubsystem::BuildLoadoutSelectPage()
    {
        // Back button
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });

        // Get available loadouts from the Loadouts folder
        auto* ls = Loadout::LoadoutSubsystem::Get();
        if (ls) {
            std::string loadoutList = ls->ListLoadouts();
            // Parse the loadout list (format: "- name1\n- name2\n...")
            std::istringstream iss(loadoutList);
            std::string line;
            while (std::getline(iss, line)) {
                // Remove "- " prefix if present
                if (line.size() > 2 && line[0] == '-' && line[1] == ' ') {
                    line = line.substr(2);
                }
                // Trim whitespace
                while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                while (!line.empty() && line.front() == ' ')
                    line = line.substr(1);
                
                if (line.empty()) continue;
                
                std::string loadoutName = line;
                items_.push_back({loadoutName,
                    [loadoutName]() -> std::string {
                        auto* ls2 = Loadout::LoadoutSubsystem::Get();
                        if (ls2 && ls2->GetSelectedLoadout() == loadoutName)
                            return "SELECTED";
                        return "";
                    },
                    [loadoutName]() {
                        auto* ls2 = Loadout::LoadoutSubsystem::Get();
                        if (ls2) ls2->SetSelectedLoadout(loadoutName);
                    }
                });
            }
        }
    }

    // =========================================================================
    // SPAWN FRIEND PAGE - Friend NPC configuration
    // =========================================================================
    void VRMenuSubsystem::BuildSpawnFriendPage()
    {
        // Back button
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });

        // Spawn friend with current settings
        items_.push_back({"Spawn Friend",
            []() -> std::string {
                auto* f = Friend::FriendSubsystem::Get();
                return f ? std::to_string(f->ActiveFriendCount()) + " active" : "";
            },
            []() {
                auto* f = Friend::FriendSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (f && w) f->SpawnFriend(w);
            }
        });

        // Select friend class
        items_.push_back({"Select Class >",
            [this]() -> std::string {
                return selectedFriendClass_.empty() ? "Random" : selectedFriendClass_;
            },
            [this]() { NavigateToPage(MenuPage::FriendClass); },
            true
        });

        // Clear all friends
        items_.push_back({"Clear All Friends",
            []() -> std::string { return ""; },
            []() {
                auto* f = Friend::FriendSubsystem::Get();
                SDK::UWorld* w = GameContext::GetWorld();
                if (f && w) f->ClearAll(w);
            }
        });
    }

    // =========================================================================
    // FRIEND CLASS PAGE - Select which NPC class to spawn
    // =========================================================================
    void VRMenuSubsystem::BuildFriendClassPage()
    {
        // Back button
        items_.push_back({"< Back",
            []() -> std::string { return ""; },
            [this]() {
                if (!pageStack_.empty()) {
                    currentPage_ = pageStack_.back();
                    pageStack_.pop_back();
                    selectedIndex_ = 0;
                    BuildMenuItems();
                    UpdateWidgetDrawSize();
                    if (poc9WidgetCreated_) UpdatePoc9Widget();
                }
            }
        });

        // Random option
        items_.push_back({"Random",
            [this]() -> std::string {
                return selectedFriendClass_.empty() ? "SELECTED" : "";
            },
            [this]() {
                selectedFriendClass_ = "";
            }
        });

        // Get available friend classes from arena discovered NPCs
        auto* arena = Arena::ArenaSubsystem::Get();
        if (arena) {
            const auto& npcs = arena->GetDiscoveredNPCs();
            for (const auto& npcClass : npcs) {
                // Extract short name from full path
                std::string shortName = npcClass;
                size_t lastSlash = shortName.rfind('/');
                if (lastSlash != std::string::npos)
                    shortName = shortName.substr(lastSlash + 1);
                // Remove BP_ prefix and _C suffix if present
                if (shortName.size() > 3 && shortName.substr(0, 3) == "BP_")
                    shortName = shortName.substr(3);
                if (shortName.size() > 2 && shortName.substr(shortName.size() - 2) == "_C")
                    shortName = shortName.substr(0, shortName.size() - 2);
                
                std::string fullClass = npcClass;
                items_.push_back({shortName,
                    [this, fullClass]() -> std::string {
                        return selectedFriendClass_ == fullClass ? "SELECTED" : "";
                    },
                    [this, fullClass]() {
                        selectedFriendClass_ = fullClass;
                    }
                });
            }
        }
    }

    // =========================================================================
    // Initialize
    // =========================================================================
    void VRMenuSubsystem::Initialize()
    {
        if (initialized_)
            return;

        BuildMenuItems();
        initialized_ = true;
        LOG_INFO("[VRMenu] Initialized with " << items_.size() << " items, DebugStr=" << debugStringEnabled_ << " Widget=" << widgetEnabled_);
    }

    // =========================================================================
    // Input hooks
    // =========================================================================

    // --- Grip state tracking ---
    void VRMenuSubsystem::OnGripPressed()
    {
        gripHeld_.store(true, std::memory_order_relaxed);
    }

    void VRMenuSubsystem::OnGripReleased()
    {
        gripHeld_.store(false, std::memory_order_relaxed);
    }

    // --- B/Y button: dual-purpose (Grip+BY = toggle, BY alone = select) ---
    bool VRMenuSubsystem::OnButtonBY()
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastToggleTime_).count();

        bool isGripHeld = gripHeld_.load(std::memory_order_relaxed);
        bool isOpen = menuOpen_.load();

        if (isGripHeld)
        {
            // --- GRIP + B/Y = TOGGLE MENU ---
            if (elapsed < 500)
            {
                return isOpen; // suppress if menu is open
            }
            lastToggleTime_ = now;

            bool wasOpen = isOpen;
            menuOpen_.store(!wasOpen);

            if (!wasOpen)
            {
                // Opening menu - reset to main page
                currentPage_ = MenuPage::Main;
                pageStack_.clear();
                selectedIndex_ = 0;
                BuildMenuItems();
                
                if (poc9Enabled_)
                    CreatePoc9Widget();
            }
            else
            {
                // Closing menu - reset state for next open
                currentPage_ = MenuPage::Main;
                pageStack_.clear();
                selectedIndex_ = 0;
                
                if (poc9Enabled_)
                    DestroyPoc9Widget();
            }

            return true; // suppress the B/Y button
        }
        else if (isOpen)
        {
            // --- B/Y WITHOUT GRIP (menu open) = SELECT ITEM ---
            if (elapsed < 300)
            {
                return true;
            }
            lastToggleTime_ = now;

            if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size()))
            {
                auto& item = items_[selectedIndex_];

                try
                {
                    item.actionFn();
                    // Menu updates automatically via UpdatePoc9Widget, no ShowMessage needed
                }
                catch (...)
                {
                    // Silent failure
                }

                if (poc9Enabled_ && poc9WidgetCreated_)
                    UpdatePoc9Widget();
            }

            return true; // suppress B/Y while menu is open
        }

        // Menu is closed and no grip held — don't consume the button
        return false;
    }

    // --- Thumbstick navigation with anti-rebound ---
    bool VRMenuSubsystem::OnNavigate(float thumbstickY)
    {
        if (!menuOpen_.load())
            return false; // don't consume input when menu is closed

        // Navigation deadzone and repeat rate
        const float deadzone = 0.5f;
        const int repeatMs = 250;

        // Anti-rebound: when inside deadzone, reset direction to CENTER
        if (std::abs(thumbstickY) < deadzone)
        {
            if (lastNavDirection_ != NavDirection::CENTER)
            {
                LOG_INFO("[VRMenu:OnNavigate] y=" << thumbstickY
                         << " inside deadzone — resetting from "
                         << (lastNavDirection_ == NavDirection::UP ? "UP" : "DOWN")
                         << " to CENTER");
            }
            lastNavDirection_ = NavDirection::CENTER;
            return true; // suppress movement while menu is open
        }

        // Determine intended direction
        NavDirection intended = (thumbstickY > 0) ? NavDirection::UP : NavDirection::DOWN;
        const char* intendedStr = (intended == NavDirection::DOWN) ? "DOWN" : "UP";
        const char* lastStr = (lastNavDirection_ == NavDirection::CENTER) ? "CENTER"
                            : (lastNavDirection_ == NavDirection::DOWN)   ? "DOWN" : "UP";

        // Anti-rebound: only accept navigation if we were at CENTER or same direction
        // This prevents stick overshoot from triggering opposite navigation
        if (lastNavDirection_ != NavDirection::CENTER && lastNavDirection_ != intended)
        {
            LOG_INFO("[VRMenu:OnNavigate] y=" << thumbstickY
                     << " REBOUND BLOCKED: intended=" << intendedStr
                     << " last=" << lastStr
                     << " (stick must return to CENTER before reversing direction)");
            return true;
        }

        // Rate limiting
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNavTime_).count();
        if (elapsed < repeatMs)
            return true;

        lastNavTime_ = now;
        lastNavDirection_ = intended;

        const int prevIndex = selectedIndex_;

        if (intended == NavDirection::DOWN)
        {
            selectedIndex_++;
            if (selectedIndex_ >= static_cast<int>(items_.size()))
                selectedIndex_ = 0;
        }
        else
        {
            selectedIndex_--;
            if (selectedIndex_ < 0)
                selectedIndex_ = static_cast<int>(items_.size()) - 1;
        }

        LOG_INFO("[VRMenu:OnNavigate] y=" << thumbstickY
                 << " direction=" << intendedStr
                 << " index " << prevIndex << " -> " << selectedIndex_
                 << " (of " << items_.size() << ")");

        // Update POC9 widget to reflect new selection
        if (poc9Enabled_ && poc9WidgetCreated_)
        {
            UpdatePoc9Widget();
        }

        return true; // suppress movement while menu is open
    }

    // =========================================================================
    // Update (called from game thread)
    // =========================================================================
    void VRMenuSubsystem::Update(SDK::UWorld* world)
    {
        if (!initialized_)
            return;

        if (!menuOpen_.load())
            return; // nothing to render

        if (!world)
            return;

        if (debugStringEnabled_)
        {
            RenderDebugString(world);
        }

        if (widgetEnabled_)
        {
            RenderWidget(world);
        }

        // Keep POC9 widget updated (e.g., for status changes from external triggers)
        if (poc9Enabled_ && poc9WidgetCreated_)
        {
            UpdatePoc9Widget();
        }
    }

    // =========================================================================
    // Approach 1: DrawDebugString — floating text near left hand
    // WARNING: DrawDebugString has NEVER been verified to render visibly in VR.
    // The debug draw pipeline may be stripped in shipping/VR builds.
    // This approach is kept as a diagnostic experiment — check logs to see if
    // the calls go through. If calls execute but nothing appears, the debug
    // canvas is disabled in this build.
    // =========================================================================
    void VRMenuSubsystem::RenderDebugString(SDK::UWorld* world)
    {
        SDK::UObject* ctx = GameContext::GetWorldContext();
        if (!ctx)
        {
            static int ctxLogs = 0;
            if (ctxLogs < 3) { ++ctxLogs; LOG_WARN("[VRMenu:DebugStr] WorldContext is null — cannot draw"); }
            return;
        }

        // Log first successful entry to help diagnose whether calls reach the engine
        static bool loggedFirstCall = false;
        if (!loggedFirstCall)
        {
            loggedFirstCall = true;
            LOG_INFO("[VRMenu:DebugStr] First DrawDebugString call executing. "
                     "If you see this log but NO text in VR, the debug draw pipeline is stripped/disabled.");
        }

        // Try to get left hand position from the gameplay character
        auto* player = GameContext::GetPlayerCharacter();
        if (!player)
            return;

        // Get the left grab sphere component for hand position
        SDK::UGrabSphere* leftHand = player->GrabSphereLeft_BP;

        SDK::FVector basePos;
        SDK::FVector forward, right, up;

        if (leftHand)
        {
            basePos = leftHand->K2_GetComponentLocation();
            forward = leftHand->GetForwardVector();
            right   = leftHand->GetRightVector();
            up      = leftHand->GetUpVector();
        }
        else
        {
            // Fallback: use player view + offset
            SDK::FVector viewLoc;
            SDK::FRotator viewRot;
            if (!GameContext::GetPlayerView(world, viewLoc, viewRot))
                return;

            // Simple forward vector from yaw only
            double yawRad = viewRot.Yaw * 3.14159265358979 / 180.0;
            forward = SDK::FVector(std::cos(yawRad), std::sin(yawRad), 0.0);
            right   = SDK::FVector(std::sin(yawRad), -std::cos(yawRad), 0.0);
            up      = SDK::FVector(0.0, 0.0, 1.0);
            basePos = viewLoc + forward * 40.0 + SDK::FVector(0.0, -30.0, -20.0);

            static int fallbackLogs = 0;
            if (fallbackLogs < 5)
            {
                ++fallbackLogs;
                LOG_WARN("[VRMenu] GrabSphereLeft_BP is null, using view fallback");
            }
        }

        // Position the menu: slightly in front and above the left hand
        SDK::FVector menuOrigin = basePos + forward * 20.0 + up * 15.0 + right * 5.0;

        // Prevent reentrant hook dispatch during DrawDebugString calls
        ScopedProcessEventGuard guard;

        // Draw the header
        const float duration = 0.15f; // overlap frames to avoid flicker
        const SDK::FLinearColor headerColor{0.0f, 0.85f, 1.0f, 1.0f};   // cyan
        const SDK::FLinearColor selectedColor{0.2f, 1.0f, 0.2f, 1.0f};  // green
        const SDK::FLinearColor normalColor{0.85f, 0.85f, 0.85f, 1.0f}; // light gray
        const SDK::FLinearColor statusOnColor{0.3f, 1.0f, 0.5f, 1.0f};  // green-ish
        const SDK::FLinearColor statusOffColor{0.6f, 0.6f, 0.6f, 1.0f}; // dim gray

        // Line spacing in world units (cm). ~2cm between lines.
        const double lineSpacing = 2.5;

        // Header
        SDK::FString headerStr = MakeStableFString(L"=== MOD MENU ===");
        SDK::UKismetSystemLibrary::DrawDebugString(ctx, menuOrigin, headerStr, nullptr, headerColor, duration);

        // Items
        for (int i = 0; i < static_cast<int>(items_.size()); i++)
        {
            SDK::FVector linePos = menuOrigin - up * (lineSpacing * (i + 1));

            bool isSelected = (i == selectedIndex_);
            const auto& item = items_[i];

            // Build display string
            std::wstring display;
            if (isSelected)
                display = L">> ";
            else
                display = L"   ";

            // Convert label to wstring
            std::wstring wlabel(item.label.begin(), item.label.end());
            display += wlabel;

            // Append status
            std::string status;
            try { status = item.statusFn(); } catch (...) { status = "?"; }
            if (!status.empty())
            {
                display += L" [";
                display += std::wstring(status.begin(), status.end());
                display += L"]";
            }

            SDK::FString lineStr = MakeStableFString(display);
            const SDK::FLinearColor& color = isSelected ? selectedColor : normalColor;
            SDK::UKismetSystemLibrary::DrawDebugString(ctx, linePos, lineStr, nullptr, color, duration);
        }

        // Footer
        SDK::FVector footerPos = menuOrigin - up * (lineSpacing * (static_cast<int>(items_.size()) + 1));
        SDK::FString footerStr = MakeStableFString(L"[grip+B/Y]=toggle  [stick]=nav  [B/Y]=select");
        SDK::UKismetSystemLibrary::DrawDebugString(ctx, footerPos, footerStr, nullptr, headerColor, duration);
    }

    // =========================================================================
    // Approach 2: Widget hijack — find existing UTextBlock and repurpose
    // =========================================================================
    void VRMenuSubsystem::TryFindWidgetComponent(SDK::UWorld* world)
    {
        if (widgetSearched_)
            return;
        widgetSearched_ = true;

        LOG_INFO("[VRMenu] Searching for PlayerDebugWidget on gameplay character...");

        auto* player = GameContext::GetPlayerCharacter();
        if (!player)
        {
            LOG_WARN("[VRMenu] No player character for widget search");
            widgetSearchFailed_ = true;
            return;
        }

        // The gameplay character has PlayerDebugWidget (UWidgetComponent*) at a known offset
        SDK::UWidgetComponent* debugWidget = player->PlayerDebugWidget;
        if (!debugWidget)
        {
            LOG_WARN("[VRMenu] PlayerDebugWidget is null on player");
            widgetSearchFailed_ = true;
            return;
        }

        LOG_INFO("[VRMenu] Found PlayerDebugWidget: " << debugWidget->GetFullName());


        // Check if it has an existing UUserWidget
        SDK::UUserWidget* existingWidget = debugWidget->GetWidget();
        if (existingWidget)
        {
            LOG_INFO("[VRMenu] PlayerDebugWidget has existing widget: " << existingWidget->GetFullName());

            // Try to find a UTextBlock child in the widget tree
            if (existingWidget->WidgetTree)
            {
                SDK::UWidget* root = existingWidget->WidgetTree->RootWidget;
                if (root)
                {
                    LOG_INFO("[VRMenu] Widget tree root: " << root->GetFullName());

                    // Check if root is a text block
                    if (root->IsA(SDK::UTextBlock::StaticClass()))
                    {
                        SDK::UTextBlock* textBlock = static_cast<SDK::UTextBlock*>(root);
                        cachedTextBlock_ = textBlock;
                        cachedWidgetComp_ = debugWidget;
                        LOG_INFO("[VRMenu] Found TextBlock root widget — will use for menu text");
                        return;
                    }
                    else
                    {
                        std::string rootClassName = root->Class ? root->Class->GetName() : "???";
                        LOG_INFO("[VRMenu] Root widget is " << rootClassName << ", not TextBlock. Attempting GObjects scan for TextBlock...");
                    }
                }
            }
        }
        else
        {
            LOG_INFO("[VRMenu] PlayerDebugWidget has no widget assigned");
        }

        // Fallback: scan GObjects for any UTextBlock we could use
        LOG_INFO("[VRMenu] Scanning GObjects for UTextBlock instances...");
        if (!SDK::UObject::GObjects)
        {
            LOG_WARN("[VRMenu] GObjects is null; cannot scan for TextBlock instances");
            widgetSearchFailed_ = true;
            return;
        }

        int totalObjects = SDK::UObject::GObjects->Num();
        if (totalObjects <= 0)
        {
            widgetSearchFailed_ = true;
            return;
        }

        int textBlockCount = 0;
        int scanLimit = totalObjects < 200000 ? totalObjects : 200000;
        for (int idx = 0; idx < scanLimit; idx++)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(idx);
            if (!obj || !obj->Class)
                continue;

            std::string className = obj->Class->GetName();
            if (className == "TextBlock")
            {
                textBlockCount++;
                if (textBlockCount <= 5)
                {
                    LOG_INFO("[VRMenu] GObjects TextBlock #" << textBlockCount << ": " << obj->GetFullName());
                }
            }
        }
        LOG_INFO("[VRMenu] Total TextBlock instances in GObjects: " << textBlockCount);

        // For now, we don't try to steal a random TextBlock — too risky.
        // The widget approach needs the PlayerDebugWidget to already have a TextBlock.
        cachedWidgetComp_ = debugWidget;
        widgetSearchFailed_ = (cachedTextBlock_ == nullptr);

        if (widgetSearchFailed_)
        {
            LOG_WARN("[VRMenu] Widget approach: no usable TextBlock found. Widget rendering will be disabled.");
        }
    }

    void VRMenuSubsystem::RenderWidget(SDK::UWorld* world)
    {
        if (widgetSearchFailed_)
            return;

        if (!widgetSearched_)
            TryFindWidgetComponent(world);

        if (!cachedTextBlock_ || !cachedWidgetComp_)
            return;

        // Build the full menu text
        std::wstring fullText = L"=== MOD MENU ===\n";

        for (int i = 0; i < static_cast<int>(items_.size()); i++)
        {
            bool isSelected = (i == selectedIndex_);
            const auto& item = items_[i];

            if (isSelected)
                fullText += L">> ";
            else
                fullText += L"   ";

            fullText += std::wstring(item.label.begin(), item.label.end());

            std::string status;
            try { status = item.statusFn(); } catch (...) { status = "?"; }
            if (!status.empty())
            {
                fullText += L" [";
                fullText += std::wstring(status.begin(), status.end());
                fullText += L"]";
            }
            fullText += L"\n";
        }

        fullText += L"\n[grip+B/Y]=toggle  [stick]=nav  [B/Y]=select";

        // Set the text on the text block
        try
        {
            ScopedProcessEventGuard guard;  // Prevent reentrant hook dispatch

            SDK::FString fstr = MakeStableFString(fullText);
            SDK::FText ftext = SDK::UKismetTextLibrary::Conv_StringToText(fstr);
            cachedTextBlock_->SetText(ftext);

            // Make sure the widget component redraws
            cachedWidgetComp_->RequestRedraw();

            static bool loggedFirstRender = false;
            if (!loggedFirstRender)
            {
                loggedFirstRender = true;
                LOG_INFO("[VRMenu:Widget] First widget render completed. If visible in VR, widget hijack works!");
            }
        }
        catch (...)
        {
            LOG_ERROR("[VRMenu] Exception in RenderWidget — disabling widget approach");
            widgetSearchFailed_ = true;
            cachedTextBlock_ = nullptr;
        }
    }

    // =========================================================================
    // Approach 3: POC9 Widget — WBP_Confirmation attached to left hand
    // This approach creates a proper UMG widget with TextBlocks and attaches
    // it to the W_GripDebug_L WidgetComponent on the player's left hand.
    // =========================================================================
    void VRMenuSubsystem::CreatePoc9Widget()
    {
        LOG_INFO("[VRMenu:POC9] Creating WBP_Confirmation widget on left hand");
        
        if (cachedPoc9Widget_)
        {
            LOG_WARN("[VRMenu:POC9] Widget already exists, skipping creation");
            return;
        }

        SDK::UWorld* world = GameContext::GetWorld();
        if (!world)
        {
            LOG_WARN("[VRMenu:POC9] No world");
            return;
        }

        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
        if (!player)
        {
            LOG_WARN("[VRMenu:POC9] No player");
            return;
        }

        SDK::UWidgetComponent* widgetComp = player->W_GripDebug_L;
        if (!widgetComp)
        {
            LOG_WARN("[VRMenu:POC9] W_GripDebug_L is null");
            return;
        }

        // Get player controller
        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!pc)
        {
            LOG_WARN("[VRMenu:POC9] No PlayerController");
            return;
        }

        // Get WBP_Confirmation_C class
        SDK::UClass* confirmWidgetClass = SDK::UWBP_Confirmation_C::StaticClass();
        if (!confirmWidgetClass)
        {
            LOG_WARN("[VRMenu:POC9] WBP_Confirmation_C class not found");
            return;
        }

        // Create the widget
        SDK::UUserWidget* widget = SDK::UWidgetBlueprintLibrary::Create(world, confirmWidgetClass, pc);
        if (!widget)
        {
            LOG_WARN("[VRMenu:POC9] Failed to create widget");
            return;
        }

        if (!widget->IsA(SDK::UWBP_Confirmation_C::StaticClass()))
        {
            LOG_WARN("[VRMenu:POC9] Created widget is not UWBP_Confirmation_C");
            return;
        }

        cachedPoc9Widget_ = static_cast<SDK::UWBP_Confirmation_C*>(widget);
        LOG_INFO("[VRMenu:POC9] Created widget: " << widget->GetFullName());

        // Set the widget on the widget component
        widgetComp->SetWidget(cachedPoc9Widget_);
        
        // Make visible
        widgetComp->SetVisibility(true, true);
        widgetComp->SetHiddenInGame(false, true);
        widgetComp->SetTwoSided(true);
        
        poc9WidgetCreated_ = true;

        // Set draw size based on number of menu items
        UpdateWidgetDrawSize();

        // Initial text update
        UpdatePoc9Widget();

        // Try to find a VR pointer interaction component (experimental)
        TryScanForVRPointer();

        LOG_INFO("[VRMenu:POC9] Widget attached to left hand");
    }

    void VRMenuSubsystem::DestroyPoc9Widget()
    {
        LOG_INFO("[VRMenu:POC9] Destroying widget");

        if (!cachedPoc9Widget_)
        {
            LOG_INFO("[VRMenu:POC9] No widget to destroy");
            poc9WidgetCreated_ = false;
            return;
        }

        // Get the widget component and clear it
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
        if (player && player->W_GripDebug_L)
        {
            player->W_GripDebug_L->SetWidget(nullptr);
            player->W_GripDebug_L->SetVisibility(false, true);
            LOG_INFO("[VRMenu:POC9] Cleared W_GripDebug_L widget");
        }

        // Note: We can't properly destroy UObjects, so we just null our pointer
        cachedPoc9Widget_ = nullptr;
        poc9WidgetCreated_ = false;

        LOG_INFO("[VRMenu:POC9] Widget destroyed");
    }

    void VRMenuSubsystem::UpdatePoc9Widget()
    {
        if (!cachedPoc9Widget_)
            return;

        // Build menu text — items only, NO header (title is set separately to avoid duplication)
        std::wostringstream menu;

        for (int i = 0; i < static_cast<int>(items_.size()); i++)
        {
            auto& item = items_[i];

            // Selection indicator
            if (i == selectedIndex_)
                menu << L">> ";
            else
                menu << L"   ";

            // Item label
            menu << std::wstring(item.label.begin(), item.label.end());

            // Status
            std::string status;
            try { status = item.statusFn(); } catch (...) { status = "?"; }
            if (!status.empty())
            {
                menu << L" [";
                menu << std::wstring(status.begin(), status.end());
                menu << L"]";
            }

            if (i < static_cast<int>(items_.size()) - 1)
                menu << L"\n";
        }

        menu << L"\n\n[grip+B/Y]=close  [stick]=nav  [B/Y]=select";

        // Create FText values with dynamic title based on current page
        std::wstring titleWStr = GetPageTitle();
        SDK::FString titleStr(titleWStr.c_str());
        SDK::FText titleText = SDK::UKismetTextLibrary::Conv_StringToText(titleStr);

        std::wstring menuStr = menu.str();
        SDK::FString descStr(menuStr.c_str());
        SDK::FText descText = SDK::UKismetTextLibrary::Conv_StringToText(descStr);

        SDK::FString yesStr(L"Select");
        SDK::FText yesText = SDK::UKismetTextLibrary::Conv_StringToText(yesStr);

        SDK::FString noStr(L"Close");
        SDK::FText noText = SDK::UKismetTextLibrary::Conv_StringToText(noStr);

        // Set properties
        cachedPoc9Widget_->Title = titleText;
        cachedPoc9Widget_->Description = descText;
        cachedPoc9Widget_->Yes_Text = yesText;
        cachedPoc9Widget_->No_Text = noText;

        // Try to set TextBlock directly (title)
        if (cachedPoc9Widget_->Txt_Confirmation_Title)
        {
            cachedPoc9Widget_->Txt_Confirmation_Title->SetText(titleText);

            SDK::FSlateColor whiteTitle;
            whiteTitle.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
            cachedPoc9Widget_->Txt_Confirmation_Title->SetColorAndOpacity(whiteTitle);
        }

        // Set MultiLineEditableText directly (description/menu content)
        if (cachedPoc9Widget_->Txt_TextConfirm)
        {
            cachedPoc9Widget_->Txt_TextConfirm->SetText(descText);

            SDK::FTextBlockStyle whiteStyle = cachedPoc9Widget_->Txt_TextConfirm->WidgetStyle;
            whiteStyle.ColorAndOpacity.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
            cachedPoc9Widget_->Txt_TextConfirm->SetWidgetStyle(whiteStyle);
        }

        // Request redraw on the widget component
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
        if (player && player->W_GripDebug_L)
        {
            player->W_GripDebug_L->RequestRedraw();
        }
    }

    // =========================================================================
    // VR Pointer Interaction (experimental)
    // Scan for existing UWidgetInteractionComponent in the scene.
    // The game uses URadiusWidgetInteractionComponent (subclass) with beam,
    // but we'll accept any UWidgetInteractionComponent.
    // =========================================================================
    void VRMenuSubsystem::TryScanForVRPointer()
    {
        if (vrPointerScanned_)
            return;
        vrPointerScanned_ = true;

        LOG_INFO("[VRMenu:Pointer] Scanning for UWidgetInteractionComponent instances...");

        SDK::UClass* wicClass = SDK::UWidgetInteractionComponent::StaticClass();
        if (!wicClass)
        {
            LOG_WARN("[VRMenu:Pointer] UWidgetInteractionComponent::StaticClass() is null");
            return;
        }

        if (!SDK::UObject::GObjects)
        {
            LOG_WARN("[VRMenu:Pointer] GObjects is null; cannot scan for pointers");
            return;
        }

        int totalObjects = SDK::UObject::GObjects->Num();
        int scanLimit = (totalObjects < 300000) ? totalObjects : 300000;
        int foundCount = 0;

        for (int idx = 0; idx < scanLimit; idx++)
        {
            SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(idx);
            if (!obj)
                continue;

            // Check if this is a UWidgetInteractionComponent (or subclass)
            if (!obj->IsA(wicClass))
                continue;

            foundCount++;
            SDK::UWidgetInteractionComponent* wic = static_cast<SDK::UWidgetInteractionComponent*>(obj);

            std::string fullName = obj->GetFullName();
            LOG_INFO("[VRMenu:Pointer] Found WIC #" << foundCount << ": " << fullName);

            // Check if it has a valid outer (attached to something)
            std::string outerName = obj->Outer ? obj->Outer->GetFullName() : "<null>";
            LOG_INFO("[VRMenu:Pointer]   Outer: " << outerName);

            // Try to use the first one we find that belongs to a player character
            if (!cachedVRPointer_ && outerName.find("PlayerCharacter") != std::string::npos)
            {
                cachedVRPointer_ = wic;
                LOG_INFO("[VRMenu:Pointer] Selected WIC for pointer interaction: " << fullName);
            }
        }

        LOG_INFO("[VRMenu:Pointer] Scan complete. Found " << foundCount << " WIC instances. "
                 << (cachedVRPointer_ ? "USING one for interaction." : "None attached to player."));

        // If we found a pointer, try to configure it for our widget
        if (cachedVRPointer_)
        {
            try
            {
                // Enable hit testing and set interaction distance
                cachedVRPointer_->bEnableHitTesting = true;
                cachedVRPointer_->InteractionDistance = 100.0f; // 1 meter
                cachedVRPointer_->bShowDebug = true; // Show debug beam so user can see it
                LOG_INFO("[VRMenu:Pointer] Configured pointer: hitTest=true, dist=100, debug=true");
            }
            catch (...)
            {
                LOG_ERROR("[VRMenu:Pointer] Exception configuring pointer");
                cachedVRPointer_ = nullptr;
            }
        }
    }
}