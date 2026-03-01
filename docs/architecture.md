# Architecture Overview

Technical architecture of the Battle Arena Mod for ITR2.

## High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        DLL Entry Point                          │
│                         (dllmain.cpp)                           │
└──────────────────────────────┬──────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                          ModMain                                │
│  - Main loop                                                    │
│  - Single-instance mutex                                        │
│  - System initialization                                        │
└──────────────────────────────┬──────────────────────────────────┘
                               │
         ┌─────────────────────┼─────────────────────┐
         │                     │                     │
         ▼                     ▼                     ▼
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐
│   HookManager   │  │   TcpServer     │  │ CommandHandlerReg.  │
│  - VTable patch │  │  - Port 7777    │  │ - Command dispatch  │
│  - ProcessEvent │  │  - CommandQueue │  │ - Handlers          │
└────────┬────────┘  └────────┬────────┘  └──────────┬──────────┘
         │                    │                      │
         │                    └──────────┬───────────┘
         │                               │
         ▼                               ▼
┌─────────────────────────────────────────────────────────────────┐
│                         OnTick Loop                             │
│  - Process TCP commands                                         │
│  - Update subsystems                                            │
│  - Apply cheats                                                 │
└──────────────────────────────┬──────────────────────────────────┘
                               │
    ┌──────────────┬───────────┼───────────┬───────────┬─────────┐
    │              │           │           │           │         │
    ▼              ▼           ▼           ▼           ▼         ▼
┌────────┐  ┌──────────┐  ┌────────┐  ┌────────┐  ┌────────┐  ┌───────┐
│ Arena  │  │  Friend  │  │ Cheats │  │Loadout │  │ VRMenu │  │  AI   │
│Subsys. │  │ Subsys.  │  │        │  │Subsys. │  │Subsys. │  │Subsys.│
└────────┘  └──────────┘  └────────┘  └────────┘  └────────┘  └───────┘
```

## Component Overview

### Entry Point (`dllmain.cpp`)

The DLL entry handles Windows loader callbacks:

```cpp
DllMain(DLL_PROCESS_ATTACH)
    ├── DisableThreadLibraryCalls()  // Critical: prevents callback chaos
    └── CreateThread() → ModMain::Run()
```

**Critical Learnings:**
- Never call `MessageBoxA` or UI functions from `DllMain` (deadlock risk)
- Always call `DisableThreadLibraryCalls` to prevent per-thread callbacks

### ModMain (`ModMain.cpp/.hpp`)

Central orchestrator that:
1. Initializes logging
2. Acquires single-instance mutex (prevents double-load issues)
3. Installs ProcessEvent hook
4. Initializes subsystems
5. Creates TCP server
6. Runs main background loop

**OnTick Path:**
```
ModMain::OnTick(world)
    ├── Level change detection
    │   ├── Clear caches
    │   ├── Deactivate cheats
    │   └── Stop arena
    ├── Process TCP commands
    │   ├── CommandQueue::Drain()
    │   └── CommandHandlerRegistry::Handle()
    ├── DrainPending feedback
    ├── Update Cheats
    ├── Update ArenaSubsystem
    ├── Update FriendSubsystem
    └── Update VRMenuSubsystem
```

### HookManager (`HookManager.cpp/.hpp`)

Implements ProcessEvent interception via VTable patching:

```cpp
HookManager
    ├── Initialize() - Prepare hook infrastructure
    ├── InstallProcessEventHook() - Patch UObject VTable
    │   ├── Find ProcessEvent slot in VTable
    │   ├── Save original pointer
    │   └── Write detour address
    ├── ProcessEventDetour() - The hook function
    │   ├── Named hook dispatch
    │   ├── Cheat hooks (ammo, durability)
    │   ├── Tracing (if enabled)
    │   └── Call original
    └── RegisterNamedHook() - Add function-specific handlers
```

**Hook Types:**
- **Cheats**: Intercept weapon fire, damage, consumption
- **Named**: VR input actions, UI events
- **Tracing**: Debug event logging

### Command System

```
TcpServer                  CommandQueue               CommandHandlerRegistry
    │                           │                           │
    ├── Accept()                │                           │
    │   └── recv()              │                           │
    │       └── PushCommand() ──┼→ commands_[]              │
    │                           │                           │
    │                           │   ←── Drain() ────────────┼─── OnTick
    │                           │                           │
    │                           │                     Handle(command)
    │                           │                           │
    │       PushResponse() ←────┼───────────────────────────┤
    │           │               │                           │
    └── send() ←┘               │                           │
```

### Subsystems

Each subsystem follows a consistent pattern:

```cpp
class Subsystem {
public:
    static Subsystem* Get();     // Singleton access
    void Initialize();           // One-time setup
    void Update(UWorld* world);  // Per-tick logic
    void Shutdown();             // Cleanup
};
```

---

## Threading Model

```
┌──────────────────────────────────────────────────────────────┐
│                      Main Game Thread                         │
│  - Unreal Engine tick                                         │
│  - ProcessEvent calls                                         │
│  - Our OnTick callback                                        │
│  - All SDK/Engine calls                                       │
└──────────────────────────────────────────────────────────────┘
                               │
                               │ Synchronized via
                               │ CommandQueue
                               │
┌──────────────────────────────▼───────────────────────────────┐
│                     Mod Background Thread                     │
│  - Created by DllMain                                         │
│  - TCP server accept loop                                     │
│  - Reads commands from network                                │
│  - Pushes to CommandQueue                                     │
└──────────────────────────────────────────────────────────────┘
```

**Thread Safety:**
- SDK calls ONLY from OnTick (game thread)
- CommandQueue uses mutex for thread-safe push/drain
- Subsystem state uses atomic/mutex where needed

---

## Memory Management

### Pointer Validity

Game pointers become invalid on level transitions:

```cpp
// BAD: Cache across levels
static SDK::ACharacter* g_Player = nullptr;

// GOOD: Use GameContext with level-change cache clearing
GameContext::ClearCache();  // Called on level change
```

### SDK Object Lifetime

```
UObject Lifecycle:
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│   Created   │────▶│   Active    │────▶│  Destroyed  │
└─────────────┘     └─────────────┘     └─────────────┘
                           │
                    IsValid() check
                    required before use
```

Always validate with `UKismetSystemLibrary::IsValid(actor)` before use.

---

## File I/O

### Loadout Files

```
Loadouts/
├── name.loadout   # INI-like format
└── ...

Format:
[LOADOUT]
name=...
timestamp=...
version=1

[ITEM]
type=Item.GameplayTag
uid=unique-id
parent=container-id
durability=100
...
[/ITEM]
```

### NPC Database

```
npcs.txt
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_...
/Game/ITR2/BPs/AI/Enemies/Mimic/BP_...
```

Full Unreal object paths for asset loading.

### Sound Groups

```
sounds/
├── groupname/
│   ├── sound1.wav
│   └── sound2.mp3
├── ambient.txt       # Lines are file paths or URLs
└── ...
```

---

## SDK Integration

The mod uses a generated SDK (Dumper-7 style):

```cpp
#include "CppSDK/SDK.hpp"

// Global object array
SDK::UObject::GObjects->Num()

// World access
SDK::UWorld::GetWorld()

// Subsystem access
auto* subsystem = SDK::UGameInstance::GetSubsystem<T>()

// Spawning
SDK::UWorld::SpawnActor()
```

**SDK Limitations:**
- No `NewObject` / `StaticConstructObject_Internal` exposed
- No widget construction from scratch
- Blueprint classes need special loading

---

## Build Configuration

### Visual Studio Project

- C++17 or later
- Windows SDK
- No external dependencies beyond SDK

### Output

- `battlearena.dll` - Main mod DLL
- Place in `Win64/Mods/`

### Debug vs Release

- Debug: Full logging, assertions
- Release: Optimized, reduced logging

---

## Extension Points

### Adding a New Command

```cpp
// In CommandHandler.cpp, InitializeDefaults()
Register("mycommand", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
{
    // Implementation
    return "Result message";
});
```

### Adding a New Cheat

1. Add state in `Cheats.hpp`
2. Add toggle/update in `Cheats.cpp`
3. Add command in `CommandHandler.cpp`
4. Add ProcessEvent hook if needed in `HookManager.cpp`

### Adding a New Subsystem

1. Create `NewSubsystem.hpp/.cpp`
2. Implement singleton pattern
3. Add initialization in `CommandHandlerRegistry` constructor
4. Add `Update()` call in `ModMain::OnTick()`
