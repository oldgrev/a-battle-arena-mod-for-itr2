# AI Agent Guidelines

Guidelines and critical learnings for AI coding assistants working on this codebase.

## Overview

This document consolidates learnings from development to help AI agents avoid common pitfalls and work effectively with this codebase.

---

## Critical Rules

### 1. NEVER Use MessageBoxA in DllMain Context

```cpp
// ❌ FATAL: Will deadlock
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    MessageBoxA(nullptr, "Debug", "Title", MB_OK);  // DEADLOCK
    return TRUE;
}

// ❌ ALSO FATAL: Functions called from DllMain
extern "C" __declspec(dllexport) const char* GetModSDKVersion() {
    MessageBoxA(nullptr, "Version check", "Title", MB_OK);  // DEADLOCK
    return "1.0.0";
}
```

**Why:** MessageBoxA runs a message pump that can deadlock against the loader lock.

**Fix:** Use logging instead, or defer UI to after initialization.

### 2. ALWAYS Call DisableThreadLibraryCalls

```cpp
// ✅ REQUIRED in DLL_PROCESS_ATTACH
case DLL_PROCESS_ATTACH:
    DisableThreadLibraryCalls(hModule);
    // ...
```

**Why:** Without this, DllMain fires for every thread the game creates, causing hangs and repeated initialization.

### 3. ALWAYS Use ScopedProcessEventGuard

```cpp
// ❌ WRONG: SDK calls may trigger ProcessEvent recursively
void MyFunction() {
    SDK::UKismetSystemLibrary::PrintString(world, message);
}

// ✅ CORRECT: Guard prevents infinite recursion
void MyFunction() {
    Mod::ScopedProcessEventGuard guard;
    SDK::UKismetSystemLibrary::PrintString(world, message);
}
```

**Why:** SDK functions often internally call ProcessEvent, causing infinite recursion in hooks.

### 4. Clear Caches on Level Change

```cpp
// ❌ WRONG: Pointer becomes invalid after level change
static SDK::ACharacter* g_CachedPlayer = nullptr;

// ✅ CORRECT: Clear on level change
void ModMain::OnTick(SDK::UWorld* world) {
    if (levelChanged) {
        GameContext::ClearCache();
        // All subsystems must also clear their caches
    }
}
```

**Why:** Level transitions invalidate all actor/object pointers.

### 5. SDK Member Access Syntax

```cpp
// ✅ GObjects is a pointer wrapper - use ->
SDK::UObject::GObjects->Num()

// ✅ TArray is a value type - use .
TArray<SDK::ULocalPlayer*> LocalPlayers;
LocalPlayers.Num()  // Correct
LocalPlayers->Num() // ❌ WRONG
```

---

## AILEARNINGS Convention

Throughout the codebase, look for `AILEARNINGS` comment blocks. These document specific lessons learned:

```cpp
/*
AILEARNINGS
- Description of what went wrong 
- How it was fixed
- What to avoid in the future
*/
```

**Always read these before modifying a file.** They contain critical context.

### Key AILEARNINGS by File

#### dllmain.cpp
- MessageBoxA deadlock in DllMain
- DisableThreadLibraryCalls requirement

#### ModMain.cpp
- Forward declarations for SDK types in headers
- GObjects access syntax (-> not .)
- Level change cache invalidation
- Hook retry should check GObjects, not World
- Split-DLL double-load issues
- SEH and C++ object unwinding incompatibility

#### ArenaSubsystem.cpp
- std::recursive_mutex required for SDK callback safety
- TArray member access through TArrayRaw helper
- Full object paths for NPC persistence
- Line-of-sight avoidance spawning
- Anti-stuck teleportation rules
- Windows MAX macro interferes with std::max

#### FriendSubsystem.cpp
- Three-layer perception disabling (component + clear + hook)
- NPCConfig is shared - don't modify directly
- Sound group names for audio feedback

#### VRMenuSubsystem.cpp
- No NewObject/ConstructWidget in this SDK
- FString lifetime management
- Input action naming conventions
- Anti-rebound for navigation

---

## Common Patterns

### Singleton with Lazy Init

```cpp
class MySubsystem {
public:
    static MySubsystem* Get() {
        static MySubsystem* s_instance = nullptr;
        if (!s_instance) {
            s_instance = new MySubsystem();
        }
        return s_instance;
    }
};
```

### Atomic State with Toggle

```cpp
class MyFeature {
    std::atomic<bool> enabled_{false};
public:
    void Toggle() {
        bool was = enabled_.exchange(!enabled_.load());
        LOG_INFO("MyFeature: " << (was ? "OFF->ON" : "ON->OFF"));
    }
    bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }
};
```

### Thread-Safe Collection

```cpp
mutable std::recursive_mutex mutex_;
std::vector<Entry> entries_;

void Add(Entry e) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    entries_.push_back(e);
}

// Use recursive_mutex if callbacks might re-acquire the lock
```

### Tick Rate Limiting

```cpp
float lastActionTime_ = 0.0f;
const float kActionInterval = 5.0f;

void Update(SDK::UWorld* world) {
    float now = SDK::UGameplayStatics::GetTimeSeconds(world);
    if (now - lastActionTime_ < kActionInterval) return;
    lastActionTime_ = now;
    
    // Do action
}
```

---

## SDK Considerations

### What the SDK Can Do

- ✅ Access existing objects via GObjects iteration
- ✅ Call methods on existing objects
- ✅ Get/set properties
- ✅ Spawn actors using existing classes
- ✅ Use Kismet library functions

### What the SDK Cannot Do

- ❌ Create UObjects from scratch (no NewObject)
- ❌ Construct widgets programmatically (no ConstructWidget)
- ❌ Modify class definitions
- ❌ Add new properties to existing classes

### Object Validity

Always validate before use:

```cpp
if (!actor || !SDK::UKismetSystemLibrary::IsValid(actor)) {
    return;
}
```

---

## Code Modification Guidelines

### Before Making Changes

1. **Read the AILEARNINGS** in the target file
2. **Check for related subsystems** that might need updates
3. **Understand the threading model** for the code path
4. **Identify cache invalidation needs**

### When Adding Features

1. Create separate subsystem file for significant features
2. Follow the Initialize/Update/Shutdown pattern
3. Add ScopedProcessEventGuard for SDK calls
4. Add commands for testing
5. Add AILEARNINGS for any non-obvious decisions

### When Fixing Bugs

1. Document the bug in AILEARNINGS
2. Explain the root cause
3. Note what was tried that didn't work
4. Describe the fix

### Testing Changes

1. Always test in-game
2. Check log file for errors
3. Test level transitions
4. Test with cheats enabled/disabled
5. Verify no memory leaks on repeated use

---

## Performance Guidelines

### ProcessEvent Hooks

- Minimize work in hook functions
- Cache string comparisons
- Return early for non-matching cases
- Avoid allocations in hot paths

### Per-Tick Updates

- Use time-based gating for expensive operations
- Don't iterate all GObjects every tick
- Cache frequently accessed pointers (with invalidation)

### Memory

- Prefer stack allocation over heap
- Use object pools for frequently created/destroyed items
- Clean up on level change

---

## Debugging Tips

### Enable Tracing

```
trace_on PartialFunctionName
```

### Check Logs

```
%USERPROFILE%\AppData\Local\ITR2Mod\mod.log
```

### Add Temporary Logging

```cpp
LOG_INFO("[DEBUG] " << variable << " state=" << state);
```

### Enable Debug Mode

```
debug
```

---

## Common Mistakes to Avoid

| Mistake | Consequence | Prevention |
|---------|-------------|------------|
| MessageBox in DllMain | Deadlock | Use logging |
| Missing DisableThreadLibraryCalls | Repeated init, hangs | Always add it |
| Missing ScopedProcessEventGuard | Stack overflow | Add to all SDK-calling functions |
| Stale pointer after level change | Crash | Clear caches |
| GObjects->Num with . | Compile error | Use -> for GObjects |
| std::mutex in SDK callbacks | Deadlock | Use std::recursive_mutex |
| UI calls from background thread | Crash | Only from OnTick |
| __try in function with C++ objects | C2712 error | Extract SEH to plain function |

---

## Questions to Ask Before Committing

1. Did I add AILEARNINGS for any non-obvious changes?
2. Did I use ScopedProcessEventGuard where needed?
3. Will this work after a level change?
4. Is this thread-safe?
5. Did I test in-game?
6. Does this work with arena running?
7. Does this work with friends spawned?
