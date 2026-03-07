// AI SHOULD:
// ALWAYS INCLUDE ALL LEARNINGS IN THIS AREA. 
//  A learning isn't just describing what this code does, it's when you've stuffed up and had to change it.
//
// THIS IS FOR A VR GAME MOD FOR INTO THE RADIUS 2
// EVERY TIME I HAVE TO COMPILE AND TEST, IT'S A PAIN IN THE NECK.
//
// AILEARNINGS HERE:
// - Header Forward Declarations: When using SDK types in ModMain.hpp, use "namespace SDK { class UWorld; }" forward declarations instead of full SDK includes to avoid "SDK is not a namespace" errors in the header.
// - SDK::UObject::GObjects is a TUObjectArrayWrapper. You MUST use '->' to access Num() (e.g. GObjects->Num()), as '.' will fail compilation.
// - TArray is a container object. Access members with '.', not '->' (e.g. LocalPlayers.Num(), not LocalPlayers->Num()).
// - Level Change: SDK::UWorld does not have GetPackage() in this generator version. Use world->GetName() for stable level-change detection.
// - Level Change: When a level changes, cached pointers (like the player character) become invalid. Always clear caches (e.g., GameContext::ClearCache()) on level change to prevent silent failures in systems like ModFeedback.
// - Hook Retry Condition: Do NOT gate the ProcessEvent hook retry on UWorld::GetWorld(). The world is null until the first map loads, but GObjects (and the VTables to patch) are populated well before that. Requiring 'world != null' caused the retry to silently never fire, especially in the split-DLL setup where the mod is loaded via LoadLibraryA early in process startup. Only check GObjects->Num() > 1000.
// - DllMain MessageBoxA DEADLOCK: NEVER call MessageBoxA (or any window/UI function) from DllMain callbacks
//   (DLL_THREAD_ATTACH, DLL_THREAD_DETACH, DLL_PROCESS_DETACH) or from functions called by the modloader
//   while it still holds the loader lock (e.g. GetModSDKVersion). MessageBoxA runs a message pump which
//   can deadlock against the loader lock.
// - DisableThreadLibraryCalls: ALWAYS call DisableThreadLibraryCalls(hModule) in DLL_PROCESS_ATTACH in the
//   mod's DllMain. Without it, DllMain fires for every thread the game creates with DLL_THREAD_ATTACH.
//   Any non-trivial callback body will cause hangs or repeated popups for every game thread.
// - Background thread AV / C2712: __try cannot be used in a function that has C++ objects requiring
//   unwinding (e.g. CommandHandlerRegistry, CommandQueue, TcpServer in Run()). Extracting the SEH block
//   into a plain helper function (no C++ objects, no destructors) fixes C2712. See SafeGetGObjectsNum().
// - Split-DLL double-load breaks TCP commands: If the mod DLL is loaded twice in the same process, one
//   instance can own the TCP server/CommandQueue while the other instance owns the ProcessEvent hook and
//   tick loop. Symptom: telnet connects but commands get no responses. Fix: add a PID-scoped named mutex
//   guard so only one ModMain thread runs per process, and log PID/TID + module path.
// - Crash/duplicate-events symptom: If you see duplicated level-change logs milliseconds apart (e.g. L_Startup -> L_MainMenu twice), assume the mod is double-loaded.
//   Fix: enforce the PID-scoped named mutex guard in ModMain::Run so the second instance exits immediately.
// - AutoMag deferred retries: REMOVED. The retry queue never made progress because Max=26 is a hard UE array wall
//   that persists across all retries. Replaced with a direct call to AddItemToStackByTag via ProcessEvent inside
//   AutoMag_FillMagazine itself — this uses UE's own TArray growth path (26→60) and is executed synchronously.
// - Menu widget height clipping: UpdateWidgetDrawSize() now sets SetPivot(0.5, 0.95) so the widget
//   anchors near the bottom and extends upward, and K2_SetRelativeLocation adds a per-item Z lift.
//


// There's so much unnecessary stuff I'd be hooking or calling every loop in this thing right now...I'll fix it later ;)


#include "ModMain.hpp"

#include <chrono>
#include <thread>
#include <string>

#include "Logging.hpp"
#include "CommandQueue.hpp"
#include "TcpServer.hpp"
#include "CommandHandler.hpp"
#include "Cheats.hpp"
#include "HookManager.hpp"
#include "RuntimeState.hpp"
#include "ArenaSubsystem.hpp"
#include "LoadoutSubsystem.hpp"
#include "GameContext.hpp"
#include "ModFeedback.hpp"
#include "ModTuning.hpp"
#include "FriendSubsystem.hpp"
#include "HandWidgetTestHarness.hpp"
#include "..\CppSDK\SDK.hpp"


namespace Mod
{
    namespace
    {
		static HANDLE gSingleInstanceMutex = nullptr;

        void InitializeLogging()
        {
            Mod::Logger::Get().Initialize();
            LOG_INFO("[mod] Log file: " << Mod::Logger::Get().GetPath());
        }

        static std::string GetThisModulePath(HMODULE hModule)
        {
            char dllPath[MAX_PATH] = {};
            DWORD len = GetModuleFileNameA(hModule, dllPath, MAX_PATH);
            if (len == 0 || len >= MAX_PATH)
                return std::string();
            return std::string(dllPath);
        }

        // Extracted into its own plain function because __try/__except cannot coexist with
        // C++ object unwinding (C2712). Run() has destructible locals so __try is banned there.
        // Returns the GObjects count, or -1 if a structured exception (e.g. AV) was caught.
        static int SafeGetGObjectsNum()
        {
            __try
            {
                return SDK::UObject::GObjects->Num();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }
    }
   DWORD ModMain::Run(HMODULE moduleHandle)
    {
        (void)moduleHandle;

        // Initialize logging first
        InitializeLogging();
        LOG_INFO("[mod] Starting mod main loop.");

        // Guard against split-DLL/double-load: only allow one mod main loop per process.
        // If two instances run, you'll see duplicated level-change logs and can get hard-to-debug crashes.
        if (!gSingleInstanceMutex)
        {
            const DWORD pid = GetCurrentProcessId();
            char nameBuf[128] = {};
            snprintf(nameBuf, sizeof(nameBuf), "ITR2_BattleArenaMod_Singleton_%lu", (unsigned long)pid);
            gSingleInstanceMutex = CreateMutexA(nullptr, TRUE, nameBuf);
            const DWORD lastErr = GetLastError();
            if (!gSingleInstanceMutex || lastErr == ERROR_ALREADY_EXISTS)
            {
                LOG_WARN("[mod] Another mod instance is already running (pid=" << pid << "); exiting this instance's Run() thread");
                return 0;
            }
            LOG_INFO("[mod] Single-instance mutex acquired (pid=" << pid << ")");
        }

        // Initialize hook manager
        HookManager::Get().Initialize();
        HookManager::Get().InstallProcessEventHook();

        // Initialize loadout subsystem
        Loadout::LoadoutSubsystem::Get()->Initialize();

        // Initialize sound system (scan sounds/ folder if present).
        Mod::ModFeedback::InitSoundSystem();

        // Initialize portable hand widget test harness
        PortableWidget::HandWidgetTestHarness::Get()->Initialize();
        LOG_INFO("[mod] HandWidget test harness initialized");

        // Create command handler with all commands
        CommandHandlerRegistry commandHandler;
        commandHandler.InitializeDefaults();

        // Register hand widget test harness commands
        PortableWidget::HandWidgetTestHarness::Get()->RegisterCommands(commandHandler);

        // Create TCP server
        CommandQueue commandQueue;
        TcpServer server;

        // Expose live instances to ProcessEvent hooks (cheat panel -> enqueue commands)
        RuntimeState::SetCommandHandlerRegistry(&commandHandler);
        RuntimeState::SetCommandQueue(&commandQueue);


        // Main background loop
        while (true)
        {
            // Just keep the thread alive for the TCP server
            // Commands are now processed on the game thread via the HookManager tick
            
            // If hooks haven't been installed yet (e.g. failed at startup), try to install them once world is valid
            if (!HookManager::Get().IsProcessEventHooked())
            {
                SDK::UEngine* engine = SDK::UEngine::GetEngine();
                SDK::UWorld* world = SDK::UWorld::GetWorld();
                if (engine && world && SDK::UObject::GObjects && SDK::UObject::GObjects->Num() > 1000) // Arbitrary "ready" threshold
                {
                    LOG_INFO("[mod] GObjects looks ready (" << SDK::UObject::GObjects->Num() << " objects), retrying hook installation...");
                    HookManager::Get().InstallProcessEventHook();
                    
                    // moved to here from before the while loop, because if the process isn't hooked, the tick loop that processes TCP commands won't run, so the retry would never fire. If for some reason 2 instances of this are loaded and the first one is non-functional, it won't ever hook and so won't start the server, making sure it's not in the way of a functional instance.
                    if (!server.Start(Mod::Tuning::kTcpDefaultPort, &commandQueue))
                    {
                        LOG_ERROR("[mod] Failed to start TCP server on port " << Mod::Tuning::kTcpDefaultPort);
                    }
                    else
                    {
                        LOG_INFO("[mod] TCP server started on port " << Mod::Tuning::kTcpDefaultPort);
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }

        return 0;
    }
    void ModMain::OnTick(SDK::UWorld* world)
    {
        // NOTE: world can be null in main menu / during loading.
        // We still want to service TCP commands so clients get responses.

        if (world)
        {
            static std::string lastWorldName = "";
            std::string currentWorldName = world->GetName();

            if (lastWorldName != "" && currentWorldName != "" && lastWorldName != currentWorldName)
            {
                LOG_INFO("[mod] Level change detected from " << lastWorldName << " to " << currentWorldName);

                // Clear cached player/controller references
                GameContext::ClearCache();

                // Shutdown portable hand widgets on level change (stale UObject pointers)
                PortableWidget::HandWidgetTestHarness::Get()->Shutdown();
                PortableWidget::HandWidgetTestHarness::Get()->Initialize();

                // Deactivate cheats
                Cheats* cheats = GetCheats();
                if (cheats)
                {
                    cheats->DeactivateAll();
                }

                // Stop arena
                auto* arena = Arena::ArenaSubsystem::Get();
                if (arena)
                {
                    arena->Stop();
                }
            }
            lastWorldName = currentWorldName;
        }

        // Get commands from TCP server
        auto* commandQueue = RuntimeState::GetCommandQueue();
        auto* commandHandler = RuntimeState::GetCommandHandlerRegistry();

        if (commandQueue && commandHandler)
        {
            std::vector<std::string> commands = commandQueue->Drain();

            // Process each command through the handler
            for (const std::string& command : commands)
            {
                std::string response = commandHandler->Handle(world, command);
                if (!response.empty())
                {
                    commandQueue->PushResponse(response);
                    LOG_INFO("[mod] Command response: " << response);
                }
            }
        }

        if (world)
        {
            // Replay any mod feedback that was suppressed during Startup/MainMenu.
            // (Avoids crashes from emitting subtitles too early, while still delivering the message once safe.)
            Mod::ModFeedback::DrainPending();

            // Update cheats
            Cheats* cheats = GetCheats();
            if (cheats)
            {
                cheats->Update(world);
            }

            // Update Arena
            auto* arena = Arena::ArenaSubsystem::Get();
            if (arena)
            {
                arena->Update(world);
            }

            // Update Friend subsystem
            Mod::Friend::FriendSubsystem::Get()->Update(world);

            // Update portable hand widget test harness
            PortableWidget::HandWidgetTestHarness::Get()->Tick(world);
        }
    }
}

