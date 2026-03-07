

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
#include <cctype>
#include <random>

#include "Logging.hpp"
#include "CommandQueue.hpp"
#include "Cheats.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"
#include "ModTuning.hpp"
#include "HookManager.hpp"
#include "FriendSubsystem.hpp"

namespace Mod
{
    namespace
    {
        bool ParseBoolArg(const std::string& text, bool defaultValue)
        {
            if (text.empty())
                return defaultValue;

            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower == "1" || lower == "true" || lower == "on" || lower == "yes")
                return true;
            if (lower == "0" || lower == "false" || lower == "off" || lower == "no")
                return false;
            return defaultValue;
        }

        bool TryParseFloatArg(const std::string& text, float& outValue)
        {
            try
            {
                outValue = std::stof(text);
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
    }

    // Global cheats instance
    static Cheats g_Cheats;

    CommandHandlerRegistry::CommandHandlerRegistry()
    {
        // Initialize AI subsystem
        aiSubsystem_.Initialize();

        // Initialize Item subsystem
        itemSubsystem_.Initialize();

        arenaSubsystem_.Initialize(&aiSubsystem_);

        // Initialize Friend subsystem
        Mod::Friend::FriendSubsystem::Get()->Initialize();
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
        std::vector<std::string> tokens;

        std::string current;
        bool inQuotes = false;
        char quoteChar = '\0';
        bool escaping = false;

        for (char ch : commandLine)
        {
            // if (escaping)
            // {
            //     current.push_back(ch);
            //     escaping = false;
            //     continue;
            // }

            // if (ch == '\\')
            // {
            //     escaping = true;
            //     continue;
            // }

            if (inQuotes)
            {
                if (ch == quoteChar)
                {
                    inQuotes = false;
                    quoteChar = '\0';
                }
                else
                {
                    current.push_back(ch);
                }
                continue;
            }

            if (ch == '"' || ch == '\'')
            {
                inQuotes = true;
                quoteChar = ch;
                continue;
            }

            if (std::isspace(static_cast<unsigned char>(ch)))
            {
                if (!current.empty())
                {
                    tokens.push_back(current);
                    current.clear();
                }
                continue;
            }

            current.push_back(ch);
        }

        // if (escaping)
        // {
        //     current.push_back('\\');
        // }

        if (!current.empty())
        {
            tokens.push_back(current);
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


        // // Register list_npcs command
        // Register("list_npcs", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
        //          { return HandleListNPCs(world, args); });

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

        // Register("sound2d", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: sound2d <SoftObjectPath> [volume] [pitch] [ui:0|1]";

        //     const std::string path = args[0];
        //     float volume = 1.0f;
        //     float pitch = 1.0f;
        //     bool isUi = true;

        //     if (args.size() >= 2)
        //     {
        //         try { volume = std::stof(args[1]); } catch (...) { return "Invalid volume: " + args[1]; }
        //     }
        //     if (args.size() >= 3)
        //     {
        //         try { pitch = std::stof(args[2]); } catch (...) { return "Invalid pitch: " + args[2]; }
        //     }
        //     if (args.size() >= 4)
        //     {
        //         isUi = ParseBoolArg(args[3], true);
        //     }

        //     std::string err;
        //     if (!Mod::ModFeedback::PlaySoundAsset2D(path, volume, pitch, isUi, &err))
        //         return "sound2d failed: " + err + " (path=" + path + ")";

        //     return "sound2d ok: " + path;
        // });

        // Register("sound3d", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: sound3d <SoftObjectPath> [volume] [pitch]";

        //     const std::string path = args[0];
        //     float volume = 1.0f;
        //     float pitch = 1.0f;

        //     if (args.size() >= 2)
        //     {
        //         try { volume = std::stof(args[1]); } catch (...) { return "Invalid volume: " + args[1]; }
        //     }
        //     if (args.size() >= 3)
        //     {
        //         try { pitch = std::stof(args[2]); } catch (...) { return "Invalid pitch: " + args[2]; }
        //     }

        //     std::string err;
        //     if (!Mod::ModFeedback::PlaySoundAsset3DAtPlayer(path, volume, pitch, &err))
        //         return "sound3d failed: " + err + " (path=" + path + ")";

        //     return "sound3d ok: " + path;
        // });

        // Register("soundasset_friend", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: soundasset_friend <SoftObjectPath> [volume]\n"
        //                "Tests SpawnSoundAttached with existing game MetaSoundSource attached to friend NPC.\n"
        //                "Example: soundasset_friend /Game/Audio/A_MS_WP_Fort17_Fire.A_MS_WP_Fort17_Fire 1.0";

        //     const std::string path = args[0];
        //     float volume = 1.0f;

        //     if (args.size() >= 2)
        //     {
        //         try { volume = std::stof(args[1]); }
        //         catch (...) { return "Invalid volume: " + args[1]; }
        //     }

        //     // Get the first friend actor to attach to
        //     auto* friendSub = Mod::Friend::FriendSubsystem::Get();
        //     if (!friendSub)
        //         return "soundasset_friend failed: FriendSubsystem not available";

        //     SDK::AActor* friendActor = friendSub->GetFirstFriendActor();
        //     if (!friendActor)
        //         return "soundasset_friend failed: No friend NPC found. Spawn one first (e.g. spawn_friend)";

        //     std::string err;
        //     if (!Mod::ModFeedback::PlaySoundAssetAttachedToActor(friendActor, path, volume, &err))
        //         return "soundasset_friend failed: " + err;

        //     std::string actorName = friendActor->GetName();
        //     return "soundasset_friend ok: " + path + " attached to " + actorName;
        // });

        // Register("wav3d_tpl", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.size() < 2)
        //         return "Usage: wav3d_tpl <TemplateSoftObjectPath> <WavFilePath> [loop:0|1] [volume]";

        //     const std::string templatePath = args[0];
        //     const std::string wavPath = args[1];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 3)
        //         loop = ParseBoolArg(args[2], false);
        //     if (args.size() >= 4)
        //     {
        //         try { volume = std::stof(args[3]); }
        //         catch (...) { return "Invalid volume: " + args[3]; }
        //     }

        //     std::string err;
        //     if (!Mod::ModFeedback::PlayWavAttachedToPlayerWithTemplate(templatePath, wavPath, loop, volume, &err))
        //         return "wav3d_tpl failed: " + err;

        //     return "wav3d_tpl ok: template=" + templatePath + " wav=" + wavPath;
        // });

        // Register("soundwaves", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     std::size_t maxEntries = 20;
        //     std::string containsFilter;
        //     bool includeDefaults = false;

        //     if (args.size() >= 1)
        //     {
        //         try { maxEntries = static_cast<std::size_t>(std::stoul(args[0])); }
        //         catch (...) { return "Invalid maxEntries: " + args[0]; }
        //     }
        //     if (args.size() >= 2)
        //         containsFilter = args[1];
        //     if (args.size() >= 3)
        //         includeDefaults = ParseBoolArg(args[2], false);

        //     return Mod::ModFeedback::DescribeLoadedSoundWaves(maxEntries, containsFilter, includeDefaults);
        // });

        // Register("wav3d_auto", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: wav3d_auto <WavFilePath> [loop:0|1] [volume] [containsFilter]";

        //     const std::string wavPath = args[0];
        //     bool loop = false;
        //     float volume = 1.0f;
        //     std::string containsFilter;

        //     if (args.size() >= 2)
        //         loop = ParseBoolArg(args[1], false);
        //     if (args.size() >= 3)
        //     {
        //         try { volume = std::stof(args[2]); }
        //         catch (...) { return "Invalid volume: " + args[2]; }
        //     }
        //     if (args.size() >= 4)
        //         containsFilter = args[3];

        //     std::string picked;
        //     std::string err;
        //     if (!Mod::ModFeedback::PlayWavAttachedToPlayerAutoTemplate(wavPath, loop, volume, containsFilter, &picked, &err))
        //         return "wav3d_auto failed: " + err;

        //     return "wav3d_auto ok: template='" + picked + "' wav='" + wavPath + "'";
        // });

        // Register("media_file", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: media_file <FilePath> [loop:0|1] [volume]";

        //     const std::string filePath = args[0];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 2)
        //         loop = ParseBoolArg(args[1], false);
        //     if (args.size() >= 3)
        //     {
        //         try { volume = std::stof(args[2]); } catch (...) { return "Invalid volume: " + args[2]; }
        //     }

        //     std::string err;
        //     if (!Mod::ModFeedback::PlayMediaFile2D(filePath, loop, volume, &err))
        //         return "media_file failed: " + err + " (file=" + filePath + ")";

        //     return "media_file ok: " + filePath;
        // });

        // Register("media_url", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: media_url <Url> [loop:0|1] [volume]";

        //     const std::string url = args[0];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 2)
        //         loop = ParseBoolArg(args[1], false);
        //     if (args.size() >= 3)
        //     {
        //         try { volume = std::stof(args[2]); } catch (...) { return "Invalid volume: " + args[2]; }
        //     }

        //     std::string err;
        //     if (!Mod::ModFeedback::PlayMediaUrl2D(url, loop, volume, &err))
        //         return "media_url failed: " + err + " (url=" + url + ")";

        //     return "media_url ok";
        // });

        // Register("media_file_attach", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.size() < 2)
        //         return "Usage: media_file_attach <actorSelector:player|npc_nearest|name-substring> <FilePath> [loop:0|1] [volume]";

        //     const std::string selector = args[0];
        //     const std::string filePath = args[1];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 3)
        //         loop = ParseBoolArg(args[2], false);
        //     if (args.size() >= 4)
        //     {
        //         try { volume = std::stof(args[3]); } catch (...) { return "Invalid volume: " + args[3]; }
        //     }

        //     std::string resolved;
        //     std::string err;
        //     if (!Mod::ModFeedback::PlayMediaFileAttached3D(selector, filePath, loop, volume, &resolved, &err))
        //         return "media_file_attach failed: " + err;

        //     return "media_file_attach ok: actor=" + resolved;
        // });

        // Register("media_url_attach", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.size() < 2)
        //         return "Usage: media_url_attach <actorSelector:player|npc_nearest|name-substring> <Url> [loop:0|1] [volume]";

        //     const std::string selector = args[0];
        //     const std::string url = args[1];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 3)
        //         loop = ParseBoolArg(args[2], false);
        //     if (args.size() >= 4)
        //     {
        //         try { volume = std::stof(args[3]); } catch (...) { return "Invalid volume: " + args[3]; }
        //     }

        //     std::string resolved;
        //     std::string err;
        //     if (!Mod::ModFeedback::PlayMediaUrlAttached3D(selector, url, loop, volume, &resolved, &err))
        //         return "media_url_attach failed: " + err;

        //     return "media_url_attach ok: actor=" + resolved;
        // });

        // Register("media_list", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     std::size_t maxEntries = 16;
        //     if (!args.empty())
        //     {
        //         try { maxEntries = static_cast<std::size_t>(std::stoul(args[0])); }
        //         catch (...) { return "Invalid maxEntries: " + args[0]; }
        //     }
        //     return Mod::ModFeedback::DescribeActiveMedia(maxEntries);
        // });

        // Register("media_stop", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     (void)args;
        //     const int stopped = Mod::ModFeedback::StopAllMedia();
        //     return "media_stop: closed " + std::to_string(stopped) + " player(s)";
        // });

        // Register("sound_groups_scan", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: sound_groups_scan <FolderPath>";

        //     std::string err;
        //     if (!Mod::ModFeedback::ScanSoundGroupsFromFolder(args[0], &err))
        //         return "sound_groups_scan failed: " + err;

        //     return Mod::ModFeedback::DescribeSoundGroups(20, 3);
        // });

        // Register("sound_groups_list", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;

        //     std::size_t maxGroups = 20;
        //     std::size_t maxEntries = 5;
        //     if (args.size() >= 1)
        //     {
        //         try { maxGroups = static_cast<std::size_t>(std::stoul(args[0])); }
        //         catch (...) { return "Invalid maxGroups: " + args[0]; }
        //     }
        //     if (args.size() >= 2)
        //     {
        //         try { maxEntries = static_cast<std::size_t>(std::stoul(args[1])); }
        //         catch (...) { return "Invalid maxEntries: " + args[1]; }
        //     }

        //     return Mod::ModFeedback::DescribeSoundGroups(maxGroups, maxEntries);
        // });

        // Register("playrandomsound", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
        //          {
        //     (void)world;
        //     if (args.empty())
        //         return "Usage: playrandomsound <GroupName> [loop:0|1] [volume]";

        //     const std::string groupName = args[0];
        //     bool loop = false;
        //     float volume = 1.0f;

        //     if (args.size() >= 2)
        //         loop = ParseBoolArg(args[1], false);
        //     if (args.size() >= 3)
        //     {
        //         try { volume = std::stof(args[2]); }
        //         catch (...) { return "Invalid volume: " + args[2]; }
        //     }

        //     std::string chosen;
        //     std::string err;
        //     if (!Mod::ModFeedback::PlayRandomSoundGroup2D(groupName, loop, volume, &chosen, &err))
        //         return "playrandomsound failed: " + err + " (group=" + groupName + ")";

        //     return "playrandomsound ok: " + chosen;
        // });

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

        // Portable light brightness (flashlight/headlamp): scales intensity for BPC_LightComp spot/point lights.
        Register("lights", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            if (args.empty())
            {
                std::ostringstream oss;
                oss << "lights: current scale=" << g_Cheats.GetPortableLightIntensityScale()
                    << ", fadeScale=" << g_Cheats.GetPortableLightFadeDistanceScale()
                    << " (usage: lights <scale> [fadeScale])";
                return oss.str();
            }

            float scale = 1.0f;
            try { scale = std::stof(args[0]); }
            catch (...) { return "Invalid scale value: " + args[0]; }

            g_Cheats.SetPortableLightIntensityScale(scale);

            if (args.size() >= 2)
            {
                float fadeScale = 1.0f;
                try { fadeScale = std::stof(args[1]); }
                catch (...) { return "Invalid fadeScale value: " + args[1]; }
                g_Cheats.SetPortableLightFadeDistanceScale(fadeScale);
            }

            std::ostringstream oss;
            oss << "lights: set scale=" << g_Cheats.GetPortableLightIntensityScale()
                << ", fadeScale=" << g_Cheats.GetPortableLightFadeDistanceScale();
            return oss.str();
        });

        // Test to call multiple cheat toggles at once to set up a specific state
        Register("test", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.SetGodMode(true);
            g_Cheats.SetUnlimitedAmmo(true);
            g_Cheats.SetHungerDisabled(true);
            g_Cheats.SetFatigueDisabled(true);
            g_Cheats.ToggleAnomaliesDisabled(); // replace this when we have a set command for anomalies.
            g_Cheats.ToggleAutoMag(); // ditto for auto-mag
            Mod::Friend::FriendSubsystem::Get()->SpawnFriend(world);
            g_Cheats.SetPortableLightIntensityScale(10.0f);
            return "Test setup applied: godmode, unlimited ammo, hunger/fatigue disabled, anomalies disabled, friend spawned, portable light scale set to 10" ;
        });




        // Access level: for testing content gated behind access levels without needing to meet requirements in-game.
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


        Register("debug", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleDebugMode();
            return g_Cheats.GetStatus();
        });

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

        // // Short alias for apply
        // Register("equip", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
        //          { return HandleApplyLoadout(world, args); });

        LOG_INFO("[Command] Simplified Arena commands initialized");

        // -----------------------------------------------------------------
        // Friend NPC commands
        // -----------------------------------------------------------------
        Register("friend", [this](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            if (!world)
                return "friend: world not ready";
            return Mod::Friend::FriendSubsystem::Get()->SpawnFriend(world);
        });

        Register("friend_clear", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            if (!world)
                return "friend_clear: world not ready";
            Mod::Friend::FriendSubsystem::Get()->ClearAll(world);
            return "All friends cleared";
        });

        Register("friend_status", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world; (void)args;
            const int count = Mod::Friend::FriendSubsystem::Get()->ActiveFriendCount();
            return std::string("Active friends: ") + std::to_string(count)
                + " / " + std::to_string(Mod::Tuning::kFriendMaxCount);
        });

        // -----------------------------------------------------------------
        // Anomaly disable cheat
        // -----------------------------------------------------------------
        Register("anomalies", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)args;
            g_Cheats.ToggleAnomaliesDisabled();
            return g_Cheats.GetStatus();
        });

        // -----------------------------------------------------------------
        // Environment prune debug cheats
        // -----------------------------------------------------------------
        Register("plants", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            if (!world)
                return "plants: world not ready";

            const float defaultRadius = Cheats::kDefaultEnvironmentPruneRadius;
            const float defaultInterval = Cheats::kDefaultEnvironmentPruneIntervalSeconds;

            if (!args.empty())
            {
                std::string mode = args[0];
                std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                if (mode == "once")
                {
                    float radius = defaultRadius;
                    if (args.size() >= 2 && !TryParseFloatArg(args[1], radius))
                        return "plants: invalid radius: " + args[1];

                    return g_Cheats.RemovePlantsBushesOnce(world, radius);
                }
            }

            float radius = defaultRadius;
            float interval = defaultInterval;

            if (args.size() >= 1)
            {
                if (!TryParseFloatArg(args[0], radius))
                    return "plants: invalid radius: " + args[0] + " (usage: plants | plants once [radius] | plants [radius] [intervalSec])";
            }
            if (args.size() >= 2)
            {
                if (!TryParseFloatArg(args[1], interval))
                    return "plants: invalid intervalSec: " + args[1];
            }

            return g_Cheats.TogglePlantsBushesPersistent(radius, interval);
        });

        Register("trees", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            if (!world)
                return "trees: world not ready";

            const float defaultRadius = Cheats::kDefaultEnvironmentPruneRadius;
            const float defaultInterval = Cheats::kDefaultEnvironmentPruneIntervalSeconds;

            if (!args.empty())
            {
                std::string mode = args[0];
                std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
                if (mode == "once")
                {
                    float radius = defaultRadius;
                    if (args.size() >= 2 && !TryParseFloatArg(args[1], radius))
                        return "trees: invalid radius: " + args[1];

                    return g_Cheats.RemoveTreesOnce(world, radius);
                }
            }

            float radius = defaultRadius;
            float interval = defaultInterval;

            if (args.size() >= 1)
            {
                if (!TryParseFloatArg(args[0], radius))
                    return "trees: invalid radius: " + args[0] + " (usage: trees | trees once [radius] | trees [radius] [intervalSec])";
            }
            if (args.size() >= 2)
            {
                if (!TryParseFloatArg(args[1], interval))
                    return "trees: invalid intervalSec: " + args[1];
            }

            return g_Cheats.ToggleTreesPersistent(radius, interval);
        });

        // -----------------------------------------------------------------
        // AutoMag: magazines placed in mag pouches auto-refill
        // -----------------------------------------------------------------
        Register("automag", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
                 {
            (void)world;
            (void)args;
            g_Cheats.ToggleAutoMag();
            return g_Cheats.GetStatus();
        });

        // -----------------------------------------------------------------
        // Heal item (spawn QuickHeal near player)
        // -----------------------------------------------------------------
        // Register("heal", [](SDK::UWorld *world, const std::vector<std::string> &args) -> std::string
        //          {
        //     (void)args;
        //     if (!world)
        //         return "heal: world not ready";
        //     return g_Cheats.SpawnHealItem(world);
        // });

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
