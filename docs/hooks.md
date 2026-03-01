# Hook System

Documentation for the ProcessEvent hooking system used by this mod.

## Overview

The mod intercepts Unreal Engine's `ProcessEvent` function to:
- Execute code on game thread ticks
- Intercept and modify game behavior (cheats)
- Track game events (tracing)
- Handle VR input actions

## How It Works

### ProcessEvent in Unreal

`UObject::ProcessEvent(UFunction*, void*)` is called for:
- Blueprint function calls
- Replicated function calls
- Delegate broadcasts
- Timer callbacks
- Input action dispatches

### VTable Patching

```cpp
// Every UObject has a VTable pointer as its first member
struct UObject {
    void** vTable;  // [0] = VTable pointer
    // ...
};

// ProcessEvent is at a specific index in the VTable
void* originalProcessEvent = vTable[PROCESS_EVENT_INDEX];

// Replace with our function
vTable[PROCESS_EVENT_INDEX] = &OurProcessEventDetour;
```

### The Detour Function

```cpp
void ProcessEventDetour(
    const SDK::UObject* object,
    SDK::UFunction* function,
    void* params)
{
    // 1. Check recursion guard
    // 2. Execute named hooks
    // 3. Execute cheat logic
    // 4. Trace if enabled
    // 5. Call OnTick if appropriate
    // 6. Call original ProcessEvent
}
```

## Recursion Guard

Some SDK calls internally trigger ProcessEvent. Without a guard, this causes infinite recursion:

```cpp
struct ScopedProcessEventGuard {
    static thread_local bool s_active;
    bool previous;
    
    ScopedProcessEventGuard() {
        previous = s_active;
        s_active = true;
    }
    
    ~ScopedProcessEventGuard() {
        s_active = previous;
    }
    
    static bool IsActive() { return s_active; }
};

// Usage in hook:
if (ScopedProcessEventGuard::IsActive()) {
    return OriginalProcessEvent(obj, fn, parms);
}
```

**Critical:** Use `Mod::ScopedProcessEventGuard` in any function that calls SDK methods if that function might be called from ProcessEvent.

## Named Hooks

Register function-specific handlers:

```cpp
// Registration
HookManager::Get().RegisterNamedHook("OnPerceptionUpdated", 
    [](UObject* obj, UFunction* fn, void* parms, ProcessEventFn orig) -> bool {
        // Return true = suppress original
        // Return false = call original
        return false;
    });

// Dispatch in detour
std::string fnName = function->GetName();
if (auto* hook = namedHooks_.find(fnName)) {
    if ((*hook)(object, function, params, originalProcessEvent_)) {
        return;  // Hook handled, skip original
    }
}
```

### Common Named Hooks

| Function Name | Purpose |
|---------------|---------|
| `OnPerceptionUpdated` | Filter friend NPC perception |
| `IA_Button2_Left` | VR B/Y button input |
| `IA_Grip_Left` | VR grip pressed |
| `IA_UnGrip_Left` | VR grip released |
| `OnDeath` | Track NPC/player deaths |

## Cheat Hooks

Inline modifications in the detour:

### Unlimited Ammo

```cpp
if (unlimitedAmmoEnabled_) {
    static const std::string fireFnName = "Fire";
    std::string fnName = function->GetName();
    
    if (fnName == fireFnName || fnName.find("Fire") != std::string::npos) {
        // Get weapon component
        // Read current ammo
        // Call original (which decrements)
        originalProcessEvent_(object, function, params);
        // Restore ammo
        SetAmmo(weapon, savedAmmo);
        return;
    }
}
```

### Durability Bypass

```cpp
if (durabilityBypassEnabled_) {
    // Functions that reduce durability
    if (fnName == "ReduceDurability" || fnName == "ApplyWear") {
        return;  // Skip entirely
    }
}
```

## Tick Callback

The OnTick callback is triggered from ProcessEvent:

```cpp
// Look for world tick events
if (fnName == "Tick" && IsWorldOrGameMode(object)) {
    SDK::UWorld* world = SDK::UWorld::GetWorld();
    if (world) {
        ModMain::OnTick(world);
    }
}
```

This ensures OnTick runs on the game thread during the normal tick cycle.

## Tracing

Debug feature to log ProcessEvent calls:

```cpp
// Enable with filters
HookManager::Trace_SetEnabled(true);
HookManager::Trace_SetFilter("Input");      // Function name filter
HookManager::Trace_SetObjectFilter("Player"); // Object name filter

// In detour:
if (traceEnabled_) {
    std::string fnName = function->GetName();
    std::string objName = object->GetName();
    
    if (MatchesFilter(fnName, fnFilter_) && 
        MatchesFilter(objName, objFilter_)) {
        LOG_INFO("[Trace] " << objName << "." << fnName);
    }
}
```

## HUD Tracing

Specialized tracing for UI/Widget events:

```cpp
// Captures detailed payload information
void HudTraceCapture(UObject* obj, UFunction* fn, void* params) {
    // Log function signature
    // Log parameter values
    // Log call stack context
    // Write to dedicated trace file
}
```

## Adding a New Hook

### 1. Identify the Function

Use tracing to find the function name:
```
trace_on YourFeature
```

### 2. Register Named Hook

In your subsystem's initialization:

```cpp
void MySubsystem::Initialize() {
    HookManager::Get().RegisterNamedHook("TargetFunctionName",
        [](UObject* obj, UFunction* fn, void* parms, ProcessEventFn orig) -> bool {
            
            // Check if this is the right object type
            if (!obj->IsA<UMyClass>()) {
                return false;  // Not ours, call original
            }
            
            // Do custom logic
            MyCustomLogic(obj, parms);
            
            // Call original if needed
            orig(obj, fn, parms);
            
            // Return true to suppress original (rare)
            return false;
        });
}
```

### 3. Parse Parameters

Parameters are packed in a struct matching the function signature:

```cpp
// For function: void MyFunc(int32 Value, FString Name)
struct MyFunc_Params {
    int32 Value;
    SDK::FString Name;
};

// In hook:
auto* params = static_cast<MyFunc_Params*>(parms);
int32 value = params->Value;
```

## Best Practices

### DO:
- ✅ Use `ScopedProcessEventGuard` when calling SDK functions
- ✅ Check object validity before casting
- ✅ Keep hook logic minimal and fast
- ✅ Return false by default (call original)
- ✅ Log sparingly (ProcessEvent is called thousands of times per second)

### DON'T:
- ❌ Call MessageBox or UI functions from hooks
- ❌ Make blocking network calls
- ❌ Allocate large amounts of memory
- ❌ Use try/catch (SEH incompatibility)
- ❌ Assume function name uniqueness across all objects

## Performance Considerations

ProcessEvent is called extremely frequently. Hooks should be:

1. **Fast path for non-matching functions**:
   ```cpp
   // String compare is expensive - do it once
   static const std::string targetName = "MyTarget";
   if (function->GetName() != targetName) return false;
   ```

2. **Avoid allocations**:
   ```cpp
   // BAD: allocates each call
   std::string name = function->GetName();
   
   // BETTER: const reference where possible
   const std::string& name = function->GetName();
   ```

3. **Minimize SDK calls**:
   ```cpp
   // Each SDK call may trigger more ProcessEvent calls
   // Use ScopedProcessEventGuard and batch calls
   ```

## Debugging Hooks

1. **Enable tracing** to see what's being called:
   ```
   trace_on MyFeature
   ```

2. **Check the log file** for hook registration:
   ```
   [Hook] Registered named hook: MyFunction
   ```

3. **Add temporary logging** in your hook:
   ```cpp
   LOG_INFO("[MyHook] Triggered on " << obj->GetName());
   ```

4. **Use debug mode** for extra verbosity:
   ```
   debug
   ```
