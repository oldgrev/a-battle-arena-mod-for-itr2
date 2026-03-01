/**
 * MenuPocSubsystem.cpp
 * 
 * Proof-of-concept implementations for mod menu using various approaches.
 * Each POC is heavily logged to trace what works and what fails.
 */

#include "MenuPocSubsystem.hpp"
#include "Logging.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"
#include "VRMenuSubsystem.hpp"
#include "Cheats.hpp"
#include "ArenaSubsystem.hpp"

#include "..\CppSDK\SDK.hpp"
#include <sstream>
#include <vector>
#include <algorithm>

namespace Mod
{
    namespace MenuPoc
    {
        namespace
        {
            // Helper to generate full menu text with current cheat states
            std::wstring GenerateMenuText(int selectedIndex = 0)
            {
                std::wostringstream menu;
                menu << L"=== MOD MENU ===\n";
                
                Cheats* cheats = GetCheats();
                
                // Menu items with current state
                struct MenuItem {
                    const wchar_t* name;
                    bool isOn;
                };
                
                std::vector<MenuItem> items;
                if (cheats)
                {
                    items.push_back({L"God Mode", cheats->IsGodModeActive()});
                    items.push_back({L"Unlimited Ammo", cheats->IsUnlimitedAmmoActive()});
                    items.push_back({L"Durability Bypass", cheats->IsDurabilityBypassActive()});
                    items.push_back({L"No Hunger", cheats->IsHungerDisabledActive()});
                    items.push_back({L"No Fatigue", cheats->IsFatigueDisabledActive()});
                    items.push_back({L"Bullet Time", cheats->IsBulletTimeActive()});
                    items.push_back({L"No Clip", cheats->IsNoClipActive()});
                    items.push_back({L"Auto Mag", cheats->IsAutoMagActive()});
                    items.push_back({L"Anomalies Disabled", cheats->IsAnomaliesDisabledActive()});
                }
                else
                {
                    items.push_back({L"God Mode", false});
                    items.push_back({L"Unlimited Ammo", false});
                    items.push_back({L"No Hunger", false});
                }
                
                // Add arena item
                auto* arena = Arena::ArenaSubsystem::Get();
                items.push_back({L"Arena", arena && arena->IsActive()});
                
                // Build menu string
                for (int i = 0; i < static_cast<int>(items.size()); i++)
                {
                    if (i == selectedIndex)
                        menu << L">> ";
                    else
                        menu << L"   ";
                    
                    menu << items[i].name;
                    menu << L" [" << (items[i].isOn ? L"ON" : L"OFF") << L"]\n";
                }
                
                menu << L"\n[Y]=close [stick]=nav";
                return menu.str();
            }

            // Helper to check if an object is a CDO (Class Default Object)
            bool IsCDO(SDK::UObject* obj)
            {
                if (!obj) return true;
                std::string name = obj->GetName();
                // CDOs have "Default__" prefix in their name
                return name.find("Default__") != std::string::npos;
            }

            // Helper to get world subsystem by manual scan (filters out CDOs)
            template<typename T>
            T* GetWorldSubsystem(SDK::UWorld* world)
            {
                if (!world) return nullptr;
                if (!SDK::UObject::GObjects) return nullptr;
                
                // Try to find subsystem in GObjects
                SDK::UClass* targetClass = T::StaticClass();
                if (!targetClass) return nullptr;

                T* foundSubsystem = nullptr;
                for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
                {
                    SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                    if (!obj) continue;
                    if (obj->IsA(targetClass))
                    {
                        // Skip CDOs (Default__ objects)
                        if (IsCDO(obj))
                        {
                            LOG_INFO("[MenuPoc] Skipping CDO: " << obj->GetFullName());
                            continue;
                        }
                        // Found a real instance
                        T* subsystem = static_cast<T*>(obj);
                        LOG_INFO("[MenuPoc] Found subsystem instance: " << obj->GetFullName());
                        foundSubsystem = subsystem;
                        break; // Use first non-CDO instance
                    }
                }
                return foundSubsystem;
            }

            // Helper to find actor of class in the world
            template<typename T>
            T* FindActorOfClass(SDK::UWorld* world)
            {
                if (!world) return nullptr;
                
                SDK::UClass* targetClass = T::StaticClass();
                if (!targetClass) return nullptr;

                SDK::AActor* found = SDK::UGameplayStatics::GetActorOfClass(world, targetClass);
                if (found)
                {
                    return static_cast<T*>(found);
                }
                return nullptr;
            }

            // Helper to find FPS_VRCharacter_C (the actual player character class used in VR)
            SDK::AFPS_VRCharacter_C* FindFPSVRCharacter(SDK::UWorld* world)
            {
                if (!world) return nullptr;
                
                SDK::UClass* vrCharClass = SDK::AFPS_VRCharacter_C::StaticClass();
                if (!vrCharClass)
                {
                    LOG_WARN("[MenuPoc] FindFPSVRCharacter: StaticClass returned null");
                    return nullptr;
                }

                SDK::AActor* found = SDK::UGameplayStatics::GetActorOfClass(world, vrCharClass);
                if (found)
                {
                    LOG_INFO("[MenuPoc] Found actor of class AFPS_VRCharacter_C: " << found->GetFullName());
                }
                if (found && !found->IsA(vrCharClass))
                {
                    LOG_WARN("[MenuPoc] Found actor is not of class AFPS_VRCharacter_C, it's: " << found->GetFullName());
                }
                if (found && found->IsA(vrCharClass))
                {
                    return static_cast<SDK::AFPS_VRCharacter_C*>(found);
                }
                LOG_WARN("[MenuPoc] FindFPSVRCharacter: No actor of class AFPS_VRCharacter_C found");
                return nullptr;
            }

            // Helper to spawn actor from class
            SDK::AActor* SpawnActorFromClass(SDK::UWorld* world, SDK::UClass* actorClass, const SDK::FTransform& transform)
            {
                if (!world || !actorClass)
                {
                    LOG_WARN("[MenuPoc] SpawnActorFromClass: null world or class");
                    return nullptr;
                }

                SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                    world, actorClass, transform,
                    SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                    nullptr, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

                if (!actor)
                {
                    LOG_WARN("[MenuPoc] BeginDeferredActorSpawnFromClass returned null");
                    return nullptr;
                }

                LOG_INFO("[MenuPoc] BeginDeferredActorSpawnFromClass succeeded, calling FinishSpawningActor");
                SDK::UGameplayStatics::FinishSpawningActor(actor, transform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);

                LOG_INFO("[MenuPoc] Actor spawned: " << actor->GetFullName());
                return actor;
            }

            // Get spawn transform in front of player
            SDK::FTransform GetTransformInFrontOfPlayer(float distance = 100.0f)
            {
                SDK::FTransform result;
                result.Scale3D = SDK::FVector{1.0, 1.0, 1.0};
                result.Rotation = SDK::FQuat{0, 0, 0, 1};
                result.Translation = SDK::FVector{0, 0, 200}; // Default fallback

                SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
                if (player)
                {
                    SDK::FVector playerLoc = player->K2_GetActorLocation();
                    SDK::FRotator playerRot = player->K2_GetActorRotation();

                    // Get forward vector and offset position
                    SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(playerRot);
                    result.Translation = SDK::FVector{
                        playerLoc.X + forward.X * distance,
                        playerLoc.Y + forward.Y * distance,
                        playerLoc.Z + 50.0 // Slightly above eye level
                    };
                    result.Rotation = SDK::UKismetMathLibrary::Conv_RotatorToQuaternion(playerRot);

                    LOG_INFO("[MenuPoc] Player location: (" << playerLoc.X << ", " << playerLoc.Y << ", " << playerLoc.Z << ")");
                    LOG_INFO("[MenuPoc] Spawn location: (" << result.Translation.X << ", " << result.Translation.Y << ", " << result.Translation.Z << ")");
                }
                else
                {
                    LOG_WARN("[MenuPoc] GetTransformInFrontOfPlayer: player not found, using default");
                }

                return result;
            }

            // Get the left hand location
            SDK::FVector GetLeftHandLocation()
            {
                SDK::UWorld* world = GameContext::GetWorld();
                SDK::AFPS_VRCharacter_C* vrChar = FindFPSVRCharacter(world);
                if (vrChar)
                {
                    return vrChar->GetLeftHandLocation();
                }
                return SDK::FVector{0, 0, 100};
            }
        }

        //-------------------------------------------------------------------------
        // POC 1: Use RadiusIngameMenuSubsystem::SwitchIngameMenu(true)
        //-------------------------------------------------------------------------
        std::string Poc1_SwitchIngameMenu()
        {
            LOG_INFO("[MenuPoc] === POC 1: SwitchIngameMenu ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC1 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] World: " << world->GetFullName());

            // Find RadiusIngameMenuSubsystem
            SDK::URadiusIngameMenuSubsystem* menuSubsystem = GetWorldSubsystem<SDK::URadiusIngameMenuSubsystem>(world);
            if (!menuSubsystem)
            {
                result << "POC1 FAILED: RadiusIngameMenuSubsystem not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] Found RadiusIngameMenuSubsystem: " << menuSubsystem->GetFullName());

            // Check current state
            bool wasInMenu = menuSubsystem->IsInIngameMenu();
            LOG_INFO("[MenuPoc] IsInIngameMenu before: " << (wasInMenu ? "true" : "false"));

            // Toggle the menu
            LOG_INFO("[MenuPoc] Calling SwitchIngameMenu(true)...");
            menuSubsystem->SwitchIngameMenu(true);

            bool isInMenu = menuSubsystem->IsInIngameMenu();
            LOG_INFO("[MenuPoc] IsInIngameMenu after: " << (isInMenu ? "true" : "false"));

            // Also check IngameMenu actor
            if (menuSubsystem->IngameMenu)
            {
                LOG_INFO("[MenuPoc] IngameMenu actor: " << menuSubsystem->IngameMenu->GetFullName());
            }
            else
            {
                LOG_INFO("[MenuPoc] IngameMenu actor is null");
            }

            result << "POC1 OK: SwitchIngameMenu called, wasInMenu=" << wasInMenu << ", isInMenu=" << isInMenu;
            Mod::ModFeedback::ShowMessage(L"POC1: SwitchIngameMenu(true) called", 3.0f, SDK::FLinearColor{0.2f, 1.0f, 0.2f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // Helper: Recursively explore widget tree and find TextBlocks
        //-------------------------------------------------------------------------
        void ExploreWidgetRecursive(SDK::UWidget* widget, int depth, std::vector<SDK::UTextBlock*>& foundTextBlocks)
        {
            if (!widget) return;
            
            std::string indent(depth * 2, ' ');
            std::string className = widget->Class ? widget->Class->GetName() : "???";
            std::string widgetName = widget->GetName();
            
            LOG_INFO("[MenuPoc] " << indent << "Widget: " << widgetName << " [" << className << "]");
            
            // Check if this is a TextBlock
            if (widget->IsA(SDK::UTextBlock::StaticClass()))
            {
                SDK::UTextBlock* textBlock = static_cast<SDK::UTextBlock*>(widget);
                foundTextBlocks.push_back(textBlock);
                
                // Log current text
                SDK::FText currentText = textBlock->GetText();
                // FText to string is tricky, just note we found one
                LOG_INFO("[MenuPoc] " << indent << "  -> Found TextBlock! Adding to list.");
            }
            
            // Check if this is a PanelWidget with children
            // We need to check if it's actually a panel by checking class hierarchy
            // For safety, try to call GetChildrenCount and catch if it fails
            try
            {
                // Check if the class inherits from PanelWidget
                std::string fullClassName = widget->Class ? widget->Class->GetFullName() : "";
                bool isPanelLike = (className == "VerticalBox" || className == "HorizontalBox" ||
                                   className == "CanvasPanel" || className == "Overlay" ||
                                   className == "Border" || className == "ScaleBox" ||
                                   className == "SizeBox" || className == "GridPanel" ||
                                   className.find("Panel") != std::string::npos ||
                                   className.find("Box") != std::string::npos);
                
                if (isPanelLike && widget->IsA(SDK::UPanelWidget::StaticClass()))
                {
                    SDK::UPanelWidget* panel = static_cast<SDK::UPanelWidget*>(widget);
                    int childCount = panel->GetChildrenCount();
                    LOG_INFO("[MenuPoc] " << indent << "  (Panel with " << childCount << " children)");
                    
                    for (int i = 0; i < childCount; i++)
                    {
                        SDK::UWidget* child = panel->GetChildAt(i);
                        ExploreWidgetRecursive(child, depth + 1, foundTextBlocks);
                    }
                }
            }
            catch (...)
            {
                // Not a panel widget, that's fine
            }
        }

        //-------------------------------------------------------------------------
        // POC 2: W_GripDebug_L - Explore structure and set text on TextBlocks
        //-------------------------------------------------------------------------
        std::string Poc2_AttachWidgetToLeftHand()
        {
            LOG_INFO("[MenuPoc] === POC 2: W_GripDebug_L Widget Exploration ===");
            std::ostringstream result;

            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
            if (!player)
            {
                result << "POC2 FAILED: Player character not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] Player: " << player->GetFullName());
            
            SDK::UWidgetComponent* debugWidgetL = player->W_GripDebug_L;
            if (!debugWidgetL)
            {
                result << "POC2 FAILED: W_GripDebug_L is null";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] W_GripDebug_L: " << debugWidgetL->GetFullName());
            
            // Log current configuration (WITHOUT changing size)
            SDK::FVector2D drawSize = debugWidgetL->GetDrawSize();
            LOG_INFO("[MenuPoc]   Original DrawSize: (" << drawSize.X << ", " << drawSize.Y << ")");
            LOG_INFO("[MenuPoc]   IsVisible: " << (debugWidgetL->IsVisible() ? "true" : "false"));
            LOG_INFO("[MenuPoc]   GetTwoSided: " << (debugWidgetL->GetTwoSided() ? "true" : "false"));

            // Log widget class
            if (debugWidgetL->WidgetClass)
            {
                LOG_INFO("[MenuPoc]   WidgetClass: " << debugWidgetL->WidgetClass->GetFullName());
            }
            else
            {
                LOG_INFO("[MenuPoc]   WidgetClass: null");
            }


            
            //SDK::FVector2D newDrawSize{500.0, 500.0};
            // portrait shape
            SDK::FVector2D newDrawSize{600.0, 1600.0};
            debugWidgetL->SetDrawSize(newDrawSize);

            debugWidgetL->SetVisibility(true, true);
            debugWidgetL->SetHiddenInGame(false, true);
            LOG_INFO("[MenuPoc]   Made visible");

            // Get the actual widget instance
            SDK::UUserWidget* userWidget = debugWidgetL->GetWidget();
            std::vector<SDK::UTextBlock*> foundTextBlocks;
            
            if (userWidget)
            {
                LOG_INFO("[MenuPoc]   UserWidget: " << userWidget->GetFullName());
                LOG_INFO("[MenuPoc]   UserWidget Class: " << (userWidget->Class ? userWidget->Class->GetFullName() : "null"));
                
                // Explore the WidgetTree
                if (userWidget->WidgetTree)
                {
                    LOG_INFO("[MenuPoc]   WidgetTree: " << userWidget->WidgetTree->GetFullName());
                    
                    SDK::UWidget* rootWidget = userWidget->WidgetTree->RootWidget;
                    if (rootWidget)
                    {
                        LOG_INFO("[MenuPoc]   Exploring widget tree from root...");
                        ExploreWidgetRecursive(rootWidget, 2, foundTextBlocks);
                    }
                    else
                    {
                        LOG_INFO("[MenuPoc]   RootWidget is null");
                    }
                }
                else
                {
                    LOG_INFO("[MenuPoc]   WidgetTree is null");
                }
            }
            else
            {
                LOG_INFO("[MenuPoc]   UserWidget is null - widget component has no widget instance");
            }

            // If we found TextBlocks, try to set text on the first one
            if (!foundTextBlocks.empty())
            {
                LOG_INFO("[MenuPoc] Found " << foundTextBlocks.size() << " TextBlock(s). Setting text on first one...");
                
                SDK::UTextBlock* textBlock = foundTextBlocks[0];
                
                // Generate menu text with current cheat states
                std::wstring menuText = GenerateMenuText(0);
                
                // Convert to FText and set
                SDK::FString fstr(menuText.c_str());
                SDK::FText ftext = SDK::UKismetTextLibrary::Conv_StringToText(fstr);
                textBlock->SetText(ftext);
                
                // Set text color to green
                SDK::FSlateColor textColor;
                textColor.SpecifiedColor = SDK::FLinearColor{0.2f, 1.0f, 0.3f, 1.0f};
                textBlock->SetColorAndOpacity(textColor);
                
                LOG_INFO("[MenuPoc]   Set menu text on TextBlock!");
                
                debugWidgetL->RequestRedraw();
                
                result << "POC2 OK: Found " << foundTextBlocks.size() << " TextBlock(s), set menu text on first";
                Mod::ModFeedback::ShowMessage(L"POC2: Menu text set!", 3.0f, SDK::FLinearColor{0.2f, 1.0f, 0.8f, 1.0f});
            }
            else
            {
                LOG_INFO("[MenuPoc] No TextBlocks found in widget tree");
                
                // Still make the panel visible and scan GObjects for TextBlocks that might be associated
                LOG_INFO("[MenuPoc] Scanning GObjects for TextBlock instances near this widget...");

                if (!SDK::UObject::GObjects)
                {
                    LOG_WARN("[MenuPoc] GObjects unavailable for TextBlock scan");
                    result << "POC2 INFO: GObjects unavailable for fallback TextBlock scan";
                    return result.str();
                }
                
                int textBlockCount = 0;
                int totalObjects = SDK::UObject::GObjects->Num();
                int scanLimit = totalObjects < 100000 ? totalObjects : 100000;
                
                for (int idx = 0; idx < scanLimit && textBlockCount < 20; idx++)
                {
                    SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(idx);
                    if (!obj || !obj->Class) continue;
                    
                    std::string className = obj->Class->GetName();
                    if (className == "TextBlock")
                    {
                        std::string fullName = obj->GetFullName();
                        // Log TextBlocks that might be related to debug/grip widgets
                        if (fullName.find("Debug") != std::string::npos ||
                            fullName.find("Grip") != std::string::npos ||
                            fullName.find("GripDebug") != std::string::npos)
                        {
                            LOG_INFO("[MenuPoc]   Related TextBlock: " << fullName);
                            textBlockCount++;
                        }
                    }
                }
                
                if (textBlockCount == 0)
                {
                    LOG_INFO("[MenuPoc]   No related TextBlocks found in GObjects scan");
                }

                // FALLBACK: Try to find the widget's assigned WidgetClass and explore what it is
                SDK::UClass* widgetClass = debugWidgetL->WidgetClass;
                if (widgetClass)
                {
                    LOG_INFO("[MenuPoc] FALLBACK: WidgetClass is set. Exploring class...");
                    LOG_INFO("[MenuPoc]   WidgetClass: " << widgetClass->GetFullName());
                    
                    // The WidgetClass is set but widget wasn't instantiated yet
                    // Let's try to find an instance or create one
                    SDK::UWorld* world = GameContext::GetWorld();
                    SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
                    
                    if (world && pc)
                    {
                        LOG_INFO("[MenuPoc]   Attempting to create widget from WidgetClass...");
                        
                        SDK::UUserWidget* newWidget = SDK::UWidgetBlueprintLibrary::Create(world, widgetClass, pc);
                        if (newWidget)
                        {
                            LOG_INFO("[MenuPoc]   Created widget: " << newWidget->GetFullName());
                            
                            // Set the widget on the component
                            debugWidgetL->SetWidget(newWidget);
                            LOG_INFO("[MenuPoc]   Called SetWidget() on component");
                            
                            // Now explore the new widget for TextBlocks
                            std::vector<SDK::UTextBlock*> newTextBlocks;
                            if (newWidget->WidgetTree && newWidget->WidgetTree->RootWidget)
                            {
                                ExploreWidgetRecursive(newWidget->WidgetTree->RootWidget, 2, newTextBlocks);
                            }
                            
                            if (!newTextBlocks.empty())
                            {
                                LOG_INFO("[MenuPoc]   Found " << newTextBlocks.size() << " TextBlock(s) in new widget!");
                                
                                // Generate menu text with current cheat states
                                std::wstring menuText = GenerateMenuText(0);
                                SDK::FString fstr(menuText.c_str());
                                SDK::FText ftext = SDK::UKismetTextLibrary::Conv_StringToText(fstr);
                                newTextBlocks[0]->SetText(ftext);
                                
                                debugWidgetL->RequestRedraw();
                                result << "POC2 FALLBACK OK: Created widget and set text";
                                Mod::ModFeedback::ShowMessage(L"POC2: Created widget with text!", 3.0f, SDK::FLinearColor{0.2f, 1.0f, 0.3f, 1.0f});
                                return result.str();
                            }
                            else
                            {
                                LOG_INFO("[MenuPoc]   New widget has no TextBlocks either");
                            }
                        }
                        else
                        {
                            LOG_WARN("[MenuPoc]   UWidgetBlueprintLibrary::Create returned null");
                        }
                    }
                }
                else
                {
                    LOG_INFO("[MenuPoc]   WidgetClass is also null - component has no widget configuration");
                }

                result << "POC2: Widget visible but no TextBlocks found. Check logs.";
                Mod::ModFeedback::ShowMessage(L"POC2: No TextBlocks found - see log", 3.0f, SDK::FLinearColor{1.0f, 0.5f, 0.2f, 1.0f});
            }

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 3: Scan world for RadiusWidgetComponents and try to show one
        //-------------------------------------------------------------------------
        std::string Poc3_ShowCheatPanel()
        {
            LOG_INFO("[MenuPoc] === POC 3: Scan for RadiusWidgetComponents ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC3 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Scan GObjects for RadiusWidgetComponent instances
            SDK::UClass* rwcClass = SDK::URadiusWidgetComponent::StaticClass();
            if (!rwcClass)
            {
                result << "POC3 FAILED: RadiusWidgetComponent class not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            int foundCount = 0;
            SDK::URadiusWidgetComponent* targetWidget = nullptr;

            if (!SDK::UObject::GObjects)
            {
                result << "POC3 FAILED: GObjects unavailable";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            
            for (int i = 0; i < SDK::UObject::GObjects->Num() && foundCount < 20; ++i)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj) continue;
                if (!obj->IsA(rwcClass)) continue;
                
                // Skip CDOs
                if (IsCDO(obj)) continue;
                
                SDK::URadiusWidgetComponent* rwc = static_cast<SDK::URadiusWidgetComponent*>(obj);
                std::string fullName = rwc->GetFullName();
                
                // Log all found components
                LOG_INFO("[MenuPoc] Found RadiusWidgetComponent [" << foundCount << "]: " << fullName);
                LOG_INFO("[MenuPoc]   IsVisible: " << (rwc->IsVisible() ? "true" : "false"));
                
                // Look for something interesting - CheatPanel, ItemInfo, PlayerInfo
                if (fullName.find("CheatPanel") != std::string::npos ||
                    fullName.find("ItemInfo") != std::string::npos ||
                    fullName.find("PlayerInfo") != std::string::npos)
                {
                    LOG_INFO("[MenuPoc]   *** This looks interesting! ***");
                    if (!targetWidget)
                    {
                        targetWidget = rwc;
                    }
                }
                
                foundCount++;
            }
            
            LOG_INFO("[MenuPoc] Found " << foundCount << " RadiusWidgetComponent instances");

            if (targetWidget)
            {
                LOG_INFO("[MenuPoc] Attempting to show: " << targetWidget->GetFullName());
                bool wasVisible = targetWidget->IsVisible();
                
                targetWidget->SetVisibility(true, true);
                targetWidget->SetHiddenInGame(false, true);
                
                bool isVisible = targetWidget->IsVisible();
                LOG_INFO("[MenuPoc] Visibility changed: " << wasVisible << " -> " << isVisible);
                
                result << "POC3 OK: Set visibility on " << targetWidget->GetName() << ", found " << foundCount << " total";
            }
            else
            {
                result << "POC3 INFO: Found " << foundCount << " RadiusWidgetComponents, none selected";
            }
            
            Mod::ModFeedback::ShowMessage(L"POC3: Scanned RadiusWidgetComponents", 3.0f, SDK::FLinearColor{1.0f, 1.0f, 0.2f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 4: Spawn a BP_IngameMenu_C actor
        //-------------------------------------------------------------------------
        std::string Poc4_SpawnIngameMenuActor()
        {
            LOG_INFO("[MenuPoc] === POC 4: Spawn BP_IngameMenu Actor ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC4 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Get the BP_IngameMenu_C class
            SDK::UClass* menuClass = SDK::ABP_IngameMenu_C::StaticClass();
            if (!menuClass)
            {
                result << "POC4 FAILED: BP_IngameMenu_C class not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] BP_IngameMenu_C class: " << menuClass->GetFullName());

            // Get spawn transform
            SDK::FTransform spawnTransform = GetTransformInFrontOfPlayer(150.0f);

            // Spawn the actor
            SDK::AActor* menuActor = SpawnActorFromClass(world, menuClass, spawnTransform);
            if (!menuActor)
            {
                result << "POC4 FAILED: Failed to spawn actor";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] Spawned BP_IngameMenu_C: " << menuActor->GetFullName());

            if (!menuActor->IsA(SDK::ABP_IngameMenu_C::StaticClass()))
            {
                result << "POC4 FAILED: Spawned actor is not BP_IngameMenu_C";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Cast to specific type and check properties
            SDK::ABP_IngameMenu_C* ingameMenu = static_cast<SDK::ABP_IngameMenu_C*>(menuActor);
            if (ingameMenu->VoiceChatPanel)
            {
                LOG_INFO("[MenuPoc] VoiceChatPanel: " << ingameMenu->VoiceChatPanel->GetFullName());
            }
            if (ingameMenu->W_CheatPanel)
            {
                LOG_INFO("[MenuPoc] W_CheatPanel: " << ingameMenu->W_CheatPanel->GetFullName());
            }

            result << "POC4 OK: Spawned BP_IngameMenu_C actor at (" 
                   << spawnTransform.Translation.X << ", " 
                   << spawnTransform.Translation.Y << ", " 
                   << spawnTransform.Translation.Z << ")";
            Mod::ModFeedback::ShowMessage(L"POC4: BP_IngameMenu spawned", 3.0f, SDK::FLinearColor{0.8f, 0.2f, 1.0f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 5: Find existing InfoPanel and call ShowPlayerInfo
        //-------------------------------------------------------------------------
        std::string Poc5_UseInfoPanel()
        {
            LOG_INFO("[MenuPoc] === POC 5: Use InfoPanel ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC5 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Find existing InfoPanel actor
            SDK::AInfoPanel* infoPanel = FindActorOfClass<SDK::AInfoPanel>(world);
            if (!infoPanel)
            {
                // Try blueprint version
                SDK::UClass* bpInfoPanelClass = SDK::ABP_InfoPanel_C::StaticClass();
                if (bpInfoPanelClass)
                {
                    SDK::AActor* bpInfoPanel = SDK::UGameplayStatics::GetActorOfClass(world, bpInfoPanelClass);
                    if (bpInfoPanel && bpInfoPanel->IsA(SDK::AInfoPanel::StaticClass()))
                    {
                        infoPanel = static_cast<SDK::AInfoPanel*>(bpInfoPanel);
                    }
                }
            }

            if (!infoPanel)
            {
                result << "POC5 FAILED: InfoPanel not found in world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] InfoPanel: " << infoPanel->GetFullName());

            // Check current state
            SDK::EInfoPanelState state = infoPanel->GetInfoPanelState();
            LOG_INFO("[MenuPoc] InfoPanel state: " << static_cast<int>(state));

            // Check components
            if (infoPanel->ItemInfoWidgetComponent)
            {
                LOG_INFO("[MenuPoc] ItemInfoWidgetComponent: " << infoPanel->ItemInfoWidgetComponent->GetFullName());
                LOG_INFO("[MenuPoc] ItemInfoWidgetComponent visible: " << (infoPanel->ItemInfoWidgetComponent->IsVisible() ? "true" : "false"));
            }
            if (infoPanel->PlayerInfoWidgetComponent)
            {
                LOG_INFO("[MenuPoc] PlayerInfoWidgetComponent: " << infoPanel->PlayerInfoWidgetComponent->GetFullName());
                LOG_INFO("[MenuPoc] PlayerInfoWidgetComponent visible: " << (infoPanel->PlayerInfoWidgetComponent->IsVisible() ? "true" : "false"));
            }
            if (infoPanel->ItemInfo)
            {
                LOG_INFO("[MenuPoc] ItemInfo widget: " << infoPanel->ItemInfo->GetFullName());
            }
            if (infoPanel->PlayerInfo)
            {
                LOG_INFO("[MenuPoc] PlayerInfo widget: " << infoPanel->PlayerInfo->GetFullName());
            }

            // Move the menu up in front of the player
            SDK::FTransform newTransform = GetTransformInFrontOfPlayer(120.0f);

            // If we found an InfoPanel actor, move it to the new transform so the menu appears
            if (infoPanel)
            {
                LOG_INFO("[MenuPoc] Moving InfoPanel to (" << newTransform.Translation.X << ", " << newTransform.Translation.Y << ", " << newTransform.Translation.Z << ")");
                infoPanel->K2_SetActorTransform(newTransform, false, nullptr, true);
                // Adjust widget component positions relative to the panel so they sit nicely in front of the player
                if (infoPanel->ItemInfoWidgetComponent)
                {
                    SDK::FVector itemOffset{0.0f, 0.0f, 50.0f};
                    infoPanel->ItemInfoWidgetComponent->K2_SetRelativeLocation(itemOffset, false, nullptr, true);
                }
                if (infoPanel->PlayerInfoWidgetComponent)
                {
                    SDK::FVector playerOffset{0.0f, 0.0f, 20.0f};
                    infoPanel->PlayerInfoWidgetComponent->K2_SetRelativeLocation(playerOffset, false, nullptr, true);
                }
            }

            LOG_INFO("[MenuPoc] Setting ItemInfoWidgetComponent visibility to true...");
            if (infoPanel->ItemInfoWidgetComponent)            {
                infoPanel->ItemInfoWidgetComponent->SetVisibility(true, true);
                LOG_INFO("[MenuPoc] ItemInfoWidgetComponent IsVisible: " << (infoPanel->ItemInfoWidgetComponent->IsVisible() ? "true" : "false"));
            }

            LOG_INFO("[MenuPoc] Setting PlayerInfoWidgetComponent visibility to true...");
            if (infoPanel->PlayerInfoWidgetComponent)
            {
                infoPanel->PlayerInfoWidgetComponent->SetVisibility(true, true);
                LOG_INFO("[MenuPoc] PlayerInfoWidgetComponent IsVisible: " << (infoPanel->PlayerInfoWidgetComponent->IsVisible() ? "true" : "false"));
            }

            // Try to show player info
            LOG_INFO("[MenuPoc] Calling ShowPlayerInfo()...");
            infoPanel->ShowPlayerInfo();

            SDK::EInfoPanelState newState = infoPanel->GetInfoPanelState();
            LOG_INFO("[MenuPoc] InfoPanel state after ShowPlayerInfo: " << static_cast<int>(newState));

            result << "POC5 OK: Called ShowPlayerInfo(), state: " << static_cast<int>(state) << " -> " << static_cast<int>(newState);
            //Mod::ModFeedback::ShowMessage(L"POC5: ShowPlayerInfo called", 3.0f, SDK::FLinearColor{0.5f, 0.5f, 1.0f, 1.0f});



            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 6: Spawn BPA_FaceFollowingMenu actor
        //-------------------------------------------------------------------------
        std::string Poc6_SpawnFaceFollowingMenu()
        {
            LOG_INFO("[MenuPoc] === POC 6: Spawn FaceFollowingMenu ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC6 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Get the BPA_FaceFollowingMenu_C class
            SDK::UClass* menuClass = SDK::ABPA_FaceFollowingMenu_C::StaticClass();
            if (!menuClass)
            {
                result << "POC6 FAILED: BPA_FaceFollowingMenu_C class not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] BPA_FaceFollowingMenu_C class: " << menuClass->GetFullName());

            // Get spawn transform
            SDK::FTransform spawnTransform = GetTransformInFrontOfPlayer(120.0f);

            // Spawn the actor
            SDK::AActor* menuActor = SpawnActorFromClass(world, menuClass, spawnTransform);
            if (!menuActor)
            {
                result << "POC6 FAILED: Failed to spawn actor";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] Spawned BPA_FaceFollowingMenu_C: " << menuActor->GetFullName());

            if (!menuActor->IsA(SDK::ABPA_FaceFollowingMenu_C::StaticClass()))
            {
                result << "POC6 FAILED: Spawned actor is not BPA_FaceFollowingMenu_C";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Cast and examine
            SDK::ABPA_FaceFollowingMenu_C* faceMenu = static_cast<SDK::ABPA_FaceFollowingMenu_C*>(menuActor);
            
            if (faceMenu->BP_MainMenu_Mih3D)
            {
                LOG_INFO("[MenuPoc] BP_MainMenu_Mih3D: " << faceMenu->BP_MainMenu_Mih3D->GetFullName());
            }
            if (faceMenu->RotationPivot)
            {
                LOG_INFO("[MenuPoc] RotationPivot: " << faceMenu->RotationPivot->GetFullName());
            }
            if (faceMenu->MenuStaticMesh)
            {
                LOG_INFO("[MenuPoc] MenuStaticMesh: " << faceMenu->MenuStaticMesh->GetFullName());
            }

            result << "POC6 OK: Spawned BPA_FaceFollowingMenu_C at (" 
                   << spawnTransform.Translation.X << ", " 
                   << spawnTransform.Translation.Y << ", " 
                   << spawnTransform.Translation.Z << ")";
            Mod::ModFeedback::ShowMessage(L"POC6: FaceFollowingMenu spawned", 3.0f, SDK::FLinearColor{1.0f, 0.5f, 0.2f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 7: Create a UserWidget using WidgetBlueprintLibrary::Create
        //-------------------------------------------------------------------------
        std::string Poc7_CreateUserWidget()
        {
            LOG_INFO("[MenuPoc] === POC 7: Create UserWidget via WidgetBlueprintLibrary ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC7 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Get player controller  
            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
            if (!player)
            {
                result << "POC7 FAILED: Player not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            
            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!pc)
            {
                result << "POC7 FAILED: PlayerController not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] PlayerController: " << pc->GetFullName());

            // Try to find WBP_CheatPanel class to create
            SDK::UClass* cheatPanelWidgetClass = nullptr;

            if (!SDK::UObject::GObjects)
            {
                result << "POC7 FAILED: GObjects unavailable";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            
            // Scan for the widget class
            for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
            {
                SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                if (!obj) continue;
                
                std::string name = obj->GetName();
                if (name.find("WBP_CheatPanel") != std::string::npos && name.find("Default__") == std::string::npos)
                {
                    if (obj->IsA(SDK::UClass::StaticClass()))
                    {
                        cheatPanelWidgetClass = static_cast<SDK::UClass*>(obj);
                        LOG_INFO("[MenuPoc] Found widget class: " << obj->GetFullName());
                        break;
                    }
                }
            }

            if (!cheatPanelWidgetClass)
            {
                LOG_WARN("[MenuPoc] WBP_CheatPanel class not found in GObjects");
                
                // Try an alternate approach - find any user widget class
                LOG_INFO("[MenuPoc] Looking for any UserWidget subclass...");
                SDK::UClass* userWidgetClass = SDK::UUserWidget::StaticClass();
                int foundWidgetClasses = 0;
                
                for (int i = 0; i < SDK::UObject::GObjects->Num() && foundWidgetClasses < 10; ++i)
                {
                    SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                    if (!obj) continue;
                    if (!obj->IsA(SDK::UClass::StaticClass())) continue;
                    
                    SDK::UClass* cls = static_cast<SDK::UClass*>(obj);
                    std::string fullName = cls->GetFullName();
                    
                    // Look for WBP_ prefixed widget classes
                    if (fullName.find("WBP_") != std::string::npos && fullName.find("WidgetBlueprintGeneratedClass") != std::string::npos)
                    {
                        LOG_INFO("[MenuPoc] Found widget BP class: " << fullName);
                        foundWidgetClasses++;
                        
                        if (!cheatPanelWidgetClass && fullName.find("Cheat") != std::string::npos)
                        {
                            cheatPanelWidgetClass = cls;
                        }
                    }
                }
            }

            if (cheatPanelWidgetClass)
            {
                LOG_INFO("[MenuPoc] Using widget class: " << cheatPanelWidgetClass->GetFullName());
                
                // Create the widget
                SDK::UUserWidget* widget = SDK::UWidgetBlueprintLibrary::Create(world, cheatPanelWidgetClass, pc);
                
                if (widget)
                {
                    LOG_INFO("[MenuPoc] Created widget: " << widget->GetFullName());
                    
                    // Try to add it to viewport (for 2D UI test)
                    widget->AddToViewport(0);
                    LOG_INFO("[MenuPoc] Added widget to viewport");
                    
                    result << "POC7 OK: Created and added widget to viewport: " << widget->GetName();
                }
                else
                {
                    result << "POC7 FAILED: WidgetBlueprintLibrary::Create returned null";
                    LOG_WARN("[MenuPoc] " << result.str());
                }
            }
            else
            {
                result << "POC7 INFO: No suitable widget class found";
            }
            
            Mod::ModFeedback::ShowMessage(L"POC7: Create widget attempted", 3.0f, SDK::FLinearColor{0.2f, 0.8f, 0.8f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 8: Spawn BP_Confirmation with text and two-sided
        //-------------------------------------------------------------------------
        std::string Poc8_SpawnConfirmation()
        {
            LOG_INFO("[MenuPoc] === POC 8: Spawn BP_Confirmation with Menu Text ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC8 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Find BP_Confirmation_C class
            SDK::UClass* confirmClass = SDK::ABP_Confirmation_C::StaticClass();
            if (!confirmClass)
            {
                result << "POC8 FAILED: BP_Confirmation_C class not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] BP_Confirmation_C class: " << confirmClass->GetFullName());

            // Get spawn transform
            SDK::FTransform spawnTransform = GetTransformInFrontOfPlayer(60.0f);

            // Spawn the actor
            SDK::AActor* confActor = SpawnActorFromClass(world, confirmClass, spawnTransform);
            if (!confActor)
            {
                result << "POC8 FAILED: Failed to spawn actor";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] Spawned BP_Confirmation_C: " << confActor->GetFullName());

            if (!confActor->IsA(SDK::ABP_Confirmation_C::StaticClass()))
            {
                result << "POC8 FAILED: Spawned actor is not BP_Confirmation_C";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            
            SDK::ABP_Confirmation_C* confirmation = static_cast<SDK::ABP_Confirmation_C*>(confActor);
            
            // Set text properties with menu text
            SDK::FString titleStr(L"=== MOD MENU ===");
            confirmation->Title = SDK::UKismetTextLibrary::Conv_StringToText(titleStr);
            
            std::wstring menuTextW = GenerateMenuText(0);
            SDK::FString descStr(menuTextW.c_str());
            confirmation->Description = SDK::UKismetTextLibrary::Conv_StringToText(descStr);
            
            SDK::FString yesStr(L"Toggle");
            confirmation->YesText = SDK::UKismetTextLibrary::Conv_StringToText(yesStr);
            
            SDK::FString noStr(L"Close");
            confirmation->NoText = SDK::UKismetTextLibrary::Conv_StringToText(noStr);
            
            LOG_INFO("[MenuPoc] Set Title, Description, YesText, NoText");

            // Configure the widget component
            if (confirmation->Confirmation)
            {
                LOG_INFO("[MenuPoc] Confirmation widget component: " << confirmation->Confirmation->GetFullName());
                
                // Make visible and two-sided
                confirmation->Confirmation->SetVisibility(true, true);
                confirmation->Confirmation->SetHiddenInGame(false, true);
                confirmation->Confirmation->SetTwoSided(true);
                confirmation->Confirmation->RequestRedraw();
                
                // Log draw size
                SDK::FVector2D drawSize = confirmation->Confirmation->GetDrawSize();
                LOG_INFO("[MenuPoc]   Original DrawSize: (" << drawSize.X << ", " << drawSize.Y << ")");
                
                // Explore the widget tree
                SDK::UUserWidget* userWidget = confirmation->Confirmation->GetWidget();
                if (userWidget)
                {
                    LOG_INFO("[MenuPoc]   Widget: " << userWidget->GetFullName());
                    
                    std::vector<SDK::UTextBlock*> textBlocks;
                    if (userWidget->WidgetTree && userWidget->WidgetTree->RootWidget)
                    {
                        ExploreWidgetRecursive(userWidget->WidgetTree->RootWidget, 4, textBlocks);
                        LOG_INFO("[MenuPoc]   Found " << textBlocks.size() << " TextBlock(s)");
                        
                        // Try to set text on TextBlocks
                        for (size_t i = 0; i < textBlocks.size() && i < 4; i++)
                        {
                            SDK::UTextBlock* tb = textBlocks[i];
                            SDK::FText currentText = tb->GetText();
                            std::string currentStr = currentText.ToString();
                            LOG_INFO("[MenuPoc]   TextBlock[" << i << "] current text: \"" << currentStr << "\"");
                            
                            // Map text to our menu content
                            SDK::FText newText;
                            if (currentStr.find("Title") != std::string::npos || i == 0)
                            {
                                newText = confirmation->Title;
                            }
                            else if (currentStr.find("Description") != std::string::npos || i == 1)
                            {
                                newText = confirmation->Description;
                            }
                            else
                            {
                                continue;
                            }
                            
                            tb->SetText(newText);
                            LOG_INFO("[MenuPoc]   Set TextBlock[" << i << "] to menu text");
                        }
                    }
                }
            }

            result << "POC8 OK: Spawned BP_Confirmation with menu text at (" 
                   << spawnTransform.Translation.X << ", " 
                   << spawnTransform.Translation.Y << ", " 
                   << spawnTransform.Translation.Z << ")";
            Mod::ModFeedback::ShowMessage(L"POC8: Confirmation with menu text", 3.0f, SDK::FLinearColor{1.0f, 0.5f, 0.2f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 9: Create WBP_Confirmation widget and attach to left hand
        //-------------------------------------------------------------------------
        std::string Poc9_CreateConfirmWidgetOnHand()
        {
            LOG_INFO("[MenuPoc] === POC 9: WBP_Confirmation on Left Hand ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC9 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = GameContext::GetPlayerCharacter();
            if (!player)
            {
                result << "POC9 FAILED: Player not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            SDK::UWidgetComponent* widgetComp = player->W_GripDebug_L;
            if (!widgetComp)
            {
                result << "POC9 FAILED: W_GripDebug_L is null";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] W_GripDebug_L: " << widgetComp->GetFullName());
            
            // Get player controller
            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!pc)
            {
                result << "POC9 FAILED: No PlayerController";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Get WBP_Confirmation_C class
            SDK::UClass* confirmWidgetClass = SDK::UWBP_Confirmation_C::StaticClass();
            if (!confirmWidgetClass)
            {
                result << "POC9 FAILED: WBP_Confirmation_C class not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] WBP_Confirmation_C class: " << confirmWidgetClass->GetFullName());

            // Create the widget
            SDK::UUserWidget* widget = SDK::UWidgetBlueprintLibrary::Create(world, confirmWidgetClass, pc);
            if (!widget)
            {
                result << "POC9 FAILED: Failed to create widget";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] Created widget: " << widget->GetFullName());

            if (!widget->IsA(SDK::UWBP_Confirmation_C::StaticClass()))
            {
                result << "POC9 FAILED: Created widget is not UWBP_Confirmation_C";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Cast to UWBP_Confirmation_C
            SDK::UWBP_Confirmation_C* confirmWidget = static_cast<SDK::UWBP_Confirmation_C*>(widget);
            
            // Generate menu text
            std::wstring menuTextW = GenerateMenuText(0);
            
            // Create FText for title and description
            SDK::FString titleStr(L"=== MOD MENU ===");
            SDK::FText titleText = SDK::UKismetTextLibrary::Conv_StringToText(titleStr);
            
            SDK::FString descStr(menuTextW.c_str());
            SDK::FText descText = SDK::UKismetTextLibrary::Conv_StringToText(descStr);
            
            SDK::FString yesStr(L"Toggle");
            SDK::FText yesText = SDK::UKismetTextLibrary::Conv_StringToText(yesStr);
            
            SDK::FString noStr(L"Close");
            SDK::FText noText = SDK::UKismetTextLibrary::Conv_StringToText(noStr);

            // Call Setupwidget to configure it
            // Note: Setupwidget blueprint function may not be available, setting directly instead
            LOG_INFO("[MenuPoc] Setting confirmation properties directly...");
            confirmWidget->Title = titleText;
            confirmWidget->Description = descText;
            confirmWidget->Yes_Text = yesText;
            confirmWidget->No_Text = noText;


            // Also try setting directly
            if (confirmWidget->Txt_Confirmation_Title)
            {
                confirmWidget->Txt_Confirmation_Title->SetText(titleText);

                SDK::FSlateColor whiteTitle;
                whiteTitle.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
                confirmWidget->Txt_Confirmation_Title->SetColorAndOpacity(whiteTitle);
                LOG_INFO("[MenuPoc] Set title TextBlock directly");
            }
            
            if (confirmWidget->Txt_TextConfirm)
            {
                confirmWidget->Txt_TextConfirm->SetText(descText);

                SDK::FTextBlockStyle whiteStyle = confirmWidget->Txt_TextConfirm->WidgetStyle;
                whiteStyle.ColorAndOpacity.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
                confirmWidget->Txt_TextConfirm->SetWidgetStyle(whiteStyle);

                LOG_INFO("[MenuPoc] Txt_TextConfirm exists: " << confirmWidget->Txt_TextConfirm->GetFullName());
            }

            // Set the widget on the widget component
            LOG_INFO("[MenuPoc] Calling SetWidget...");
            widgetComp->SetWidget(confirmWidget);
            
            // Make visible
            widgetComp->SetVisibility(true, true);
            widgetComp->SetHiddenInGame(false, true);
            widgetComp->SetTwoSided(true);
            widgetComp->RequestRedraw();
            
            // Log final draw size
            SDK::FVector2D drawSize = widgetComp->GetDrawSize();
            LOG_INFO("[MenuPoc] Final DrawSize: (" << drawSize.X << ", " << drawSize.Y << ")");

            result << "POC9 OK: WBP_Confirmation widget attached to left hand";
            Mod::ModFeedback::ShowMessage(L"POC9: Menu widget on hand!", 3.0f, SDK::FLinearColor{0.2f, 1.0f, 0.2f, 1.0f});

            return result.str();
        }

    } // namespace MenuPoc
} // namespace Mod
