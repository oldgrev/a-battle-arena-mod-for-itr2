/*
AILEARNINGS:
- HandWidgetMultiplexer owns switching logic between layers on a shared UWidgetComponent.
- Each layer has its own HandWidget (which creates its own WBP_Confirmation_C widget).
- Only one layer's widget is SetWidget()'d on the component at a time.
- When switching layers, the previous layer's widget is detached (SetWidget(nullptr)),
  then the new layer's widget is attached (SetWidget(w)).
- Layers are sorted by priority; highest-priority ACTIVE layer wins.
- Paused layers still tick for expiry etc. but don't render.
*/

#include "HandWidgetMultiplexer.hpp"
#include "HandWidget.hpp"
#include "Logging.hpp"
#include "HookManager.hpp"

#include "..\CppSDK\SDK.hpp"

#include <algorithm>
#include <sstream>

namespace PortableWidget
{
    // =========================================================================
    // Constructor / Destructor
    // =========================================================================
    HandWidgetMultiplexer::HandWidgetMultiplexer(const std::string& debugName)
        : debugName_(debugName)
    {
        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Created");
    }

    HandWidgetMultiplexer::~HandWidgetMultiplexer()
    {
        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Destroying");
        DeactivateAll();
    }

    // =========================================================================
    // Bind
    // =========================================================================
    void HandWidgetMultiplexer::Bind(SDK::UWidgetComponent* component)
    {
        component_ = component;

        if (component_)
            LOG_INFO("[HandWidgetMux:" << debugName_ << "] Bound to component: " << component_->GetFullName());
        else
            LOG_WARN("[HandWidgetMux:" << debugName_ << "] Bound to null component");
    }

    // =========================================================================
    // Layer management
    // =========================================================================
    MultiplexerLayer& HandWidgetMultiplexer::AddLayer(const std::string& name, LayerPriority priority)
    {
        // Check for duplicate
        for (auto& layer : layers_)
        {
            if (layer.name == name)
            {
                LOG_WARN("[HandWidgetMux:" << debugName_ << "] Layer '" << name << "' already exists, returning existing");
                return layer;
            }
        }

        MultiplexerLayer layer;
        layer.name = name;
        layer.priority = priority;
        layers_.push_back(layer);

        // Sort by priority descending so highest priority is first
        std::sort(layers_.begin(), layers_.end(),
            [](const MultiplexerLayer& a, const MultiplexerLayer& b) {
                return static_cast<int>(a.priority) > static_cast<int>(b.priority);
            });

        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Added layer '" << name
                 << "' priority=" << static_cast<int>(priority)
                 << " total_layers=" << layers_.size());

        // Find and return the layer we just added (it may have moved due to sort)
        for (auto& l : layers_)
        {
            if (l.name == name)
                return l;
        }

        // Should never reach here
        return layers_.back();
    }

    void HandWidgetMultiplexer::RemoveLayer(const std::string& name)
    {
        if (activeLayerName_ == name)
        {
            DeactivateAll();
        }

        layers_.erase(
            std::remove_if(layers_.begin(), layers_.end(),
                [&name](const MultiplexerLayer& l) { return l.name == name; }),
            layers_.end());

        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Removed layer '" << name
                 << "' remaining=" << layers_.size());
    }

    // =========================================================================
    // Tick
    // =========================================================================
    void HandWidgetMultiplexer::Tick(SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (!component_)
            return;

        // Determine which layer should be active
        MultiplexerLayer* winner = nullptr;
        std::string winnerName;

        // If forced, use that layer
        if (!forcedLayerName_.empty())
        {
            for (auto& layer : layers_)
            {
                if (layer.name == forcedLayerName_)
                {
                    winner = &layer;
                    winnerName = layer.name;
                    break;
                }
            }
        }

        // Otherwise, highest priority active layer wins
        if (!winner)
        {
            for (auto& layer : layers_)
            {
                bool isActive = layer.isActiveQuery ? layer.isActiveQuery() : false;
                if (isActive)
                {
                    winner = &layer;
                    winnerName = layer.name;
                    break; // layers sorted by priority descending
                }
            }
        }

        // Has the active layer changed?
        if (winnerName != activeLayerName_)
        {
            if (logCount_++ < kLogLimit)
                LOG_INFO("[HandWidgetMux:" << debugName_ << "] Active layer change: '"
                         << activeLayerName_ << "' -> '" << winnerName << "'");

            // Deactivate old layer
            if (!activeLayerName_.empty())
            {
                for (auto& layer : layers_)
                {
                    if (layer.name == activeLayerName_ && layer.onPauseChanged)
                    {
                        layer.onPauseChanged(true); // pause old layer
                        break;
                    }
                }
            }

            // Activate new layer
            if (winner)
            {
                ActivateLayer(winner, world, pc);
                if (winner->onPauseChanged)
                    winner->onPauseChanged(false); // unpause new layer
            }
            else
            {
                // No active layer — hide the component
                DeactivateAll();
            }

            activeLayerName_ = winnerName;
        }
    }

    // =========================================================================
    // Force layer
    // =========================================================================
    void HandWidgetMultiplexer::ForceLayer(const std::string& name)
    {
        forcedLayerName_ = name;
        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Force layer: '" << name << "'");
    }

    void HandWidgetMultiplexer::ClearForce()
    {
        forcedLayerName_.clear();
        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Force cleared");
    }

    // =========================================================================
    // Internal
    // =========================================================================
    void HandWidgetMultiplexer::ActivateLayer(MultiplexerLayer* layer, SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (!component_ || !layer || !layer->handWidget)
            return;

        Mod::ScopedProcessEventGuard guard;

        // Ensure the layer's widget exists
        // The HandWidget needs to be bound to the SAME component
        // But since we're multiplexing, we'll bind the winning layer's HandWidget to the component
        layer->handWidget->Bind(component_);
        layer->handWidget->EnsureWidget(world, pc);

        LOG_INFO("[HandWidgetMux:" << debugName_ << "] Activated layer '" << layer->name << "'");
    }

    void HandWidgetMultiplexer::DeactivateAll()
    {
        if (!component_)
            return;

        try
        {
            Mod::ScopedProcessEventGuard guard;

            // Check if component is still valid before calling methods on it
            if (SDK::UKismetSystemLibrary::IsValid(component_))
            {
                component_->SetWidget(nullptr);
                component_->SetVisibility(false, true);
            }
        }
        catch (...)
        {
            LOG_ERROR("[HandWidgetMux:" << debugName_ << "] Exception in DeactivateAll");
        }

        activeLayerName_.clear();

        LOG_INFO("[HandWidgetMux:" << debugName_ << "] All layers deactivated");
    }

} // namespace PortableWidget
