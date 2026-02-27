# VR Menu Implementation Design

## Overview

Two independent in-world VR menu rendering approaches, both driven by the same shared menu state. The menu is toggled and navigated entirely from VR controller input — no telnet, no desktop required.

---

## Approach 1: DrawDebugString Panel (UNVERIFIED — may not render in VR)

**Status: EXPERIMENTAL.** `DrawDebugString` has never been observed to produce visible output in the VR headset. `ShowWorldText` (which wraps it) exists in the codebase but has zero callers — it was never actually tested. In UE5 shipping/VR builds, the debug draw pipeline is commonly stripped or the debug canvas doesn't render to the HMD. This approach is kept as a diagnostic experiment to prove or disprove DrawDebugString visibility in VR.

**How it works:** Every tick, when the menu is open, we call `UKismetSystemLibrary::DrawDebugString` multiple times at calculated world positions near the player's left hand. Each line of text is a separate DrawDebugString call, offset vertically in world space.

**Pros:**
- Zero UObject creation — no `NewObject`, no widget trees, no actors to spawn
- Very low crash risk — if it fails, it just doesn't draw text
- Can be toggled on and off instantly
- Good diagnostic: if the LOG shows calls going through but nothing is visible, we know the debug pipeline is stripped

**Cons:**
- **May not render at all in VR** — debug draw primitives share a canvas that is often disabled in shipping builds
- No background panel, just floating text (looks like debug overlay — if it renders)
- Selected item highlighted by color/prefix only (e.g. `>>` marker, green highlight)
- Text re-drawn every frame so it flickers if duration is too short; we use a ~0.15s duration to overlap frames
- No true interactivity (no hit testing, no "buttons"), navigation is by controller input hooks
- Only confirmed working feedback in this mod is **subtitles** (`ShowSubtitles`)

**Key SDK calls:**
```cpp
SDK::UKismetSystemLibrary::DrawDebugString(worldCtx, position, text, nullptr, color, 0.15f);
```

**Position calculation:** Get left hand (`GrabSphereLeft_BP`) component location, offset forward and slightly right relative to the hand's orientation using the component's forward/right/up vectors.

---

## Approach 2: UWidgetComponent Hijack

**How it works:** At runtime, find an existing `UWidgetComponent` on the player character (e.g. `W_GripDebug_L` or `PlayerDebugWidget`) and hijack its widget. Or find the game's `ABP_IngameMenu_C::W_CheatPanel` widget component. We write directly into the UWidgetComponent's `Widget` pointer (offset 0x0630) with a reference to a `UUserWidget` we find/reuse.

**Pros:**
- If it works, it's a proper 3D panel rendered by the engine's UMG pipeline
- Could potentially reuse the game's existing cheat panel widget
- Engine handles anti-aliasing, backface culling, all rendering

**Cons:**
- SDK has **no `NewObject<T>()`** — we cannot programmatically create UObjects
- SDK has **no `UWidgetTree::ConstructWidget()`** — we cannot build widget trees
- Hijacking existing widgets may crash if the engine doesn't expect the pointer swap
- Need to find the widget component at runtime via GObjects scan
- Very high crash risk without proper object construction

**Mitigation:** Instead of constructing new UObjects, we scan GObjects for existing `UWBP_CheatPanel_C` instances or `UTextBlock` instances and repurpose them. This is extremely fragile but would prove/disprove the approach in one VR test.

**Alternative UWidgetComponent approach:** Find the player's `PlayerDebugWidget` (a `UWidgetComponent*` at known offset on gameplay character), check if it already has a widget, and manipulate that existing widget's text. Less risky than pointer-swapping.

---

## Shared Menu State

Both approaches read from the same `VRMenuState` struct:

```
MenuOpen: bool
SelectedIndex: int
Items: vector<MenuItem>
```

Each `MenuItem` has:
- `label`: Display text (e.g. "God Mode")
- `statusFn`: Lambda returning current state string (e.g. "ON" / "OFF")
- `actionFn`: Lambda to execute when selected (e.g. toggle god mode)

---

## VR Controller Input (Both Approaches)

**Toggle menu open/close:** Hook `InpActEvt_IA_Button2_Left` (left controller Y/B button press). When detected via ProcessEvent named hook, flip `MenuOpen`. This is the least-used button on the left controller.

**Navigate up/down:** Hook `InpActEvt_IA_Button1_Left_Hold` for up, repurpose left thumbstick movement during menu-open state. Actually — simpler: just use the left thumbstick Y axis. We hook `InpActEvt_IA_Movement` and when menu is open, check the `FInputActionValue` for Y axis > threshold to move selection.

**Select item:** Hook `InpActEvt_IA_Trigger_Left` (left trigger press). When menu is open, execute the selected item's action.

**Input suppression:** When the menu is open, the input hooks return `true` (handled) to suppress game movement/actions from the hooked inputs. This prevents the player from walking or shooting while navigating the menu.

### ProcessEvent Function Names to Hook

| Action | UFunction GetName() | Notes |
|--------|---------------------|-------|
| Toggle menu | `InpActEvt_IA_Button2_Left_K2Node_EnhancedInputActionEvent_17` | Left Y/B button (Started trigger) |
| Also toggle | `InpActEvt_IA_Button2_Left_K2Node_EnhancedInputActionEvent_18` | Left Y/B button (Completed trigger) |
| Movement/nav | `InpActEvt_IA_Movement_K2Node_EnhancedInputActionEvent_0` | Left thumbstick (Started) |
| Movement/nav | `InpActEvt_IA_Movement_K2Node_EnhancedInputActionEvent_1` | Left thumbstick (Triggered/tick) |
| Select | `InpActEvt_IA_Trigger_Left_K2Node_EnhancedInputActionEvent_24` | Left trigger press |

### FInputActionValue Layout

The `FInputActionValue` struct passed in params has the actual input value. For thumbstick, it's a 2D vector (X=horizontal, Y=vertical). For buttons, it's a bool/float. We need to inspect the raw bytes to extract the value since the params struct layout is:
```cpp
struct Params {
    FInputActionValue ActionValue;  // Offset 0x00
    float ElapsedTime;              // Offset after ActionValue
    float TriggeredTime;
    const UInputAction* SourceAction;
};
```

---

## SDK Constraints & Workarounds

| What we need | SDK status | Workaround |
|-------------|-----------|------------|
| Create UObject (NewObject) | **Not available** | Reuse existing objects from GObjects scan |
| UWidgetTree::ConstructWidget | **Not available** | Write to existing widget's text fields |
| SpawnActor | Via `UGameplayStatics::BeginDeferredActorSpawnFromClass` | Available but risky without knowing class layout |
| DrawDebugString | **Available** via `UKismetSystemLibrary` | **UNVERIFIED in VR** — ShowWorldText has zero callers; never observed rendering in headset. Debug draw may be stripped in shipping/VR builds. |
| GetComponentLocation | **Available** via `USceneComponent::K2_GetComponentLocation` | Works on GrabSphere, UWidgetComponent |
| MakeStableFString | Thread-local ring buffer in ModFeedback.cpp | Reuse or duplicate the pattern |
| FString lifetime | Non-owning wrapper | Must keep backing wstring alive |

---

## Implementation Order

1. Create `VRMenuSubsystem.hpp/cpp` with shared menu state + both renderers
2. Register ProcessEvent hooks for input in `HookManager::RegisterDefaultHooks()`
3. Call `VRMenuSubsystem::Update()` from `ModMain::OnTick()`
4. Build and test — DrawDebugString renderer is UNVERIFIED (may not render in VR); widget approach needs VR testing to see if GObjects widget hijack survives. Both log extensively for diagnostics.

---

## Risk Assessment

| Risk | Impact | Mitigation |
|------|--------|-----------|
| DrawDebugString doesn't render in VR | High | Keep as diagnostic; primary rendering via widget hijack or subtitle fallback |
| Widget hijack crashes | High | Wrapped in try/catch, can be disabled via bool |
| Input hooks conflict with gameplay | High | Only suppress inputs when menu is open |
| FInputActionValue layout wrong | Medium | Log raw bytes first, adjust offsets |
| Menu toggle fires twice (Started+Completed) | Low | Debounce with timestamp |
| GrabSphereLeft_BP is null | Medium | Fall back to player view position + offset |

---

## AILEARNINGS

- SDK (Dumper-7) does not expose NewObject, StaticConstructObject_Internal, or CreateDefaultSubobject. Cannot programmatically create UObjects.
- SDK does not expose UWidgetTree::ConstructWidget. Cannot build widget trees from scratch.
- UWidgetComponent::Widget is at offset 0x0630 (UWidgetComponent class layout from SDK).
- DrawDebugString is **UNVERIFIED in VR**. ShowWorldText wraps it but has zero callers — never tested. Debug draw pipeline may be stripped in VR/shipping builds. Only subtitles (ShowSubtitles) are confirmed to produce visible output in the headset.
- FString is non-owning — wraps a wchar_t* pointer. Must use MakeStableFString ring buffer pattern.
- The gameplay character has GrabSphereLeft_BP (UGrabSphere*) and GrabSphereRight_BP (UGrabSphere*) with K2_GetComponentLocation()/GetForwardVector()/GetRightVector()/GetUpVector().
- The gameplay character has PlayerDebugWidget (UWidgetComponent*) and W_GripDebug_L/R (UWidgetComponent*) already attached.
- InpActEvt_IA_Button2_Left has two ProcessEvent variants (_17 and _18) — likely Started and Completed triggers. Must debounce.
- UGameplayStatics::BeginDeferredActorSpawnFromClass + FinishSpawningActor exists but we'd need a valid UClass* to spawn, which requires finding it in GObjects first.
