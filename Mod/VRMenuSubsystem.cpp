/*
AILEARNINGS:
- DrawDebugString is UNVERIFIED in VR. ShowWorldText wraps it but has zero callers — never tested.
  The debug draw pipeline may be stripped in VR/shipping builds. Only subtitles are confirmed visible.
  We keep Approach 1 as a diagnostic experiment with extensive logging.
- DrawDebugString duration of 0.0f means one frame in UE. We use ~0.15s to bridge frame gaps.
- FInputActionValue is 0x20 bytes of opaque data. For a boolean action, the first byte is likely
  the bool (or first 4 bytes are a float 0.0/1.0). For a 2D axis, expect two floats.
  We log the raw bytes on first intercept to verify.
- The menu toggle hook uses InpActEvt_IA_Button2_Left (Y/B button on left controller).
  There are two variants (_17 and _18) corresponding to Started and Completed triggers.
  We only act on one and debounce with 500ms.
- Navigation uses InpActEvt_IA_Movement (left thumbstick). We only consume this when menu is open.
  When closed, we return false so normal movement works.
- Selection uses InpActEvt_IA_Run_Toggle (left stick press). Only consumed when menu is open.
- For widget hijack: we scan GObjects for UTextBlock instances and try to repurpose one on the
  player's PlayerDebugWidget. This is highly experimental.
*/

#include "VRMenuSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "Cheats.hpp"
#include "HookManager.hpp"
#include "ArenaSubsystem.hpp"
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
    // Menu items
    // =========================================================================
    void VRMenuSubsystem::BuildMenuItems()
    {
        items_.clear();

        // -- Cheats --
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

        // -- Arena --
        items_.push_back({"Arena Start",
            []() -> std::string {
                auto* a = Arena::ArenaSubsystem::Get();
                return (a && a->IsActive()) ? "ACTIVE" : "STOPPED";
            },
            []() {
                auto* a = Arena::ArenaSubsystem::Get();
                if (!a) return;
                if (a->IsActive())
                    a->Stop();
                else
                    a->Start();  // Use defaults for enemy count and distance
            }
        });

        items_.push_back({"Spawn Heal",
            []() -> std::string { return ""; },
            []() {
                Cheats* c = GetCheats();
                SDK::UWorld* w = GameContext::GetWorld();
                if (c && w) c->SpawnHealItem(w);
            }
        });

        // -- Rendering approach toggles (meta) --
        items_.push_back({"[DebugStr Render]",
            [this]() -> std::string { return debugStringEnabled_ ? "ON" : "OFF"; },
            [this]() { debugStringEnabled_ = !debugStringEnabled_; }
        });

        items_.push_back({"[Widget Render]",
            [this]() -> std::string { return widgetEnabled_ ? "ON" : "OFF"; },
            [this]() {
                widgetEnabled_ = !widgetEnabled_;
                if (!widgetEnabled_)
                {
                    // Clear cached widget state when disabling
                    cachedTextBlock_ = nullptr;
                }
            }
        });

        LOG_INFO("[VRMenu] Built " << items_.size() << " menu items");
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
    bool VRMenuSubsystem::OnToggleMenu()
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastToggleTime_).count();
        if (elapsed < 500)
        {
            // Debounce: ignore toggles within 500ms
            return menuOpen_.load();  // still suppress if menu is open
        }
        lastToggleTime_ = now;

        bool wasOpen = menuOpen_.load();
        menuOpen_.store(!wasOpen);

        LOG_INFO("[VRMenu] Menu toggled " << (wasOpen ? "CLOSED" : "OPEN"));

        if (!wasOpen)
        {
            // Opening menu — show confirmation and create POC9 widget
            Mod::ModFeedback::ShowMessage(L"[Mod Menu] OPENED — Use left stick to navigate, left stick press to select",
                3.0f, SDK::FLinearColor{0.3f, 1.0f, 0.3f, 1.0f});
            
            // Create the POC9 widget on the left hand
            if (poc9Enabled_)
            {
                CreatePoc9Widget();
            }
        }
        else
        {
            Mod::ModFeedback::ShowMessage(L"[Mod Menu] CLOSED",
                2.0f, SDK::FLinearColor{0.8f, 0.8f, 0.8f, 1.0f});
            
            // Destroy the POC9 widget
            if (poc9Enabled_)
            {
                DestroyPoc9Widget();
            }
        }

        return true; // always suppress the original button action
    }

    bool VRMenuSubsystem::OnNavigate(float thumbstickY)
    {
        if (!menuOpen_.load())
            return false; // don't consume input when menu is closed

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNavTime_).count();

        // Navigation deadzone and repeat rate
        const float deadzone = 0.6f;
        const int repeatMs = 250;

        if (std::abs(thumbstickY) < deadzone)
            return true; // inside deadzone but menu is open — still suppress movement

        if (elapsed < repeatMs)
            return true; // rate limit

        lastNavTime_ = now;

        if (thumbstickY > deadzone)
        {
            // Down
            selectedIndex_++;
            if (selectedIndex_ >= static_cast<int>(items_.size()))
                selectedIndex_ = 0;
        }
        else if (thumbstickY < -deadzone)
        {
            // Up
            selectedIndex_--;
            if (selectedIndex_ < 0)
                selectedIndex_ = static_cast<int>(items_.size()) - 1;
        }

        // Update POC9 widget to reflect new selection
        if (poc9Enabled_ && poc9WidgetCreated_)
        {
            UpdatePoc9Widget();
        }

        return true; // suppress movement while menu is open
    }

    bool VRMenuSubsystem::OnSelect()
    {
        if (!menuOpen_.load())
            return false; // don't consume input when menu is closed

        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size()))
        {
            auto& item = items_[selectedIndex_];
            LOG_INFO("[VRMenu] Executing: " << item.label);

            try
            {
                item.actionFn();
            }
            catch (...)
            {
                LOG_ERROR("[VRMenu] Exception in action for: " << item.label);
            }

            // Update POC9 widget to reflect new state after action
            if (poc9Enabled_ && poc9WidgetCreated_)
            {
                UpdatePoc9Widget();
            }
        }

        return true; // suppress trigger while menu is open
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
        SDK::FString footerStr = MakeStableFString(L"[Y/B]=close  [stick]=nav  [stick press]=select");
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
                    SDK::UTextBlock* textBlock = static_cast<SDK::UTextBlock*>(root);
                    // Validate by checking the class name
                    std::string rootClassName = root->Class ? root->Class->GetName() : "???";
                    if (rootClassName.find("TextBlock") != std::string::npos)
                    {
                        cachedTextBlock_ = textBlock;
                        cachedWidgetComp_ = debugWidget;
                        LOG_INFO("[VRMenu] Found TextBlock root widget — will use for menu text");
                        return;
                    }
                    else
                    {
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

        fullText += L"\n[Y/B]=close  [stick]=nav  [stick press]=select";

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

        cachedPoc9Widget_ = static_cast<SDK::UWBP_Confirmation_C*>(widget);
        LOG_INFO("[VRMenu:POC9] Created widget: " << widget->GetFullName());

        // Set the widget on the widget component
        widgetComp->SetWidget(cachedPoc9Widget_);
        
        // Make visible
        widgetComp->SetVisibility(true, true);
        widgetComp->SetHiddenInGame(false, true);
        widgetComp->SetTwoSided(true);
        
        poc9WidgetCreated_ = true;

        // Initial text update
        UpdatePoc9Widget();

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

        // Build menu text
        std::wostringstream menu;
        menu << L"=== MOD MENU ===";

        for (int i = 0; i < static_cast<int>(items_.size()); i++)
        {
            auto& item = items_[i];

            // Selection indicator
            if (i == selectedIndex_)
                menu << L"\n>> ";
            else
                menu << L"\n   ";

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
        }

        menu << L"\n\n[A]=close  [stick]=nav  [stick press]=select";

        // Create FText values
        SDK::FString titleStr(L"=== MOD MENU ===");
        SDK::FText titleText = SDK::UKismetTextLibrary::Conv_StringToText(titleStr);

        std::wstring menuStr = menu.str();
        SDK::FString descStr(menuStr.c_str());
        SDK::FText descText = SDK::UKismetTextLibrary::Conv_StringToText(descStr);

        SDK::FString yesStr(L"Toggle");
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
}
