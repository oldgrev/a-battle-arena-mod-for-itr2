#pragma once

/**
 * MenuPocSubsystem.hpp
 * 
 * Proof-of-concept implementations that attempt to spawn/show a mod menu
 * attached to the player's left hand using various approaches discovered
 * from SDK/trace analysis.
 * 
 * Each POC is triggered by a telnet command: poc1, poc2, poc3, poc4, poc5
 */

#include <string>

namespace Mod
{
    namespace MenuPoc
    {
        /**
         * POC 1: Use RadiusIngameMenuSubsystem::SwitchIngameMenu(true)
         * Tests if we can toggle the built-in in-game menu system.
         * Expected: The game's menu appears in front of the player.
         */
        std::string Poc1_SwitchIngameMenu();

        /**
         * POC 2: Create and attach a RadiusWidgetComponent to the player's left hand.
         * Tests dynamic widget component creation and attachment.
         * Expected: A widget component appears on the left hand.
         */
        std::string Poc2_AttachWidgetToLeftHand();

        /**
         * POC 3: Find the CheatPanel on FPS_VRCharacter and toggle visibility.
         * The game has a pre-existing CheatPanel widget component.
         * Expected: CheatPanel becomes visible near the player.
         */
        std::string Poc3_ShowCheatPanel();

        /**
         * POC 4: Spawn a BP_IngameMenu_C actor directly using SpawnActor.
         * Tests direct actor spawning for menu blueprint class.
         * Expected: An ingame menu actor spawns in the world.
         */
        std::string Poc4_SpawnIngameMenuActor();

        /**
         * POC 5: Find existing InfoPanel/AInfoPanel and call ShowPlayerInfo.
         * Tests using the vise-style ItemInfo panel system for custom display.
         * Expected: The info panel shows player info.
         */
        std::string Poc5_UseInfoPanel();

        /**
         * POC 6: Spawn actor using BPA_FaceFollowingMenu as base class.
         * Tests spawning a face-following menu that tracks the player's view.
         * Expected: A face-following menu spawns.
         */
        std::string Poc6_SpawnFaceFollowingMenu();

        /**
         * POC 7: Create a UserWidget using WidgetBlueprintLibrary::Create.
         * Tests widget creation via the UMG system (2D viewport widget).
         * Expected: A widget appears on the 2D viewport/HUD.
         */
        std::string Poc7_CreateUserWidget();

        /**
         * POC 8: Spawn BP_Confirmation_C actor (3D confirmation popup).
         * Tests spawning a 3D confirmation popup in front of player.
         * Expected: A confirmation dialog appears in 3D space.
         */
        std::string Poc8_SpawnConfirmation();

        /**
         * POC 9: Create WBP_Confirmation widget and attach to left hand.
         * Uses proper UUserWidget with known TextBlock members and attaches
         * to W_GripDebug_L widget component on the player's hand.
         * Expected: A confirmation-style widget appears on left hand with menu text.
         */
        std::string Poc9_CreateConfirmWidgetOnHand();

    } // namespace MenuPoc
} // namespace Mod
