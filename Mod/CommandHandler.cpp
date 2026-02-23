

/*
AILEARNINGS
Implemented randomized spawning and NPC hierarchy diagnostics.
Symptom: spawn_mimics only spawning one type of NPC (Scout).
Cause: Static class discovery in ArenaSubsystem returning limited results if other mimics aren't loaded.
Solution: Added 'diag_mimics' for tracing, 'spawn_mimics' now uses true randomization from discovered classes, and improved 'DiscoverMimicClasses' in ArenaSubsystem.

VR iteration learning: typing long commands using a VR keyboard is slow/error-prone and wastes a full test cycle.
Solution: add short aliases and a one-shot "vr_diag" command to enable/disable the key diagnostics (notif bridge + tablet diag + trace filters) with minimal input.
*/

#include "CommandHandler.hpp"

#include <sstream>
#include <algorithm>
#include <random>

#include "Logging.hpp"
#include "CommandQueue.hpp"
#include "Cheats.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"
#include "ModTuning.hpp"
#include "HookManager.hpp"

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

        // When the mod is running in the main menu / map load, UWorld can be null.
        // Still respond to TCP clients so it doesn't look "dead".
        if (!world && commandLower != "help")
        {
            return "World not ready yet (still loading / in menu). Try again once you're in a level. (Use: help)";
        }

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

        // new cheats matching the Lua helper functions
        Register("money", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            int amount = 0;
            if (!args.empty())
            {
                try { amount = std::stoi(args[0]); }
                catch (...) { return "Invalid amount: " + args[0]; }
            }
            g_Cheats.AddMoney(amount);
            return "Money command executed";
        });

        Register("access", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            if (args.empty())
                return "Usage: access <level>";
            int level = 0;
            try { level = std::stoi(args[0]); } catch (...) { return "Invalid level: " + args[0]; }
            g_Cheats.SetAccessLevel(level);
            return "Access level set to " + std::to_string(level);
        });

        Register("noclip", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleNoClip();
            return g_Cheats.GetStatus();
        });

        Register("jump", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleJumpAllowed();
            return g_Cheats.GetStatus();
        });

        Register("debug", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleDebugMode();
            return g_Cheats.GetStatus();
        });

        // convenience aliases matching the Lua keybind names
        Register("health", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { (void)world; (void)args; g_Cheats.ToggleGodMode(); return g_Cheats.GetStatus(); });
        Register("stamina", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { (void)world; (void)args; g_Cheats.ToggleFatigueDisabled(); return g_Cheats.GetStatus(); });

        // Arena: simplified commands
        Register("arena_start", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            int count = Mod::Tuning::kArenaDefaultWaveSize;
            float distance = Mod::Tuning::kArenaDefaultSpawnDistance;

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

        // Feedback diagnostics: verify VR-visible messages and both 2D and 3D sound playback.
        Register("fb_test", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)args;

            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
            if (!player)
            {
                Mod::ModFeedback::ShowMessage(L"[Mod] fb_test: player not ready", 2.0f, SDK::FLinearColor{1.0f, 0.4f, 0.4f, 1.0f});
                return "fb_test: player not ready";
            }

            Mod::ModFeedback::ShowMessage(L"[Mod] fb_test: subtitles + sound", 3.0f, SDK::FLinearColor{0.4f, 1.0f, 0.4f, 1.0f});

            // 2D sound (UI/confirmation)
            if (player->RadioStartSound)
            {
                // Distinct pitch so you can tell 2D vs 3D in one test run.
                Mod::ModFeedback::PlaySound2D(player->RadioStartSound, 1.0f, 1.25f, true);
            }

            // 3D sound in front of the player's view
            SDK::FVector viewLoc{};
            SDK::FRotator viewRot{};
            if (Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
            {
                const SDK::FVector forward = SDK::UKismetMathLibrary::GetForwardVector(viewRot);
                SDK::FVector soundLoc;
                soundLoc.X = viewLoc.X + forward.X * 150.0;
                soundLoc.Y = viewLoc.Y + forward.Y * 150.0;
                soundLoc.Z = viewLoc.Z + forward.Z * 150.0;

                if (player->RadioEndSound)
                {
                    Mod::ModFeedback::PlaySoundAtLocation(player->RadioEndSound, soundLoc, 1.0f, 0.75f);
                }

                Mod::ModFeedback::ShowWorldText(soundLoc, L"fb_test 3D sound here", 3.0f, SDK::FLinearColor{0.8f, 0.8f, 1.0f, 1.0f});
            }

            return "fb_test: triggered subtitles + 2D + 3D sounds"; });

        // Notification test: force-emit subtitles using the same PlayerCharacter->ShowSubtitles
        // path observed in traces (e.g. safety/no mag). This is the ONLY supported mod feedback path.
        auto notifyHandler = [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
             {
            (void)world;
            // notify_test [safety|nomag|custom...]
            std::wstring msg;
            if (args.empty())
            {
                msg = L"Safety is on! Switch it off with <b>{BTN_Top}</b>.";
            }
            else
            {
                std::string kind = args[0];
                std::transform(kind.begin(), kind.end(), kind.begin(), ::tolower);
                if (kind == "safety")
                    msg = L"Safety is on! Switch it off with <b>{BTN_Top}</b>.";
                else if (kind == "nomag")
                    msg = L"No mag! Insert it and\npull the charging handle!";
                else
                {
                    // Treat remaining tokens as free-form (lossy widen).
                    std::wstring out;
                    for (size_t i = 0; i < args.size(); ++i)
                    {
                        if (i) out.push_back(L' ');
                        out.append(std::wstring(args[i].begin(), args[i].end()));
                    }
                    msg = out;
                }
            }

            // NOTE: ModFeedback::ShowMessage is intentionally a single-path implementation now.
            Mod::ModFeedback::ShowMessage(msg.c_str(), 3.0f, SDK::FLinearColor{0.8f, 0.9f, 1.0f, 1.0f});
            return "notify_test: emitted";
        };

        Register("notify_test", notifyHandler);

        // Short alias for VR keyboard.
        Register("n", notifyHandler);

        // Subtitle format/length test: staggered sequence to probe what breaks.
        // Usage:
        //   subtitle_test            -> start with default interval
        //   subtitle_test 500        -> start with 500ms interval
        //   subtitle_test stop       -> stop
        Register("subtitle_test", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world;
            if (!args.empty())
            {
                std::string v = args[0];
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                if (v == "stop" || v == "off")
                {
                    Mod::ModFeedback::StopSubtitleTestSequence();
                    return "subtitle_test: stopped";
                }
                try
                {
                    const uint32_t intervalMs = static_cast<uint32_t>(std::stoul(args[0]));
                    Mod::ModFeedback::StartSubtitleTestSequence(intervalMs);
                    return std::string("subtitle_test: started intervalMs=") + std::to_string(intervalMs);
                }
                catch (...)
                {
                    return "subtitle_test: invalid arg (use: subtitle_test [intervalMs|stop])";
                }
            }

            Mod::ModFeedback::StartSubtitleTestSequence(900);
            return "subtitle_test: started intervalMs=900";
        });

        // CRUFT (should be removed): Notification bridge mirrors game subtitles into a popup.
        // Kept for now because it can still help diagnose headset UI issues, but it is not
        // part of the supported mod feedback path.
        Register("notif_bridge", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world;
            if (args.empty())
            {
                return std::string("notif_bridge: ")
                    + (Mod::HookManager::NotifBridge_IsEnabled() ? "on" : "off")
                    + " sound=" + (Mod::HookManager::NotifBridge_IsPlaySoundEnabled() ? "on" : "off")
                    + " (usage: notif_bridge on|off [sound])";
            }

            std::string v = args[0];
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            if (v == "on")
            {
                bool sound = false;
                if (args.size() >= 2)
                {
                    std::string s = args[1];
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    sound = (s == "sound" || s == "1" || s == "true" || s == "on");
                }
                Mod::HookManager::NotifBridge_SetEnabled(true, sound);
                Mod::ModFeedback::ShowMessage(sound ? L"[Mod] notif_bridge ON (popup + sound)" : L"[Mod] notif_bridge ON (popup)", 2.0f);
                return std::string("notif_bridge: on sound=") + (sound ? "on" : "off");
            }
            if (v == "off")
            {
                Mod::HookManager::NotifBridge_SetEnabled(false, false);
                Mod::ModFeedback::ShowMessage(L"[Mod] notif_bridge OFF", 2.0f);
                return "notif_bridge: off";
            }
            return "Usage: notif_bridge on|off [sound]"; });

        // Short alias: nb -> notif_bridge
        Register("nb", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            // Forward to notif_bridge behavior
            if (args.empty())
            {
                return std::string("nb: ")
                    + (Mod::HookManager::NotifBridge_IsEnabled() ? "on" : "off")
                    + " sound=" + (Mod::HookManager::NotifBridge_IsPlaySoundEnabled() ? "on" : "off")
                    + " (usage: nb on|off [sound])";
            }
            std::string v = args[0];
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            if (v == "on")
            {
                bool sound = false;
                if (args.size() >= 2)
                {
                    std::string s = args[1];
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    sound = (s == "sound" || s == "1" || s == "true" || s == "on");
                }
                Mod::HookManager::NotifBridge_SetEnabled(true, sound);
                Mod::ModFeedback::ShowMessage(sound ? L"[Mod] notif_bridge ON (popup + sound)" : L"[Mod] notif_bridge ON (popup)", 2.0f);
                return std::string("nb: on sound=") + (sound ? "on" : "off");
            }
            if (v == "off")
            {
                Mod::HookManager::NotifBridge_SetEnabled(false, false);
                Mod::ModFeedback::ShowMessage(L"[Mod] notif_bridge OFF", 2.0f);
                return "nb: off";
            }
            return "Usage: nb on|off [sound]"; });

        // Alias (people will type this in VR): notify_bridge -> notif_bridge
        Register("notify_bridge", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            // Same behavior as notif_bridge
            if (args.empty())
            {
                return std::string("notify_bridge (alias): ")
                    + (Mod::HookManager::NotifBridge_IsEnabled() ? "on" : "off")
                    + " sound=" + (Mod::HookManager::NotifBridge_IsPlaySoundEnabled() ? "on" : "off")
                    + " (usage: notify_bridge on|off [sound])";
            }
            std::string v = args[0];
            std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            if (v == "on")
            {
                bool sound = false;
                if (args.size() >= 2)
                {
                    std::string s = args[1];
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    sound = (s == "sound" || s == "1" || s == "true" || s == "on");
                }
                Mod::HookManager::NotifBridge_SetEnabled(true, sound);
                Mod::ModFeedback::ShowMessage(sound ? L"[Mod] notif_bridge ON (popup + sound)" : L"[Mod] notif_bridge ON (popup)", 2.0f);
                return std::string("notify_bridge: on sound=") + (sound ? "on" : "off");
            }
            if (v == "off")
            {
                Mod::HookManager::NotifBridge_SetEnabled(false, false);
                Mod::ModFeedback::ShowMessage(L"[Mod] notif_bridge OFF", 2.0f);
                return "notify_bridge: off";
            }
            return "Usage: notify_bridge on|off [sound]"; });

            Register("tablet_last", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
                (void)world; (void)args;
                return Mod::HookManager::TabletDiag_GetLastHolsteredSummary(); });

            Register("tablet_diag", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
                (void)world;
                if (args.empty())
                {
                    return std::string("tablet_diag: ") + (Mod::HookManager::TabletDiag_IsEnabled() ? "on" : "off")
                        + " (usage: tablet_diag on|off|last)";
                }
                std::string v = args[0];
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                if (v == "on")
                {
                    Mod::HookManager::TabletDiag_SetEnabled(true);
                    Mod::ModFeedback::ShowMessage(L"[Mod] tablet_diag ON", 2.0f);
                    return "tablet_diag: on";
                }
                if (v == "off")
                {
                    Mod::HookManager::TabletDiag_SetEnabled(false);
                    Mod::ModFeedback::ShowMessage(L"[Mod] tablet_diag OFF", 2.0f);
                    return "tablet_diag: off";
                }
                if (v == "last")
                {
                    return Mod::HookManager::TabletDiag_GetLastInteractionSummary();
                }
                return "Usage: tablet_diag on|off|last"; });

            // Short alias: td -> tablet_diag
            Register("td", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
                (void)world;
                if (args.empty())
                {
                    return std::string("td: ") + (Mod::HookManager::TabletDiag_IsEnabled() ? "on" : "off")
                        + " (usage: td on|off|last)";
                }
                std::string v = args[0];
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                if (v == "on")
                {
                    Mod::HookManager::TabletDiag_SetEnabled(true);
                    Mod::ModFeedback::ShowMessage(L"[Mod] tablet_diag ON", 2.0f);
                    return "td: on";
                }
                if (v == "off")
                {
                    Mod::HookManager::TabletDiag_SetEnabled(false);
                    Mod::ModFeedback::ShowMessage(L"[Mod] tablet_diag OFF", 2.0f);
                    return "td: off";
                }
                if (v == "last")
                {
                    return Mod::HookManager::TabletDiag_GetLastInteractionSummary();
                }
                return "Usage: td on|off|last"; });

            // One-shot VR diagnostics to reduce VR keyboard churn.
            // vr_diag on  => trace_reset + trace_on with narrow filters + notif bridge (sound) + tablet diag
            // vr_diag off => trace_off + trace_flush + disable notif bridge + disable tablet diag
            Register("vr_diag", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
                (void)world;
                if (args.empty())
                {
                    return "vr_diag usage: vr_diag on|off";
                }
                std::string v = args[0];
                std::transform(v.begin(), v.end(), v.begin(), ::tolower);
                if (v == "on")
                {
                    // Keep the trace scoped to the known high-signal notification path.
                    Mod::HookManager::Trace_Reset();
                    Mod::HookManager::Trace_SetFilter("bp_radiusplayercharacter_gameplay");
                    Mod::HookManager::Trace_SetObjectFilter("bp_radiusplayercharacter_gameplay");
                    Mod::HookManager::Trace_SetEnabled(true);

                    Mod::HookManager::NotifBridge_SetEnabled(true, true);
                    Mod::HookManager::TabletDiag_SetEnabled(true);
                    Mod::ModFeedback::ShowMessage(L"[Mod] vr_diag ON (trace + notif_bridge + tablet_diag)", 3.0f);
                    return "vr_diag: on";
                }
                if (v == "off")
                {
                    Mod::HookManager::NotifBridge_SetEnabled(false, false);
                    Mod::HookManager::TabletDiag_SetEnabled(false);
                    Mod::HookManager::Trace_SetEnabled(false);
                    Mod::HookManager::Trace_Flush();
                    Mod::ModFeedback::ShowMessage(L"[Mod] vr_diag OFF", 2.0f);
                    return "vr_diag: off";
                }
                return "vr_diag usage: vr_diag on|off"; });

        // -----------------------------------------------------------------
        // Diagnostics: ProcessEvent tracing
        // -----------------------------------------------------------------
        Register("trace_on", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world;
            // Optional: trace_on <fnFilterSubstring> [objFilterSubstring]
            // Use 'none' to clear either filter.
            if (args.size() >= 1)
            {
                std::string filter = args[0];
                std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
                if (filter == "none") filter.clear();
                Mod::HookManager::Trace_SetFilter(filter);
            }

            if (args.size() >= 2)
            {
                std::string objFilter = args[1];
                std::transform(objFilter.begin(), objFilter.end(), objFilter.begin(), ::tolower);
                if (objFilter == "none") objFilter.clear();
                Mod::HookManager::Trace_SetObjectFilter(objFilter);
            }
            Mod::HookManager::Trace_SetEnabled(true);
            return "trace_on: enabled"; });

        Register("trace_off", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world; (void)args;
            Mod::HookManager::Trace_SetEnabled(false);
            return "trace_off: disabled"; });

        Register("trace_reset", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world; (void)args;
            Mod::HookManager::Trace_Reset();
            return "trace_reset: ok"; });

           Register("trace_flush", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                  {
              (void)world; (void)args;
              Mod::HookManager::Trace_Flush();
              return "trace_flush: ok"; });

           Register("trace_path", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                  {
              (void)world; (void)args;
              return std::string("trace_path: ") + Mod::HookManager::Trace_GetFilePath(); });

        Register("trace_dump", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
            (void)world;
            int topN = 30;
            int lastN = 50;
            if (args.size() >= 1) { try { topN = std::stoi(args[0]); } catch (...) {} }
            if (args.size() >= 2) { try { lastN = std::stoi(args[1]); } catch (...) {} }

            std::string dump = Mod::HookManager::Trace_Dump(topN, lastN);
            // Also emit a single log line marker so you can correlate in the file.
            LOG_INFO("[Trace] Dump requested (top=" << topN << " last=" << lastN << ")");
            return dump; });

            Register("trace_dump_full", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                 {
                (void)world; (void)args;
                // Full underlying events are written continuously to the dedicated trace file.
                // This command forces a flush and returns the path (crash-survivable retrieval).
                Mod::HookManager::Trace_Flush();
                return std::string("trace_dump_full: ") + Mod::HookManager::Trace_GetFilePath(); });

            // Holster-focused trace shortcuts (high signal for loadout attachment debugging)
            Register("holster_trace_on", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                     {
                (void)world;
                // Usage: holster_trace_on [objFilterSubstring]
                // Defaults: fn filter = "holster" (catches StartHolstering/InstantHolsterActor/etc)
                // Optional obj filter helps scope to molle/holder components.
                Mod::HookManager::Trace_Reset();
                Mod::HookManager::Trace_SetFilter("holster");

                std::string objFilter;
                if (!args.empty())
                {
                    objFilter = args[0];
                    std::transform(objFilter.begin(), objFilter.end(), objFilter.begin(), ::tolower);
                    if (objFilter == "none") objFilter.clear();
                }
                Mod::HookManager::Trace_SetObjectFilter(objFilter);
                Mod::HookManager::Trace_SetEnabled(true);
                return std::string("holster_trace_on: enabled (trace_path=") + Mod::HookManager::Trace_GetFilePath() + ")";
            });

            Register("holster_trace_off", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
                     {
                (void)world; (void)args;
                Mod::HookManager::Trace_SetEnabled(false);
                Mod::HookManager::Trace_Flush();
                return std::string("holster_trace_off: disabled (flushed to ") + Mod::HookManager::Trace_GetFilePath() + ")";
            });

        // -----------------------------------------------------------------
        // Loadout commands
        // -----------------------------------------------------------------
        Register("capture", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleCapture(world, args); });

        Register("loadouts", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleLoadouts(world, args); });

        Register("loadout", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleLoadout(world, args); });

        Register("apply", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleApplyLoadout(world, args); });

        // Short alias for apply
        Register("equip", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 { return HandleApplyLoadout(world, args); });

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
        size_t maxLines = Mod::Tuning::kCommandDefaultListMaxLines;
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
        size_t maxLines = Mod::Tuning::kCommandDefaultListMaxLines;
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

    // -----------------------------------------------------------------
    // Loadout command handlers
    // -----------------------------------------------------------------
    
    std::string CommandHandlerRegistry::HandleCapture(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        std::string loadoutName = "default";
        if (!args.empty())
        {
            loadoutName = args[0];
        }
        
        auto* loadoutSubsystem = Loadout::LoadoutSubsystem::Get();
        if (!loadoutSubsystem)
        {
            return "Error: LoadoutSubsystem not available";
        }
        
        return loadoutSubsystem->CaptureLoadout(world, loadoutName);
    }
    
    std::string CommandHandlerRegistry::HandleLoadouts(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)world;
        (void)args;
        
        auto* loadoutSubsystem = Loadout::LoadoutSubsystem::Get();
        if (!loadoutSubsystem)
        {
            return "Error: LoadoutSubsystem not available";
        }
        
        return loadoutSubsystem->ListLoadouts();
    }
    
    std::string CommandHandlerRegistry::HandleLoadout(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        (void)world;
        
        auto* loadoutSubsystem = Loadout::LoadoutSubsystem::Get();
        if (!loadoutSubsystem)
        {
            return "Error: LoadoutSubsystem not available";
        }
        
        if (args.empty())
        {
            // Show current selection
            std::string selected = loadoutSubsystem->GetSelectedLoadout();
            if (selected.empty())
            {
                return "No loadout selected. Usage: loadout <name>";
            }
            return "Selected loadout: " + selected;
        }
        
        std::string loadoutName = args[0];
        
        // Check if loadout exists
        if (!loadoutSubsystem->LoadoutExists(loadoutName))
        {
            return "Error: Loadout '" + loadoutName + "' not found. Use 'loadouts' to list available.";
        }
        
        loadoutSubsystem->SetSelectedLoadout(loadoutName);
        
        Mod::ModFeedback::ShowMessage(
            (L"Loadout selected: " + std::wstring(loadoutName.begin(), loadoutName.end())).c_str(),
            2.0f,
            SDK::FLinearColor{0.5f, 0.8f, 1.0f, 1.0f});
        
        return "Selected loadout: " + loadoutName;
    }
    
    std::string CommandHandlerRegistry::HandleApplyLoadout(SDK::UWorld *world, const std::vector<std::string> &args)
    {
        auto* loadoutSubsystem = Loadout::LoadoutSubsystem::Get();
        if (!loadoutSubsystem)
        {
            return "Error: LoadoutSubsystem not available";
        }
        
        std::string loadoutName;
        if (!args.empty())
        {
            loadoutName = args[0];
        }
        else
        {
            // Use selected loadout
            loadoutName = loadoutSubsystem->GetSelectedLoadout();
            if (loadoutName.empty())
            {
                return "Error: No loadout specified and none selected. Usage: apply <loadout_name>";
            }
        }
        
        // Check if loadout exists
        if (!loadoutSubsystem->LoadoutExists(loadoutName))
        {
            return "Error: Loadout '" + loadoutName + "' not found. Use 'loadouts' to list available.";
        }
        
        return loadoutSubsystem->ApplyLoadout(world, loadoutName);
    }
}
