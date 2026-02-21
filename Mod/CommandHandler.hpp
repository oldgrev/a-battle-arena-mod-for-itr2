#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>

#include "..\CppSDK\SDK.hpp"
#include "AISubsystem.hpp"
#include "ArenaSubsystem.hpp"
#include "ItemSubsystem.hpp"
#include "Cheats.hpp"

namespace Mod
{
    // Forward declarations
    class CommandQueue;

    // Command handler type
    using CommandHandler = std::function<std::string(SDK::UWorld *, const std::vector<std::string> &)>;

    class CommandHandlerRegistry
    {
    public:
        CommandHandlerRegistry();
        ~CommandHandlerRegistry() = default;

        // Register a command handler
        void Register(const std::string &name, CommandHandler handler);

        // Handle a command and return response
        std::string Handle(SDK::UWorld *world, const std::string &commandLine);

        // Get help text
        std::string GetHelp() const;

        // Initialize all built-in commands
        void InitializeDefaults();

    private:
        std::map<std::string, CommandHandler> handlers_;

        // Helper to split command line
        std::vector<std::string> SplitCommandLine(const std::string &commandLine) const;

        // Built-in command implementations
        std::string HandleHelp(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleSpawnNPC(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleSpawnNPCFull(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleClearNPCs(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleListNPCs(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleReinitNPC(SDK::UWorld *world, const std::vector<std::string> &args);

        // Built-in command implementations for items
        std::string HandleSpawnItem(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleSpawnItemFull(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleClearItems(SDK::UWorld *world, const std::vector<std::string> &args);
        std::string HandleListItems(SDK::UWorld *world, const std::vector<std::string> &args);

        // AI Subsystem instance
        AI::AISubsystem aiSubsystem_;

        // Arena wave prototype
        Arena::ArenaSubsystem arenaSubsystem_;

        // Item Subsystem instance
        Items::ItemSubsystem itemSubsystem_;
    };

    // Get the global cheats instance
    Cheats *GetCheats();
}
