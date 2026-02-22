# AI Assistant Guide (Into The Radius 2 mod)

## What this project is

This repo is a VR game mod for **Into The Radius 2 (UE 5.5)** built on a Dumper-7 generated SDK. The modding SDK has been dumped from the game "Into the Radius 2", which is a VR title. The mod is built as a DLL that replaces the original version.dll, allowing us to inject custom code and modify game behavior at runtime.

VR GAMES ARE HARD TO TEST, DON'T WASTE TIME ON SMALL, NON-DETERMINATIVE CHANGES. When in doubt, add logs and make bigger changes that can conclusively confirm or refute a hypothesis in one VR test cycle.

**CRITICAL REQUIREMENT: AILEARNINGS BLOCKS**
ALWAYS ADD LEARNINGS TO THE TOP OF THE CPP FILES IMMEDIATELY AFTER FIXING A CRASH OR COMPILATION ERROR. This allows future AI assistants to quickly orient themselves. 
- **If you solve a problem, you MUST document it in the affected .cpp file.**
- **Failure to update the AILEARNINGS block is a breach of these instructions.**
- Document the symptom (e.g., "spawn_mimic crash"), the cause (e.g., "RegisterNPCWithSubsystem sensitive to timing"), and the solution (e.g., "disable or delay registration").

Building out the mod will begin with proving some core concepts:

1. Override the AI spawning system. The first step of replacing the spawning system is to demonstrate the ability to disable or override the loading of all existing AI on a map, and the ability to manually spawn AI at a point in front of the player on demand. The TCP server would be used with this to recieve a command to spawn enemy, the enemy type, the distance etc.
2. Demonstrate the ability to spawn items for the player in the world, in their inventory, and equipped on their body. Initial interaction via the TCP server.
3. Modify attributes of items, npcs and the player in the world. For example disabling weapon degradation, or player hunger, or weapon damage.
4. Demonstrate level loading, or modification of existing levels by spawning and removal of props.
Based on the progress and feasibility of the demonstrated functionality, stage 2 of planning would then develop a path forward from there.

The modding SDK is a dense list of functions and files and needs to be documented. SDK.hpp is an auto generated file that includes the list of generated code.

The current focus is "proof-mode" iteration on gameplay changes with comprehensive logging to minimize expensive VR test cycles. The end goal is to implement a wave based combat arena, that helps the player learn muscle memory for the core VR combat loop, as well as increase their familiarity with the game's weapons, enemies, and mechanics.
The initial phase of the mod is focused on implementing game changing modifications to demonstrate the hooking infrastructure. The first major feature is **unlimited ammo** for held weapons, implemented via `ProcessEvent` interception and scoped to avoid affecting non-held weapons (e.g. shop items, world spawns). In a future revision, the unlimited ammo could apply to an in game powerup, but will also be useful for testing and learning about the game codebase in general.

### Primary active cheats

- **Unlimited ammo** (implemented via `ProcessEvent` interception; scoped to held weapons only)
- **Durability bypass** (prevent weapon degradation; scoped to held weapons only)
- **Bullet Time** (slow motion world while player moves at normal speed; implemented via global/custom time dilation)
- **God Mode** (prevents player death; likely implemented via health locking in `Cheats::Update`)
- **Hunger/Fatigue Bypass** (disables hunger and fatigue accumulation)

Both cheats can now be toggled via TCP commands (`ammo`, `durability`, `bullettime`, `godmode`, `hunger`, `fatigue`) and via the built-in in-game cheat panel checkboxes (see below).

## High-level approach

### Why ProcessEvent hooking

Most gameplay logic in ITR2 is Blueprint/reflection driven. Hooking **`UObject::ProcessEvent`** via VTable patching is the most reliable way to intercept key gameplay functions without chasing volatile memory addresses.

### How hooking is implemented

- **Manual VTable patching**: We locate unique VTables in `GObjects` and patch the `ProcessEvent` slot.
- **Named-hook registry**: Hooks are keyed by `UFunction::GetName()` for clean modularity.
- **Lock-free dispatch**: We use a snapshot of the hook registry for O(1) lookups in the `ProcessEvent` hot-path.
- **Reentry Guard**: A thread-local guard prevents infinite recursion when the mod calls SDK functions that would otherwise re-trigger the hook.

## Best Practices for AI Assistants

1. **Gating is Everything**: Always scope your hooks using `IsHeldContext()`. Intercepting gameplay functions globally will likely break the shop, NPC behavior, or world objects.
2. **Logging is your only eyes**: Since VR testing is slow, add `LOG_INFO` with context (e.g., `pThis`, `parms`, current state) in every execution path of a new hook.
3. **Reentry Nuance**: If your hook needs to call other SDK functions (like `InitializeMagazine`), wrap those calls in a `ScopedProcessEventGuard`. But **do not** wrap the call to the `originalFn` for the gameplay event itself, or you will block nested engine logic.
4. **Use `ModFeedback`**: When a cheat or command is triggered, use `Mod::ModFeedback::ShowMessage` to give visual confirmation in the headset.
5. **GObjects Access**: `SDK::UObject::GObjects` is a `TUObjectArrayWrapper`. Use `->` (e.g., `GObjects->Num()`) to access the underlying `TUObjectArray`. Using `.` will cause a compilation error as `Num()` is not a member of the wrapper itself.
6. **Header Forward Declarations**: When using SDK types in mod headers (like `ModMain.hpp`), use forward declarations (e.g., `namespace SDK { class UWorld; }`) instead of including large SDK headers to avoid circular dependencies and slow compile times.

### Session Discipline (don’t go off the rails)

These are process rules. They are here because VR testing is expensive and assistant drift wastes time.

1. **Do exactly what the user asked**: If the request is “verify MCP server”, do not edit unrelated gameplay/mod files. If extra work seems useful, ask first.
2. **Do not reinterpret instructions**: The user’s chat instructions + this guide are the authoritative sources. Don’t “improve” the plan by silently changing semantics.
3. **Clarify ambiguous persistence**: If you propose “avoid disk spam”, explicitly state whether trace data is persisted to disk, streamed over TCP, or only kept in-memory.
4. **Never deliver only a trace summary**: A summary is supplemental. When tracing is requested, provide a way to retrieve the *full underlying events*.
5. **Be explicit about tool capability**: If MCP tools exist in the environment, use them. Don’t claim you can’t if you can.

### Trace Logging (non-negotiable requirements)

When implementing any “trace” / “trace all” / “log literally everything” mode, follow these rules:

1. **Full data must be retrievable**
  - A trace mode that only increments counters or outputs a summary is *not* sufficient.
  - Provide a way to retrieve the *full event stream* captured during the trace window.

2. **Persistence must match the ask**
  - If the user asks for a “full trace log”, the trace must be persisted (disk file and/or streamed over TCP) in a way that survives a crash.
  - “Avoid disk spam” means batching, throttling, compression, or a dedicated trace file — it does *not* mean “don’t write trace events at all”.

3. **Crash survivability**
  - If tracing is used to diagnose crashes, in-memory-only buffers are insufficient by themselves.
  - Use one of:
    - (Preferred) write append-only trace batches to a separate trace file (e.g. `itr2_trace.log`) with explicit flush points
    - Stream trace events out over TCP/UDP to an external collector
    - Or both

4. **User-controlled scope**
  - Tracing must be toggleable and scoping/filtering must be available (e.g. by function name prefix).
  - Provide commands that: enable, disable, reset, dump-full, and (optionally) dump-summary.
  - If performance impact is acceptable while enabled, bias toward completeness.

5. **Output format**
  - Each event should include at least: timestamp, thread id, function name, object name/class (if available), and a stable way to reconstruct ordering.
  - If you add a ring buffer, it’s fine for speed — but it must not be the *only* place the data exists when “full trace log” is required.

6. **If you change behavior, prove it in the log**
  - Add an unmistakable “TRACE ENABLED” / “TRACE DISABLED” log line (and where it’s writing to).
  - Add build/version stamping where needed so users can verify the running DLL matches the compiled code.

## Where to edit

### Core hook plumbing

- `Mod/HookManager.hpp`
- `Mod/HookManager.cpp`

Key concepts inside `HookManager.cpp`:

- **Named hook snapshot**: hooks are registered at init and copied to a snapshot for safe lock-free dispatch.
- **Reentry guard scope**: only guard the *intentional SDK calls* (e.g. resync), not the entire gameplay call chain.
- **Held-weapon gating**: never mutate world/shop weapons.

### Cheats toggles / UI

- `Mod/Cheats.cpp`, `Mod/Cheats.hpp`
- TCP control path: `Mod/TcpServer.*`, `Mod/CommandHandler.*`

## Current code map (what files do what)

This section is here so future AI assistants can quickly orient themselves without re-discovering the repo.

### Root / project glue

- `dllmain.cpp`
  - DLL entry point (loaded as `version.dll` replacement).

- `itr2-dumper7-sdk-base.cpp`, `itr2-dumper7-sdk-base.h`
  - Project-level glue code (often minimal; entry wiring depends on your build).

- `pch.h`, `pch.cpp`
  - Precompiled header setup.

- `SDK.hpp`
  - Auto-generated “umbrella include” for the dumped SDK.

- `UnrealContainers.hpp`, `PropertyFixup.hpp`, `UtfN.hpp`, `Assertions.inl`
  - Dumper-7 runtime support (container types, UE property fixups, string helpers, asserts).

### Mod runtime

- `Mod/ModMain.cpp`, `Mod/ModMain.hpp`
  - Mod lifecycle entry.
  - Initializes subsystems, starts TCP server, registers/initializes hooks.

- `Mod/Logging.hpp`
  - Logging macros and output routing.
  - Prefer rate-limited logs in hot paths.

- `Mod/GameContext.*`
  - Best-effort helpers to get `UWorld`, player character, and a `WorldContextObject` usable for Kismet calls.

- `Mod/ModFeedback.*`
  - VR-friendly feedback via `UKismetSystemLibrary::PrintString` (and optional `DrawDebugString`).

- `Mod/TcpServer.cpp`, `Mod/TcpServer.hpp`
  - TCP server that accepts external commands.

- `Mod/CommandHandler.cpp`, `Mod/CommandHandler.hpp`
  - Command parsing and dispatch.

- `Mod/CommandQueue.hpp`
  - Cross-thread / deferred work patterns (useful if you ever need to defer work out of ProcessEvent).

### Cheats

- `Mod/Cheats.cpp`, `Mod/Cheats.hpp`
  - User-facing toggles.
  - Should only flip state and delegate the heavy lifting to HookManager / subsystems.
  - Shows in-headset feedback when toggled.

### Hooking

- `Mod/HookManager.hpp`, `Mod/HookManager.cpp`
  - Manual vtable patching for `ProcessEvent`.
  - Named-hook registry (snapshot for lock-free dispatch).
  - Unlimited ammo and durability bypass are currently implemented here as named hooks.

### Gameplay subsystems (arena groundwork)

- `Mod/ArenaSubsystem.*`
  - Automated wave-based arena. 
  - Features:
    - **NPC Discovery**: Scans for new NPC types every 30s and persists them to `npcs.txt`.
    - **Random Spawning**: Randomly selects from discovered types for each spawn slot.
    - **Circular Spawning**: Spawns enemies in a circle around the player with edge/void avoidance (10 retries per slot).
    - **Active Pressure**: Instructs all arena NPCs to move to player position every 30s.
    - **Buffed AI**: Maxes out NPC awareness (sight/sound) and allows 10 concurrent attackers.
    - **Auto-advance**: Next wave starts automatically after current wave is cleared.
    - **Feedback**: VR-friendly notifications for wave start, kills (remaining count), and wave clear.
  - Commands: 
    - `arena_start [count] [distance]` (defaults: 10, 3000)
    - `arena_stop`
  - AI spawning and AI-related experimentation.

- `Mod/ItemSubsystem.*`
  - Item spawning and inventory experiments.

## Recommended additional files / structure

As the mod grows, keeping all hook logic in `HookManager.cpp` will get painful. A clean next step is to keep HookManager as plumbing and move hook logic into focused modules.

Suggested structure (incremental, not mandatory):

- `Mod/Hooks/HookNames.hpp`
  - Central place for function-name strings used in named hooks.
  - Prevents typos and makes it easy to grep.

- `Mod/Hooks/AmmoHooks.hpp` / `Mod/Hooks/AmmoHooks.cpp`
  - Unlimited ammo logic:
    - `ServerShootProjectile` handler
    - `TryExtractNextItem` handler
    - ammo tag capture rules
    - resync gating

- `Mod/Hooks/DurabilityHooks.hpp` / `Mod/Hooks/DurabilityHooks.cpp`
  - Durability bypass logic:
    - `DamageDurabilityFromShot`
    - `ChangeDurability`

- `Mod/Hooks/HeldWeapon.hpp` / `Mod/Hooks/HeldWeapon.cpp`
  - “Held weapon” detection helpers.
  - Keep both approaches (grip-controller and shot-context) together.

- `Mod/Hooks/Resync.hpp` / `Mod/Hooks/Resync.cpp`
  - `ForceResyncFirearm()` and any future resync variants.
  - Centralizes the “be careful, it breaks visuals if misused” logic.

- `Mod/Diagnostics/RateLimit.hpp`
  - Small helper for rate-limited logs (so every hook doesn’t reinvent `static int logCount`).

Why this structure helps:

- Keeps `HookManager.cpp` stable (less crash risk).
- Lets you iterate on ammo vs durability independently.
- Makes it easier to revert/disable one hook family when debugging.

## SDK research guide (how to find info fast)

The Dumper-7 SDK is big. The fastest workflow is usually:

1. Find the **function name** you’re seeing in logs (`UFunction::GetName()`).
2. Find its **parameter struct** in `*_parameters.hpp`.
3. Find any **rep/state structs** in `*_structs.hpp`.
4. Find the **class** in `*_classes.hpp`.

### Key SDK files to search

- `SDK/IntoTheRadius2_classes.hpp`
  - Main game classes (e.g. `URadiusFirearmComponent`, `ARadiusItemBase`).

- `SDK/IntoTheRadius2_structs.hpp`
  - Replication / payload structs (e.g. `FFirearmComponentShotExtendedRep`).

- `SDK/IntoTheRadius2_parameters.hpp`
  - Parameter layouts for reflected calls (most important for ProcessEvent handlers).

- `SDK/VRExpansionPlugin_classes.hpp`
  - Grip system types (`UGripMotionControllerComponent`, `GetIsHeld`, etc.).

- `SDK/Engine_classes.hpp`
  - Base engine types and helpers (`UActorComponent::GetOwner()`, etc.).

### Terminology / strings worth grepping

When you’re trying to solve a gameplay problem, grep for:

- Networking / authority:
  - `Server`, `Client`, `Multicast`, `Replicated`, `OnRep_`, `RepNotify`

- Ammo / firearm:
  - `Shoot`, `Projectile`, `Chamber`, `Barrel`, `Magazine`, `AmmoState`, `TryExtractNextItem`, `IsOutOfAmmo`

- Gripping / held detection:
  - `Grip`, `Gripped`, `IsHeld`, `GetIsHeld`, `IsInPlayerHands`, `IsHeldByParentChain`

- Durability / malfunctions:
  - `Durability`, `Dysfunction`, `Jam`, `FailReason`

### Practical SDK “triangulation” tip

If a ProcessEvent handler needs to read/write `parms`, do not guess the layout.

- Find the exact handler’s params in `*_parameters.hpp`.
- If the function takes a `struct` by reference (like `ServerShootProjectile`), find that struct in `*_structs.hpp`.
- **Beware of TArray**: Generated TArray helpers might not always be available. Use `UnsafeTArrayNum` in `HookManager.cpp` for a stable way to read UE TArray sizes.

## Recommended troubleshooting approaches

VR testing is expensive, so prefer approaches that give a decisive answer in one run.

### If the game crashes after enabling a cheat

- Add a single log right at hook entry and right before calling `originalFn` to confirm how far you get.
- Suspect (in order):
  - pointer misuse (`object`, `function`, `parms`)
  - parameter struct mismatch
  - reentrancy recursion (calling SDK functions inside hook without a guard)
  - thread-safety issues (reading/writing hook tables without a snapshot)

### If unlimited ammo “sort of works” but ammo still runs out

- Confirm the hook is intercepting the right function name (log `function->GetName()` once).
- Confirm the handler returns `true` when it intends to override behavior.
- Re-check reentry guard scope:
  - it must not be enabled while calling `originalFn` for gameplay events that rely on nested ProcessEvent calls.

### If shop-spawned / world weapons get visually broken

- Your held-weapon gating is too permissive.
- Ensure hooks only apply to:
  - weapons that the grip system reports as held (`GetIsHeld`), or
  - nested calls inside an active held-weapon firing chain.

### If a newly purchased weapon fires once then becomes “empty”

- Do not short-circuit `TryExtractNextItem` unless you have a valid observed ammo tag.
- Avoid resyncing on bad state:
  - follow the resync gating rule (mag > 1 and ammo present in chamber/barrel).

### If performance tanks / logs explode

- Rate-limit logs.
- Keep the ProcessEvent hot path minimal (early-outs before any string work when cheats are disabled).
- Prefer named-hook snapshot dispatch over scanning or complex conditionals.

## Unlimited ammo: current design

Unlimited ammo is not “just return false from IsOutOfAmmo”. The effective path is:

- Intercept `URadiusFirearmComponent::ServerShootProjectile(FFirearmComponentShotExtendedRep& Rep)`
  - Capture the ammo tag (for later extraction)
  - Call original
  - Conditionally call `ForceResyncFirearm()` (see resync gating below)

- Intercept `TryExtractNextItem`
  - Only short-circuit when:
    - the weapon is **held**, and
    - we have a previously observed ammo tag
  - Otherwise allow original behavior (prevents breaking newly spawned / newly purchased weapons)

### Resync gating (important)

Resync calls are powerful and can break state/visuals if applied at the wrong time.

Current rule (by requirement): only resync if:

- **MagazineAmmoLeft > 1**, and
- there is **ammo in chamber/barrel** (approximated from the shot rep’s `AmmoInBarrel`)

If the rule fails: skip resync and log why.

## Durability bypass: current design

Durability is bypassed by intercepting `DamageDurabilityFromShot` and `ChangeDurability`.

Constraints:

- Only apply to **held weapons**.
- If not held: return `false` from handler so the original function runs.

## Held weapon detection (two approaches)

Held scoping is critical to avoid side effects (shop items, spawned guns, dropped weapons, etc.).

Current implementation uses two independent signals:

1. **Grip-controller signal**: for `ARadiusItemBase` owners, consult `GripControllerPrimary/Secondary->GetIsHeld(actor)`.
2. **Shot-context signal**: while inside a held weapon’s `ServerShootProjectile` original call chain, mark a thread-local “active shot” context so nested `ProcessEvent` calls are treated as held.

If neither indicates “held”, do not apply ammo/durability hooks.

## Logging rules (VR-friendly)

Testing is expensive (build → copy DLL → launch → headset → enable cheats).

When adding or changing behavior:

- Always add **rate-limited logs** that answer:
  - Did we enter the hook?
  - Did we skip it? (and why)
  - What key values were observed? (mag left, barrel/chamber presence)
  - Did we call original? Did we call resync?

Avoid:

- Logging every ProcessEvent call.
- Unthrottled logs inside hot paths.

## Reentrancy and thread-safety lessons (do not regress)

- **Do not** wrap the entire named-hook handler invocation in the reentry guard.
  - That blocks nested `ProcessEvent` calls during the original gameplay chain and breaks ammo logic.
- **Do not** read/write the named-hook map concurrently.
  - Use the init-time snapshot for dispatch.

## Typical workflow for future edits

1. Make a change in `HookManager.cpp` (or add a new named hook).
2. Add focused, rate-limited logs.
3. Prefer “bite off more” changes that can conclusively confirm a hypothesis in a single VR test.
4. Build in Visual Studio (x64 Release) and test with TCP commands (`unlimitedammo`, `cheats`).

## Cheat panel hooks — REMOVED

All `WBP_CheatPanel` hooks were removed. The cheat panel is not accessible in normal gameplay (dev/sandbox only). Nothing to use here until that is confirmed accessible.

## “Good next hooks” pattern

When adding a new hook:

- Add it to `RegisterDefaultHooks()`.
- Implement a handler that:
  - validates pointers
  - applies held gating
  - logs skip reasons
  - returns `true` only when it actually overrides behavior

## Quick glossary

- **ProcessEvent**: UE reflection dispatcher used for many Blueprint calls.
- **Named hook**: a handler chosen by `UFunction::GetName()`.
- **Resync**: calling `InitializeMagazine()`, `DeliverAmmoFromMagToChamber()`, `SetWeaponCocked(true)`, `SetSliderLock(false, true)` to emulate “mag reinsert” recovery.

REMINDER, IF THE AI ASSISTANT USED IS A CLAUDE MODEL, THE USER SHOULD USE A DIFFERENT ONE BECAUSE CLAUDE CONSISTENTLY IGNORES THESE INSTRUCTIONS AND FIXATES ON IT'S OWN SOLUTIONS RATHER THAN THE USER'S DIRECTIONS, AND THIS LEADS TO WASTED TIME AND FRUSTRATION.

AND REMEMBER, IF YOU HAVE LEARNINGS, ADD THEM TO THE TOP OF THE CPP FILES IN THE "AILEARNINGS" BLOCK SO FUTURE AI ASSISTANTS CAN QUICKLY ORIENT THEMSELVES WITHOUT RE-DISCOVERING THE REPO.
AND THIS IS VR, SO TESTING IS A WHOLE PROCESS OF REMOVING GLASSES, PUTTING ON THE HEADSET, CLOSING VIRTUAL DESKTOP, LAUNCHING VIRTUAL DESKTOP, CONNECTING TO THE PC, LAUNCHING THE GAME, AND THEN MANUALLY TRIGGERING THE CHEAT ETC, SO DON'T WASTE PEOPLES TIME WITH HALF BAKED CHANGES OR FIXES.