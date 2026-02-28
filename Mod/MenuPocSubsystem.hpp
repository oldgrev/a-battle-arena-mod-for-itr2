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

    } // namespace MenuPoc
} // namespace Mod
