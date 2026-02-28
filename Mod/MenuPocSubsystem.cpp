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

#include "..\CppSDK\SDK.hpp"
#include <sstream>

namespace Mod
{
    namespace MenuPoc
    {
        namespace
        {
            // Helper to get world subsystem by manual scan
            template<typename T>
            T* GetWorldSubsystem(SDK::UWorld* world)
            {
                if (!world) return nullptr;
                
                // Try to find subsystem in GObjects
                SDK::UClass* targetClass = T::StaticClass();
                if (!targetClass) return nullptr;

                for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
                {
                    SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
                    if (!obj) continue;
                    if (obj->IsA(targetClass))
                    {
                        // Verify it's in the same world context
                        T* subsystem = static_cast<T*>(obj);
                        LOG_INFO("[MenuPoc] Found subsystem: " << obj->GetFullName());
                        return subsystem;
                    }
                }
                return nullptr;
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
                if (!vrCharClass) return nullptr;

                SDK::AActor* found = SDK::UGameplayStatics::GetActorOfClass(world, vrCharClass);
                if (found && found->IsA(vrCharClass))
                {
                    return static_cast<SDK::AFPS_VRCharacter_C*>(found);
                }
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
        // POC 2: Create and attach a RadiusWidgetComponent to the player
        //-------------------------------------------------------------------------
        std::string Poc2_AttachWidgetToLeftHand()
        {
            LOG_INFO("[MenuPoc] === POC 2: Attach Widget to Left Hand ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC2 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Find FPS_VRCharacter directly
            SDK::AFPS_VRCharacter_C* vrChar = FindFPSVRCharacter(world);
            if (!vrChar)
            {
                result << "POC2 FAILED: FPS_VRCharacter not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] VRCharacter: " << vrChar->GetFullName());
            
            // Get left hand mesh
            SDK::USkeletalMeshComponent* leftHandMesh = nullptr;
            SDK::USkeletalMeshComponent* rightHandMesh = nullptr;
            vrChar->GetPhysicalHandMeshes(&leftHandMesh, &rightHandMesh);

            if (!leftHandMesh)
            {
                result << "POC2 FAILED: Left hand mesh not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] Left hand mesh: " << leftHandMesh->GetFullName());

            // Try to get the HandContainer_L component
            if (vrChar->HandContainer_L)
            {
                LOG_INFO("[MenuPoc] HandContainer_L: " << vrChar->HandContainer_L->GetFullName());
            }

            // For now, just log what we have - creating a widget component at runtime is complex
            // Let's see if there are existing widget components we can repurpose
            
            SDK::FVector leftHandLoc = vrChar->GetLeftHandLocation();
            LOG_INFO("[MenuPoc] Left hand location: (" << leftHandLoc.X << ", " << leftHandLoc.Y << ", " << leftHandLoc.Z << ")");

            result << "POC2 INFO: Found left hand at (" << leftHandLoc.X << ", " << leftHandLoc.Y << ", " << leftHandLoc.Z << ")";
            Mod::ModFeedback::ShowMessage(L"POC2: Found left hand, see log for details", 3.0f, SDK::FLinearColor{0.2f, 1.0f, 0.8f, 1.0f});

            return result.str();
        }

        //-------------------------------------------------------------------------
        // POC 3: Find and show the CheatPanel on FPS_VRCharacter
        //-------------------------------------------------------------------------
        std::string Poc3_ShowCheatPanel()
        {
            LOG_INFO("[MenuPoc] === POC 3: Show CheatPanel ===");
            std::ostringstream result;

            SDK::UWorld* world = GameContext::GetWorld();
            if (!world)
            {
                result << "POC3 FAILED: No world";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            // Find FPS_VRCharacter directly
            SDK::AFPS_VRCharacter_C* vrChar = FindFPSVRCharacter(world);
            if (!vrChar)
            {
                result << "POC3 FAILED: FPS_VRCharacter not found";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }
            LOG_INFO("[MenuPoc] VRCharacter: " << vrChar->GetFullName());

            // Access CheatPanel
            SDK::URadiusWidgetComponent* cheatPanel = vrChar->CheatPanel;
            if (!cheatPanel)
            {
                result << "POC3 FAILED: CheatPanel is null";
                LOG_WARN("[MenuPoc] " << result.str());
                return result.str();
            }

            LOG_INFO("[MenuPoc] CheatPanel: " << cheatPanel->GetFullName());
            LOG_INFO("[MenuPoc] CheatPanel widget class: " << (cheatPanel->WidgetClass ? cheatPanel->WidgetClass->GetFullName() : "null"));

            // Check current visibility
            bool wasVisible = cheatPanel->IsVisible();
            LOG_INFO("[MenuPoc] CheatPanel IsVisible before: " << (wasVisible ? "true" : "false"));

            // Try to get the widget
            SDK::UUserWidget* widget = cheatPanel->GetWidget();
            if (widget)
            {
                LOG_INFO("[MenuPoc] CheatPanel widget: " << widget->GetFullName());
            }
            else
            {
                LOG_INFO("[MenuPoc] CheatPanel widget is null (not yet created)");
            }

            // Toggle visibility (SetVisibility takes 2 args: visibility, propagateToChildren)
            if (!wasVisible)
            {
                LOG_INFO("[MenuPoc] Calling SetVisibility(true, true)...");
                cheatPanel->SetVisibility(true, true);
                cheatPanel->SetHiddenInGame(false, true);
            }
            else
            {
                LOG_INFO("[MenuPoc] CheatPanel already visible, toggling off then on...");
                cheatPanel->SetVisibility(false, true);
                cheatPanel->SetHiddenInGame(true, true);
            }

            bool isVisible = cheatPanel->IsVisible();
            LOG_INFO("[MenuPoc] CheatPanel IsVisible after: " << (isVisible ? "true" : "false"));

            result << "POC3 OK: CheatPanel visibility=" << isVisible << " (was " << wasVisible << ")";
            Mod::ModFeedback::ShowMessage(L"POC3: CheatPanel toggled", 3.0f, SDK::FLinearColor{1.0f, 1.0f, 0.2f, 1.0f});

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
                    if (bpInfoPanel)
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

            // Try to show player info
            LOG_INFO("[MenuPoc] Calling ShowPlayerInfo()...");
            infoPanel->ShowPlayerInfo();

            SDK::EInfoPanelState newState = infoPanel->GetInfoPanelState();
            LOG_INFO("[MenuPoc] InfoPanel state after ShowPlayerInfo: " << static_cast<int>(newState));

            result << "POC5 OK: Called ShowPlayerInfo(), state: " << static_cast<int>(state) << " -> " << static_cast<int>(newState);
            Mod::ModFeedback::ShowMessage(L"POC5: ShowPlayerInfo called", 3.0f, SDK::FLinearColor{0.5f, 0.5f, 1.0f, 1.0f});

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

    } // namespace MenuPoc
} // namespace Mod
