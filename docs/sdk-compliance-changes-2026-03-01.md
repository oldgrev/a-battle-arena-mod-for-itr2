# SDK Compliance Change Log (UsingTheSDK excerpt)

Date: 2026-03-01  
Scope: `a battle arena mod for itr2/Mod/**` plus loader entrypoint checks in `dllmain.cpp` / `ModMain.cpp`  
Out of scope: `CppSDK/**` (explicitly excluded)

## Goal
Bring runtime code into closer alignment with Dumper7 `UsingTheSDK.md` guidance, especially:
- validate type (`IsA`) before downcasting from base `UObject`/`AActor` pointers,
- prefer safe object retrieval patterns with null checks,
- avoid unsafe startup/loader-lock behavior,
- harden subsystem and global object access paths.

---

## 1) `IsA`-before-cast hardening

### Why this change is good
The SDK excerpt explicitly emphasizes checking object type before casting. In UE, many APIs return base pointers (`UObject*`, `AActor*`) and relying on assumptions can produce undefined behavior when data changes between maps, game modes, or blueprint variants.

### Pros
- Reduces crash risk from invalid downcasts.
- Makes intent explicit at each cast boundary.
- Improves resilience to runtime class variation (especially BP/native mix).

### Cons
- Adds more branch checks and code verbosity.
- In very hot paths this introduces minor overhead (acceptable for safety here).

### Changes made
- `Mod/HookManager.cpp`
  - `Hook_ServerShootProjectile`: require `object->IsA(URadiusFirearmComponent)` before cast.
  - `GetDynamicDataFromActor`: ensure resolved weak pointer object `IsA(URadiusItemDynamicData)` before cast.
- `Mod/FriendSubsystem.cpp`
  - Spawn path now requires spawned actor `IsA(ARadiusAICharacterBase)` before AI-specific use.
  - Follow/reposition/idle tick paths now validate friend actor type before cast.
- `Mod/ArenaSubsystem.cpp`
  - Wave start and spawn paths use `IsA(ARadiusAICharacterBase)` before AI cast/use.
  - Time controller iteration validates `IsA(ARadiusTimeController)` before cast.
- `Mod/MenuPocSubsystem.cpp`
  - Added cast guards for spawned actors/widgets in POC4/POC6/POC8/POC9.
  - InfoPanel fallback cast now gated with `IsA(AInfoPanel)`.
  - Recursive widget walk now identifies text blocks by `IsA(UTextBlock)`.
- `Mod/VRMenuSubsystem.cpp`
  - POC9 creation validates widget type `IsA(UWBP_Confirmation_C)` before cast.
  - Root widget text-block cast now gated by `IsA(UTextBlock)`.
- `Mod/Cheats.cpp`
  - Replicator fallback paths validate `actors[0]` is `ARadiusGameDataReplicator` before cast.
  - `ApplyGodMode` fallback stats component path verifies `IsA(UPlayerStatsComponent)` before cast.
- `Mod/LoadoutSubsystem.cpp`
  - Player pawn now validated as `ABP_RadiusPlayerCharacter_Gameplay_C` before body-slot cast.
  - Fallback container-subsystem scan validates candidate `IsA(URadiusContainerSubsystem)` before cast.
- `Mod/AISubsystem.cpp`
  - World-subsystem returns are validated against `URadiusAICoordinationSubsystem` before cast in register/unregister paths.

---

## 2) Null-chain safety for object retrieval

### Why this change is good
The SDK excerpt’s retrieval examples imply defensive handling (`World`, `GameInstance`, `LocalPlayers`, `Controller`). Null-safe retrieval prevents transient startup/loading states from causing dereferences.

### Pros
- Prevents startup/map-transition null derefs.
- Better behavior in early runtime when objects are not initialized.

### Cons
- More guard code and early returns.

### Changes made
- `Mod/ItemSubsystem.cpp`
  - `GetPrimaryController` now checks `LocalPlayers[0]` for null before use.
- `Mod/AISubsystem.cpp`
  - `GetPrimaryController` now checks `players[0]` and `players[0]->PlayerController` before returning.
- `Mod/ModMain.cpp`
  - Hook-retry loop now requires `SDK::UObject::GObjects` non-null before `GObjects->Num()` checks.

---

## 3) `GObjects` guardrails before scanning/counting

### Why this change is good
Many mod features iterate global object arrays. During some startup windows, `GObjects` can be absent or unstable. Guarding this access avoids immediate hard faults.

### Pros
- Prevents null dereference on global object-array access.
- Makes behavior deterministic in early-load states.

### Cons
- Some diagnostics/features may no-op until `GObjects` is available.

### Changes made
- `Mod/HookManager.cpp`
  - `InstallProcessEventHookInternal` now returns with log if `GObjects` is null.
- `Mod/MenuPocSubsystem.cpp`
  - Added `GObjects` availability checks before subsystem scan and POC scan loops.
- `Mod/VRMenuSubsystem.cpp`
  - Added `GObjects` checks in both widget-textblock fallback scan and pointer scan.
- `Mod/LoadoutSubsystem.cpp`
  - Added explicit `GObjects` null guard before fallback subsystem scan.

---

## 4) Subsystem retrieval type validation

### Why this change is good
Even when requesting a known subsystem class, treating return values as `UObject*` first and validating type improves robustness against unexpected runtime states and blueprint overrides.

### Pros
- Safer subsystem access contracts.
- Better logging and clearer failure paths.

### Cons
- Slightly more verbose than direct cast style.

### Changes made
- `Mod/AISubsystem.cpp`
  - Coordination subsystem retrieval now goes through `UObject*` + `IsA` validation.
- `Mod/ArenaSubsystem.cpp`
  - Coordination/time subsystem retrieval now validates type before cast.
- `Mod/LoadoutSubsystem.cpp`
  - `GetWorldSubsystem` path now validates `URadiusContainerSubsystem` type before cast.

---

## 5) Loader/startup safety (DllMain/GetModSDKVersion guidance)

### Why this change is good
The excerpt and your own `AILEARNINGS` in `dllmain.cpp` emphasize loader-lock safety. This was validated and retained.

### Pros
- Avoids deadlocks and undefined loader-lock behavior.
- Keeps startup logic in worker thread, not `DllMain` body.

### Cons
- No code-level downside in this context.

### Status
- `dllmain.cpp` already compliant:
  - `DisableThreadLibraryCalls(hModule)` is present.
  - Main logic is dispatched to worker thread.
  - `GetModSDKVersion()` stays minimal and loader-safe.
- Additional hardening done in `Mod/ModMain.cpp` for `GObjects` null checks during hook retry.

---

## 6) Build/validation status

- Build executed: `Release|x64` solution build.
- Result: success, 0 errors, 0 warnings.

---

## Files modified for compliance work

- `Mod/AISubsystem.cpp`
- `Mod/ArenaSubsystem.cpp`
- `Mod/Cheats.cpp`
- `Mod/FriendSubsystem.cpp`
- `Mod/HookManager.cpp`
- `Mod/ItemSubsystem.cpp`
- `Mod/LoadoutSubsystem.cpp`
- `Mod/MenuPocSubsystem.cpp`
- `Mod/VRMenuSubsystem.cpp`
- `Mod/ModMain.cpp`

(Plus validation-only review of `dllmain.cpp`, no functional changes required.)

---

## Notes on tradeoffs

- Most modifications bias toward runtime safety and correctness over minimal branching.
- A few checks may be redundant in paths where engine APIs strongly imply returned types; they were still added to satisfy explicit guideline adherence and reduce hidden assumptions.
- No changes were made inside `CppSDK/**` per request.
