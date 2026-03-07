#pragma once

/*
AILEARNINGS:
- HandWidgetMultiplexer manages sharing a single UWidgetComponent between multiple layers
  (e.g. notifications and menu).
- Priority system: menu takes precedence over notifications. When menu is open,
  notifications are paused. When menu closes, notifications resume.
- Each layer owns a HandWidget instance but all share the same underlying UWidgetComponent.
  The multiplexer switches which HandWidget's content is displayed.
- Alternative approach (simpler): each layer has its OWN HandWidget, but only one is
  SetWidget()'d on the component at a time. This avoids interference between layers.
  This is the approach we implement here.
*/

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace SDK
{
    class UWidgetComponent;
    class UWorld;
    class APlayerController;
}

namespace PortableWidget
{
    class HandWidget;
    class HandWidgetNotification;
    class HandWidgetMenu;

    // =========================================================================
    // Layer priority (higher number = higher priority)
    // =========================================================================
    enum class LayerPriority : int
    {
        Notification = 10,
        Menu = 50,
        Debug = 100,    // For future use
    };

    // =========================================================================
    // Layer entry in the multiplexer
    // =========================================================================
    struct MultiplexerLayer
    {
        std::string name;
        LayerPriority priority;
        HandWidget* handWidget = nullptr;

        // Callbacks for pause/unpause (so the multiplexer can pause notifications etc.)
        std::function<void(bool paused)> onPauseChanged;

        // Callback to check if this layer wants to be active (e.g. menu.IsOpen())
        std::function<bool()> isActiveQuery;
    };

    // =========================================================================
    // HandWidgetMultiplexer — Manages sharing a UWidgetComponent between layers.
    //
    // The multiplexer ensures only one layer renders at a time on the component.
    // The highest-priority active layer gets exclusive access.
    // Lower-priority layers are paused (e.g. notification queue still ticks but
    // doesn't render).
    //
    // Each layer has its OWN HandWidget that creates its own WBP_Confirmation_C.
    // The multiplexer switches which widget is SetWidget()'d on the component.
    //
    // Usage:
    //   auto mux = HandWidgetMultiplexer("LeftHand");
    //   mux.Bind(player->W_GripDebug_L);
    //   
    //   auto& menuLayer = mux.AddLayer("Menu", LayerPriority::Menu);
    //   menuLayer.handWidget = &myMenuHandWidget;
    //   menuLayer.isActiveQuery = [&]{ return myMenu.IsOpen(); };
    //   
    //   auto& notifLayer = mux.AddLayer("Notif", LayerPriority::Notification);
    //   notifLayer.handWidget = &myNotifHandWidget;
    //   notifLayer.isActiveQuery = [&]{ return myNotif.IsShowing(); };
    //   notifLayer.onPauseChanged = [&](bool p){ myNotif.SetPaused(p); };
    //   
    //   // In tick:
    //   mux.Tick(world, pc);
    // =========================================================================
    class HandWidgetMultiplexer
    {
    public:
        explicit HandWidgetMultiplexer(const std::string& debugName = "Multiplexer");
        ~HandWidgetMultiplexer();

        // Bind to the shared UWidgetComponent
        void Bind(SDK::UWidgetComponent* component);

        // Get the bound component
        SDK::UWidgetComponent* GetComponent() const { return component_; }

        // Add a layer. Returns reference for configuration.
        MultiplexerLayer& AddLayer(const std::string& name, LayerPriority priority);

        // Remove a layer by name.
        void RemoveLayer(const std::string& name);

        // Tick: evaluate priorities and switch active layer.
        // Must be called every frame/tick.
        void Tick(SDK::UWorld* world, SDK::APlayerController* pc);

        // Get the name of the currently active layer (empty = none).
        const std::string& GetActiveLayerName() const { return activeLayerName_; }

        // Force a specific layer active (bypasses priority).
        void ForceLayer(const std::string& name);

        // Clear force and return to priority-based selection.
        void ClearForce();

        const std::string& GetDebugName() const { return debugName_; }

    private:
        std::string debugName_;
        SDK::UWidgetComponent* component_ = nullptr;
        std::vector<MultiplexerLayer> layers_;

        std::string activeLayerName_;
        std::string forcedLayerName_;

        int logCount_ = 0;
        static constexpr int kLogLimit = 200;

        // Switch the component to display a specific layer's widget
        void ActivateLayer(MultiplexerLayer* layer, SDK::UWorld* world, SDK::APlayerController* pc);
        void DeactivateAll();
    };

} // namespace PortableWidget
