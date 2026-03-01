# Modding Guide

How to extend and modify the Battle Arena Mod codebase.

## Prerequisites

- Visual Studio 2019/2022 with C++ workload
- Windows SDK (10.0+)
- Understanding of C++17
- Basic knowledge of Unreal Engine concepts

## Project Structure

```
a battle arena mod for itr2/
├── dllmain.cpp              # DLL entry point
├── pch.h / pch.cpp          # Precompiled headers
├── CppSDK/                  # Generated Unreal SDK
│   ├── SDK.hpp              # Main include
│   └── SDK/                 # Type definitions
├── Mod/                     # All mod code
│   ├── ModMain.cpp/.hpp     # Main loop
│   ├── *Subsystem.cpp/.hpp  # Feature modules
│   └── ...
├── Loadouts/                # Saved loadout files
├── sounds/                  # Custom audio files
└── docs/                    # This documentation
```

## Building

### Open the Solution

```
a battle arena mod for itr2.sln
```

### Build Configuration

| Config | Use |
|--------|-----|
| Debug | Development, full logging |
| Release | Production, optimized |

### Output

Build produces `battlearena.dll` in:
- Debug: `x64/Debug/`
- Release: `x64/Release/`

Copy to game's `Mods/` folder for testing.

## Adding a New Feature

### Step 1: Create Subsystem Files

```cpp
// Mod/MyFeatureSubsystem.hpp
#pragma once

#include <string>
#include "..\CppSDK\SDK.hpp"

namespace Mod::MyFeature
{
    class MyFeatureSubsystem
    {
    public:
        static MyFeatureSubsystem* Get();
        
        void Initialize();
        void Shutdown();
        void Update(SDK::UWorld* world);
        
        // Feature-specific methods
        std::string DoSomething(SDK::UWorld* world, int param);
        
    private:
        static MyFeatureSubsystem* s_instance;
        bool initialized_ = false;
        // State variables
    };
}
```

```cpp
// Mod/MyFeatureSubsystem.cpp
#include "MyFeatureSubsystem.hpp"
#include "Logging.hpp"

namespace Mod::MyFeature
{
    static MyFeatureSubsystem* s_instance = nullptr;
    
    MyFeatureSubsystem* MyFeatureSubsystem::Get()
    {
        if (!s_instance) {
            s_instance = new MyFeatureSubsystem();
        }
        return s_instance;
    }
    
    void MyFeatureSubsystem::Initialize()
    {
        if (initialized_) return;
        LOG_INFO("[MyFeature] Initializing...");
        // Setup
        initialized_ = true;
    }
    
    void MyFeatureSubsystem::Update(SDK::UWorld* world)
    {
        if (!initialized_ || !world) return;
        // Per-tick logic
    }
}
```

### Step 2: Add to ModMain

In `ModMain.cpp`:

```cpp
#include "MyFeatureSubsystem.hpp"

// In OnTick:
void ModMain::OnTick(SDK::UWorld* world)
{
    // ... existing code ...
    
    // Update your subsystem
    Mod::MyFeature::MyFeatureSubsystem::Get()->Update(world);
}
```

### Step 3: Add Commands

In `CommandHandler.cpp`, in `InitializeDefaults()`:

```cpp
// Import header
#include "MyFeatureSubsystem.hpp"

// Register command
Register("mycommand", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
{
    if (args.empty()) {
        return "Usage: mycommand <param>";
    }
    
    int param = 0;
    try {
        param = std::stoi(args[0]);
    } catch (...) {
        return "Invalid param: " + args[0];
    }
    
    return Mod::MyFeature::MyFeatureSubsystem::Get()->DoSomething(world, param);
});
```

### Step 4: Add VR Menu Entry (Optional)

In `VRMenuSubsystem.cpp`, modify the menu page definitions:

```cpp
std::vector<VRMenuItem> GetMainMenuItems()
{
    return {
        // ... existing items ...
        {
            "My Feature",
            nullptr,  // No status display
            []() { /* action */ },
            false
        },
    };
}
```

## Adding a New Cheat

### Step 1: Add State to Cheats

In `Cheats.hpp`:

```cpp
class Cheats {
    // Add state
    std::atomic<bool> myCheatActive_{false};
    
public:
    void ToggleMyCheat();
    bool IsMyCheatActive() const;
};
```

### Step 2: Implement Toggle

In `Cheats.cpp`:

```cpp
void Cheats::ToggleMyCheat()
{
    bool newState = !myCheatActive_.exchange(!myCheatActive_.load());
    LOG_INFO("[Cheats] MyCheat: " << (newState ? "ON" : "OFF"));
}

bool Cheats::IsMyCheatActive() const
{
    return myCheatActive_.load(std::memory_order_relaxed);
}
```

### Step 3: Add Update Logic

In `Cheats::Update()`:

```cpp
void Cheats::Update(SDK::UWorld* world)
{
    // ... existing code ...
    
    if (myCheatActive_) {
        ApplyMyCheat(world, player);
    }
}

void Cheats::ApplyMyCheat(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player)
{
    Mod::ScopedProcessEventGuard guard;  // Prevent recursion
    
    // Your cheat logic
}
```

### Step 4: Add Command

In `CommandHandler.cpp`:

```cpp
Register("mycheat", [](SDK::UWorld* world, const std::vector<std::string>& args) -> std::string
{
    (void)world;
    (void)args;
    g_Cheats.ToggleMyCheat();
    return g_Cheats.GetStatus();
});
```

### Step 5: Update Status String

In `Cheats::GetStatus()`:

```cpp
std::string Cheats::GetStatus() const
{
    std::ostringstream ss;
    ss << "GodMode: " << (godModeActive_ ? "ON" : "OFF");
    // ... existing ...
    ss << " | MyCheat: " << (myCheatActive_ ? "ON" : "OFF");
    return ss.str();
}
```

## Adding a ProcessEvent Hook

### Step 1: Identify Target Function

Use tracing to find the function:

```
trace_on PartialName
```

### Step 2: Define Parameters Struct

Find the function signature in the SDK or through tracing:

```cpp
// If function is: void MyTargetFunc(float Value, bool Flag)
struct MyTargetFunc_Params {
    float Value;
    bool Flag;
    uint8_t Pad[3];  // Alignment padding
};
```

### Step 3: Register the Hook

In your subsystem or in `HookManager.cpp`:

```cpp
void MyFeatureSubsystem::Initialize()
{
    HookManager::Get().RegisterNamedHook("MyTargetFunc",
        [](SDK::UObject* obj, SDK::UFunction* fn, void* parms, 
           HookManager::ProcessEventFn orig) -> bool
        {
            auto* params = static_cast<MyTargetFunc_Params*>(parms);
            
            // Read/modify parameters
            LOG_INFO("[MyHook] Value=" << params->Value);
            
            // Optionally modify
            params->Value *= 2.0f;
            
            // Call original (usually)
            orig(obj, fn, parms);
            
            // Return true only to fully suppress original
            return false;
        });
}
```

## Working with the SDK

### Finding Objects

```cpp
// Iterate loaded objects
for (int i = 0; i < SDK::UObject::GObjects->Num(); ++i)
{
    SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
    if (!obj) continue;
    
    // Check class
    if (obj->IsA<SDK::ARadiusNPCCharacterBase>())
    {
        auto* npc = static_cast<SDK::ARadiusNPCCharacterBase*>(obj);
        // Use npc
    }
}
```

### Spawning Actors

```cpp
SDK::FActorSpawnParameters spawnParams;
spawnParams.SpawnCollisionHandlingOverride = 
    SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

SDK::AActor* actor = world->SpawnActor(
    actorClass,
    &spawnLocation,
    &spawnRotation,
    spawnParams);
```

### Calling Blueprint Functions

```cpp
// If the function exists in the class
myActor->SomeBlueprintFunction(arg1, arg2);

// Using ProcessEvent for non-exposed functions
struct Params { int Arg1; float Arg2; };
Params p = { 42, 3.14f };

SDK::UFunction* fn = myActor->Class->FindFunctionByName("UnexposedFunc");
if (fn) {
    myActor->ProcessEvent(fn, &p);
}
```

### Getting Subsystems

```cpp
// World subsystem
auto* gameInstance = SDK::UGameplayStatics::GetGameInstance(world);
auto* mySub = gameInstance->GetSubsystem<SDK::UMySubsystemClass>();

// Player subsystem
auto* controller = SDK::UGameplayStatics::GetPlayerController(world, 0);
auto* mySub = controller->GetLocalPlayer()->GetSubsystem<SDK::UMySubsystemClass>();
```

## Logging

Use the provided logging macros:

```cpp
#include "Logging.hpp"

LOG_INFO("[MyFeature] Info message: " << value);
LOG_WARN("[MyFeature] Warning: " << message);
LOG_ERROR("[MyFeature] Error occurred: " << error);
```

Log output goes to `%USERPROFILE%\AppData\Local\ITR2Mod\mod.log`.

## Thread Safety

### Rules

1. **SDK calls only on game thread** (in OnTick or hooks)
2. **Use atomics for shared state**:
   ```cpp
   std::atomic<bool> enabled_{false};
   std::atomic<float> value_{1.0f};
   ```
3. **Use mutex for complex shared state**:
   ```cpp
   mutable std::mutex mutex_;
   std::vector<Entry> entries_;
   
   void AddEntry(Entry e) {
       std::lock_guard<std::mutex> lock(mutex_);
       entries_.push_back(e);
   }
   ```

### CommandQueue Pattern

TCP commands are enqueued from background thread, processed on game thread:

```cpp
// Background thread (TCP receive)
commandQueue->PushCommand(cmd);

// Game thread (OnTick)
std::vector<std::string> commands = commandQueue->Drain();
for (const auto& cmd : commands) {
    Handle(cmd);
}
```

## Testing

### In-Game Testing

1. Build the mod
2. Copy DLL to `Mods/`
3. Launch game
4. Connect: `telnet localhost 7777`
5. Test commands

### Iteration Tips

- Keep the game running during code changes
- Restart only when changing hooks or main loop
- Use the log file to track issues
- Enable `debug` mode for extra logging

### Common Issues

| Issue | Cause | Solution |
|-------|-------|----------|
| Game hangs | Infinite recursion | Add ScopedProcessEventGuard |
| Crash on level change | Stale pointers | Clear caches in OnTick |
| Command no response | Not on game thread | Ensure SDK calls in OnTick |
| Hook not firing | Wrong function name | Use tracing to verify |

## Code Style

### Naming

- Classes: `PascalCase`
- Methods: `PascalCase`
- Member variables: `camelCase_` (trailing underscore)
- Local variables: `camelCase`
- Constants: `kPascalCase`

### Files

- Header: `.hpp`
- Implementation: `.cpp`
- One class per file (generally)

### Comments

Use `AILEARNINGS` blocks for important lessons:

```cpp
/*
AILEARNINGS
- Description of what went wrong and how it was fixed
- Another lesson learned
*/
```

These help AI assistants avoid repeating mistakes.
