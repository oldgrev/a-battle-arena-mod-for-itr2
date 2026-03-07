# HandWidget System — Integration Guide

This guide explains how to integrate the portable **HandWidget** menu and notification system
into a mod for *Into The Radius 2*. The system is designed to be instance‑based and
widget‑agnostic: you pass it `UWidgetComponent*` pointers rather than hard‑coding
`W_GripDebug_L` / `W_GripDebug_R`.

---

## Architecture Overview

The system is four layers, each built on the one below:

| Layer | Class | Purpose |
|-------|-------|---------|
| 1 — Surface | `HandWidget` | Manages a single `UWidgetComponent` + `WBP_Confirmation_C` pair. Handles widget creation, text updates, visibility, draw‑size, pivot, forward offset, and forced redraws. |
| 2a — Notification | `HandWidgetNotification` | Timed text messages with a queue, auto‑hide, pause support. Wraps a `HandWidget`. |
| 2b — Menu | `HandWidgetMenu` | Page‑based interactive menu with thumbstick navigation, selection, and page‑stack. Wraps a `HandWidget`. |
| 3 — Multiplexer | `HandWidgetMultiplexer` | Manages sharing a single `UWidgetComponent` between multiple layers (e.g. menu + notification on the same hand) by priority. |

All classes live in `namespace PortableWidget` and include only standard C++ headers plus
the SDK. They have no dependency on the rest of the mod codebase.

---

## Quick Start — Minimal Notification

```cpp
#include "HandWidget.hpp"
#include "HandWidgetNotification.hpp"

// Create once (e.g. in your mod's initialization)
auto hw = std::make_unique<PortableWidget::HandWidget>("MyNotifHW");
auto notif = std::make_unique<PortableWidget::HandWidgetNotification>(hw.get(), "MyNotif");

// When the player is available, bind to a widget component:
SDK::UWidgetComponent* comp = player->W_GripDebug_R;  // or any UWidgetComponent
hw->Bind(comp);

// Show a notification (call from the game thread):
notif->Show(L"Hello from my mod!", 4.0f);

// Every tick:
notif->Tick(world, playerController);
```

---

## Quick Start — Menu

```cpp
#include "HandWidget.hpp"
#include "HandWidgetMenu.hpp"

auto hw = std::make_unique<PortableWidget::HandWidget>("MyMenuHW");
auto menu = std::make_unique<PortableWidget::HandWidgetMenu>(hw.get(), "MyMenu");

// Register pages (page 0 is the default root page):
menu->RegisterPage(0, "MAIN MENU", [](std::vector<PortableWidget::MenuItem>& items,
                                       PortableWidget::HandWidgetMenu& m) {
    items.push_back({"Do Something",
        []() -> std::string { return "OK"; },   // status callback
        []() { /* action */ }                    // on‑select callback
    });

    items.push_back({"Sub‑menu >",
        []() -> std::string { return ""; },
        [&m]() { m.NavigateToPage(1); },
        true  // isNavigation flag
    });
});

menu->RegisterPage(1, "SUB MENU", [](std::vector<PortableWidget::MenuItem>& items,
                                      PortableWidget::HandWidgetMenu& m) {
    items.push_back({"< Back",
        []() -> std::string { return ""; },
        [&m]() { m.GoBack(); }
    });
    items.push_back({"Sub‑item A",
        []() -> std::string { return "A"; },
        []() { /* action */ }
    });
});

// Bind when player is available:
hw->Bind(player->W_GripDebug_L);

// Open/close/toggle (wire to your input hooks):
menu->Toggle(world, pc);

// Navigation input (wire to thumbstick Y axis):
menu->OnNavigate(thumbstickY);   // returns true if consumed

// Selection input (wire to a button):
menu->OnSelect();                // returns true if consumed

// Every tick when menu is open:
if (menu->IsOpen())
    menu->Render(world, pc);
```

---

## Multiplexer — Sharing One Hand Between Menu + Notification

When both a menu and a notification layer need to live on the same hand,
use `HandWidgetMultiplexer`. Each layer gets its own `HandWidget` instance
because the multiplexer switches which one is `Bind()`'d to the shared
`UWidgetComponent`.

```cpp
#include "HandWidgetMultiplexer.hpp"

// Create separate HandWidgets for each layer:
auto menuHW  = std::make_unique<PortableWidget::HandWidget>("MenuHW");
auto notifHW = std::make_unique<PortableWidget::HandWidget>("NotifHW");

auto menu  = std::make_unique<PortableWidget::HandWidgetMenu>(menuHW.get(), "Menu");
auto notif = std::make_unique<PortableWidget::HandWidgetNotification>(notifHW.get(), "Notif");

auto mux = std::make_unique<PortableWidget::HandWidgetMultiplexer>("LeftHand");
mux->Bind(player->W_GripDebug_L);

// Add layers — higher priority wins when active:
auto& menuLayer = mux->AddLayer("Menu", PortableWidget::LayerPriority::Menu);
menuLayer.handWidget = menuHW.get();
menuLayer.isActiveQuery = [&]() { return menu->IsOpen(); };

auto& notifLayer = mux->AddLayer("Notification", PortableWidget::LayerPriority::Notification);
notifLayer.handWidget = notifHW.get();
notifLayer.isActiveQuery = [&]() { return notif->IsShowing() || notif->HasQueued(); };
notifLayer.onPauseChanged = [&](bool paused) { notif->SetPaused(paused); };

// Every tick:
mux->Tick(world, pc);
notif->Tick(world, pc);
if (menu->IsOpen())
    menu->Render(world, pc);
```

The priority enum values (higher = wins):

| Value | Name |
|-------|------|
| 0 | `LayerPriority::Background` |
| 100 | `LayerPriority::Notification` |
| 200 | `LayerPriority::Menu` |
| 300 | `LayerPriority::Alert` |

When a higher‑priority layer activates, the multiplexer pauses lower layers
(via `onPauseChanged`) so they stop ticking/auto‑hiding.

---

## Level Transitions

The game aggressively garbage‑collects widgets during level changes.
All cached `UObject*` pointers become stale. On level change:

1. Call `Shutdown()` on your integration object (resets all `unique_ptr`s).
2. Call `Initialize()` again to recreate objects.
3. The next `Tick()` call will re‑bind to the new player's widget components.

The `HandWidget` class validates its cached widget with `UKismetSystemLibrary::IsValid()`
before every operation, so a stale pointer won't crash — but you should still
reinitialize to get a clean state.

---

## `MenuItem` Structure

```cpp
struct MenuItem {
    std::string label;                        // Display text
    std::function<std::string()> statusFn;    // Returns status text (e.g. "ON"/"OFF")
    std::function<void()> action;             // Called on selection
    bool isNavigation = false;                // If true, shown with ">" indicator
};
```

Pages are registered with a `MenuPageBuilder` callback:
```cpp
using MenuPageBuilder = std::function<void(std::vector<MenuItem>&, HandWidgetMenu&)>;
```

The builder is called each time the page is entered, so item lists are always fresh
(e.g. dynamic loadout lists, NPC class lists).

---

## `NotificationMessage` Structure

```cpp
struct NotificationMessage {
    std::wstring text;
    float durationSeconds = 3.0f;
    std::wstring title = L"";
    int priority = 0;  // Reserved for future use
};
```

Use `Enqueue()` to add messages to a FIFO queue (shown one at a time after the
current message expires). Use `Show()` for an immediate display that replaces
the current message.

---

## TCP Testing Commands

The test harness (if integrated) provides these TCP commands:

| Command | Description |
|---------|-------------|
| `hwtest notif <text>` | Show notification on right hand |
| `hwtest notif_left <text>` | Show notification on left hand |
| `hwtest notif_queue <text>` | Enqueue notification on right hand |
| `hwtest menu` | Toggle left hand menu |
| `hwtest clear` | Clear all notifications |
| `hwtest status` | Show current system state |
| `hwtest bind` | Force re‑bind to hand widgets |

---

## ModMain Integration Pattern

```cpp
// In Run() — initialization:
PortableWidget::HandWidgetTestHarness::Get()->Initialize();
// After command handler is created:
PortableWidget::HandWidgetTestHarness::Get()->RegisterCommands(commandHandler);

// In OnTick(world) — every frame:
PortableWidget::HandWidgetTestHarness::Get()->Tick(world);

// On level change — cleanup:
PortableWidget::HandWidgetTestHarness::Get()->Shutdown();
PortableWidget::HandWidgetTestHarness::Get()->Initialize();
```

---

## Input Hook Wiring

The menu needs three input events routed from your `HookManager`:

| VR Input | Hook Function Name | Route To |
|----------|-------------------|----------|
| B/Y Started (`_17`) | `InpActEvt_IA_Button2_Left_..._17` | `OnToggleMenu(world)` if grip held, `OnSelect()` if menu open |
| Grip Left (`_26`) | `InpActEvt_IA_Grip_Left_..._26` | Track `gripHeld = true` |
| UnGrip Left (`_4`) | `InpActEvt_IA_UnGrip_Left_..._4` | Track `gripHeld = false` |
| Movement Started/Triggered (`_0`/`_1`) | `InpActEvt_IA_Movement_..._0/1` | `OnNavigate(thumbstickY)` — suppress game movement when menu open |

The hook functions should read `FInputActionValue` as `double*` (UE5 uses double‑precision
FVector). `doubles[1]` is the Y axis (forward/backward = menu up/down).

---

## FString Ring Buffer

`SDK::FString` is non‑owning in this SDK. `HandWidget::MakeStableFString()` uses a
thread‑local ring buffer of 64 `std::wstring` slots to keep the backing memory alive.
You don't need to manage this yourself — all `SetTitle()` / `SetBody()` / `SetButtons()`
calls go through this automatically.

---

## Files

| File | Description |
|------|-------------|
| `Mod/HandWidget.hpp` / `.cpp` | Layer 1: Surface management |
| `Mod/HandWidgetNotification.hpp` / `.cpp` | Layer 2a: Notification queue |
| `Mod/HandWidgetMenu.hpp` / `.cpp` | Layer 2b: Interactive menu |
| `Mod/HandWidgetMultiplexer.hpp` / `.cpp` | Layer 3: Priority multiplexer |
| `Mod/HandWidgetTestHarness.hpp` / `.cpp` | Mod‑specific integration + TCP test commands |
