

/*
AILEARNINGS
Implemented randomized spawning and NPC hierarchy diagnostics.
Symptom: spawn_mimics only spawning one type of NPC (Scout).
Cause: Static class discovery in ArenaSubsystem returning limited results if other mimics aren't loaded.
Solution: Added 'diag_mimics' for tracing, 'spawn_mimics' now uses true randomization from discovered classes, and improved 'DiscoverMimicClasses' in ArenaSubsystem.
*/

#include "CommandHandler.hpp"

#include <sstream>
#include <algorithm>
#include <random>

#include "Logging.hpp"
#include "CommandQueue.hpp"
#include "Cheats.hpp"
#include "ModFeedback.hpp"

namespace Mod
{
    // Global cheats instance
    static Cheats g_Cheats;

    CommandHandlerRegistry::CommandHandlerRegistry()
    {
        // Initialize AI subsystem
        aiSubsystem_.Initialize();

        // Initialize Item subsystem
        itemSubsystem_.Initialize();

        arenaSubsystem_.Initialize(&aiSubsystem_);
    }

    void CommandHandlerRegistry::Register(const std::string &name, CommandHandler handler)
    {
        handlers_[name] = handler;
        LOG_INFO("[Command] Registered handler: " << name);
    }

    std::string CommandHandlerRegistry::Handle(SDK::UWorld *world, const std::string &commandLine)
    {
        if (commandLine.empty())
        {
            return "";
        }

        std::vector<std::string> tokens = SplitCommandLine(commandLine);
        if (tokens.empty())
        {
            return "";
        }

        const std::string &command = tokens[0];

        // Convert command to lowercase for case-insensitive matching
        std::string commandLower = command;
        std::transform(commandLower.begin(), commandLower.end(), commandLower.begin(), ::tolower);

        // Build args vector (everything after the command)
        std::vector<std::string> args(tokens.begin() + 1, tokens.end());

        auto it = handlers_.find(commandLower);
        if (it != handlers_.end())
        {
            try
            {
                return it->second(world, args);
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("[Command] Exception handling command: " << e.what());
                return std::string("Error: ") + e.what();
            }
        }

        return "Unknown command: " + command;
    }

    std::string CommandHandlerRegistry::GetHelp() const
    {
        std::ostringstream help;
        help << "Available commands:\n";
        for (const auto &pair : handlers_)
        {
            help << "  " << pair.first << "\n";
        }
        return help.str();
    }

    std::vector<std::string> CommandHandlerRegistry::SplitCommandLine(const std::string &commandLine) const
    {
        std::istringstream stream(commandLine);
        std::vector<std::string> tokens;
        std::string token;
        while (stream >> token)
        {
            tokens.push_back(token);
        }
        return tokens;
    }

    void CommandHandlerRegistry::InitializeDefaults()
    {
        // Register help command
        Register("help", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            return GetHelp(); });

        // Register spawn_npc command
        Register("spawn_npc", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleSpawnNPC(world, args); });

        // Register spawn_npc_full command
        Register("spawn_npc_full", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleSpawnNPCFull(world, args); });

        // Register clear_npc/clear_npcs command
        Register("clear_npc", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleClearNPCs(world, args); });

        Register("clear_npcs", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleClearNPCs(world, args); });

        // Register list_npcs command
        Register("list_npcs", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleListNPCs(world, args); });

        // Register reinit_npc command
        Register("reinit_npc", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleReinitNPC(world, args); });

        // Register spawn_item command
        Register("spawn_item", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleSpawnItem(world, args); });

        // Register spawn_item_full command
        Register("spawn_item_full", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleSpawnItemFull(world, args); });

        // Register clear_items command
        Register("clear_items", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleClearItems(world, args); });

        // Register list_items command
        Register("list_items", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleListItems(world, args); });

        // Register cheat commands
        Register("god", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleGodMode();
            return g_Cheats.GetStatus(); });

        Register("ammo", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleUnlimitedAmmo();
            return g_Cheats.GetStatus(); });

        Register("durability", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleDurabilityBypass();
            return g_Cheats.GetStatus(); });

        Register("hunger", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleHungerDisabled();
            return g_Cheats.GetStatus(); });

        Register("fatigue", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleFatigueDisabled();
            return g_Cheats.GetStatus(); });

        Register("cheats", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            Mod::ModFeedback::ShowMessage(L"[Mod] Cheats status requested", 2.0f, SDK::FLinearColor{0.8f, 0.8f, 0.8f, 1.0f});
            return g_Cheats.GetStatus(); });

        Register("bullettime", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            if (!args.empty()) {
                try {
                    float scale = std::stof(args[0]);
                    g_Cheats.SetBulletTimeScale(scale);
                } catch (...) {
                    return "Invalid scale value: " + args[0];
                }
            }
            g_Cheats.ToggleBulletTime();
            return g_Cheats.GetStatus(); });

        // Arena: simplified commands
        Register("arena_start", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            int count = 30;
            float distance = 7000.0f;

            if (args.size() >= 1)
            {
                try { count = std::stoi(args[0]); } catch (...) { return "Invalid count: " + args[0]; }
            }
            if (args.size() >= 2)
            {
                try { distance = std::stof(args[1]); } catch (...) { return "Invalid distance: " + args[1]; }
            }
            // enable unlimited ammo, godmode, hunger and fatigue fixes before loading arena to avoid messing with player combat loop
            g_Cheats.SetUnlimitedAmmo(true);
            g_Cheats.SetGodMode(true);
            g_Cheats.SetHungerDisabled(true);
            g_Cheats.SetFatigueDisabled(true);




            arenaSubsystem_.Start(count, distance);
            return arenaSubsystem_.GetStatus(); });

        Register("arena_stop", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            arenaSubsystem_.Stop();
            return arenaSubsystem_.GetStatus(); });

        LOG_INFO("[Command] Simplified Arena commands initialized");
    }

    // Expose cheats update for main loop
    Cheats *GetCheats()
    {
        return &g_Cheats;
    }

    std::string CommandHandlerRegistry::HandleHelp(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)world;
        (void)args;
        return GetHelp();
    }

    std::string CommandHandlerRegistry::HandleSpawnNPC(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        if (args.empty())
        {
            return "Usage: spawn_npc <ClassName> [distance]";
        }

        AI::SpawnParams params;
        params.className = args[0];
        params.useFullName = false;

        if (args.size() >= 2)
        {
            try
            {
                params.distance = std::stof(args[1]);
            }
            catch (...)
            {
                return "Invalid distance: " + args[1];
            }
        }

        auto result = aiSubsystem_.SpawnNPC(world, params);
        if (result.success)
        {
            std::ostringstream response;
            response << "Spawned: " << result.message << " (total: " << aiSubsystem_.GetNPCCount(world) << ")";
            return response.str();
        }

        return "Failed: " + result.message;
    }

    std::string CommandHandlerRegistry::HandleSpawnNPCFull(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        if (args.empty())
        {
            return "Usage: spawn_npc_full <FullClassName> [distance]";
        }

        // For full names, everything after the command is the class name (may contain spaces)
        // Find the last argument that could be a distance
        AI::SpawnParams params;
        params.useFullName = true;

        if (args.size() >= 2)
        {
            // Check if last argument is a number (distance)
            const std::string &lastArg = args.back();
            try
            {
                params.distance = std::stof(lastArg);
                // Join all but last as class name
                std::ostringstream className;
                for (size_t i = 0; i < args.size() - 1; ++i)
                {
                    if (i > 0)
                        className << " ";
                    className << args[i];
                }
                params.className = className.str();
            }
            catch (...)
            {
                // Last arg is not a number, use all as class name
                std::ostringstream className;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    if (i > 0)
                        className << " ";
                    className << args[i];
                }
                params.className = className.str();
            }
        }
        else
        {
            params.className = args[0];
        }

        auto result = aiSubsystem_.SpawnNPC(world, params);
        if (result.success)
        {
            std::ostringstream response;
            response << "Spawned: " << result.message << " (total: " << aiSubsystem_.GetNPCCount(world) << ")";
            return response.str();
        }

        return "Failed: " + result.message;
    }

    std::string CommandHandlerRegistry::HandleClearNPCs(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)args;
        int removed = aiSubsystem_.ClearNPCs(world);
        std::ostringstream response;
        response << "Cleared NPCs: " << removed;
        return response.str();
    }

    std::string CommandHandlerRegistry::HandleListNPCs(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        size_t maxLines = 10;
        if (!args.empty())
        {
            try
            {
                maxLines = std::stoul(args[0]);
            }
            catch (...)
            {
                // Use default
            }
        }
        return aiSubsystem_.ListNPCs(world, maxLines);
    }

    std::string CommandHandlerRegistry::HandleReinitNPC(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)args;
        aiSubsystem_.ReinitNPC(world);
        return "NPC system reinitialized";
    }

    std::string CommandHandlerRegistry::HandleSpawnItem(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        if (args.empty())
        {
            return "Usage: spawn_item <ClassName> [distance]";
        }

        Items::SpawnParams params;
        params.className = args[0];
        params.useFullName = false;

        if (args.size() >= 2)
        {
            try
            {
                params.distance = std::stof(args[1]);
            }
            catch (...)
            {
                return "Invalid distance: " + args[1];
            }
        }

        auto result = itemSubsystem_.SpawnItem(world, params);
        if (result.success)
        {
            std::ostringstream response;
            response << "Spawned: " << result.message << " (total: " << itemSubsystem_.GetItemCount(world) << ")";
            return response.str();
        }

        return "Failed: " + result.message;
    }

    std::string CommandHandlerRegistry::HandleSpawnItemFull(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        if (args.empty())
        {
            return "Usage: spawn_item_full <FullClassName> [distance]";
        }

        Items::SpawnParams params;
        params.useFullName = true;

        if (args.size() >= 2)
        {
            // Check if last argument is a number (distance)
            const std::string &lastArg = args.back();
            try
            {
                params.distance = std::stof(lastArg);
                // Join all but last as class name
                std::ostringstream className;
                for (size_t i = 0; i < args.size() - 1; ++i)
                {
                    if (i > 0)
                        className << " ";
                    className << args[i];
                }
                params.className = className.str();
            }
            catch (...)
            {
                // Last arg is not a number, use all as class name
                std::ostringstream className;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    if (i > 0)
                        className << " ";
                    className << args[i];
                }
                params.className = className.str();
            }
        }
        else
        {
            params.className = args[0];
        }

        auto result = itemSubsystem_.SpawnItem(world, params);
        if (result.success)
        {
            std::ostringstream response;
            response << "Spawned: " << result.message << " (total: " << itemSubsystem_.GetItemCount(world) << ")";
            return response.str();
        }

        return "Failed: " + result.message;
    }

    std::string CommandHandlerRegistry::HandleClearItems(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)args;
        int removed = itemSubsystem_.ClearItems(world);
        std::ostringstream response;
        response << "Cleared items: " << removed;
        return response.str();
    }

    std::string CommandHandlerRegistry::HandleListItems(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        size_t maxLines = 10;
        if (!args.empty())
        {
            try
            {
                maxLines = std::stoul(args[0]);
            }
            catch (...)
            {
                // Use default
            }
        }
        return itemSubsystem_.ListItems(world, maxLines);
    }
}
