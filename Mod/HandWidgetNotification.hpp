#pragma once

/*
AILEARNINGS:
- HandWidgetNotification manages timed text messages on a HandWidget.
- Messages have a duration; when expired, the widget is cleared/hidden.
- A message queue allows stacking: next message shows when current expires.
- Priority levels allow urgent messages to preempt queued ones.
- The Tick() method must be called regularly (e.g. from game thread tick).
- When shared with a HandWidgetMenu via HandWidgetMultiplexer, the multiplexer
  controls when notifications get to render vs when menu takes over.
*/

#include <string>
#include <deque>
#include <mutex>
#include <cstdint>
#include <functional>

namespace PortableWidget
{
    // Forward declare
    class HandWidget;

    // =========================================================================
    // Notification message definition
    // =========================================================================
    struct NotificationMessage
    {
        std::wstring text;
        float durationSeconds = 3.0f;
        std::wstring title = L"MOD";  // Optional title override
        
        // Priority: higher = more important. 0 = normal.
        int priority = 0;
    };

    // =========================================================================
    // HandWidgetNotification — Timed notification display on a HandWidget.
    //
    // Usage:
    //   auto hw = std::make_shared<HandWidget>("RightHand");
    //   hw->Bind(someWidgetComponent);
    //   HandWidgetNotification notif(hw);
    //   notif.Show(L"Hello world!", 3.0f);
    //   // In tick loop:
    //   notif.Tick();
    // =========================================================================
    class HandWidgetNotification
    {
    public:
        // Construct with the HandWidget to render on.
        // The HandWidget must already be Bind()'d to a component.
        explicit HandWidgetNotification(HandWidget* handWidget, const std::string& debugName = "Notification");
        ~HandWidgetNotification();

        // Non-copyable
        HandWidgetNotification(const HandWidgetNotification&) = delete;
        HandWidgetNotification& operator=(const HandWidgetNotification&) = delete;

        // --- Show messages ---

        // Show a simple message for the given duration.
        void Show(const std::wstring& text, float durationSeconds = 3.0f);

        // Show a message with a custom title.
        void ShowWithTitle(const std::wstring& title, const std::wstring& text, float durationSeconds = 3.0f);

        // Enqueue a message (shown after current message expires).
        void Enqueue(const NotificationMessage& msg);

        // Clear the current message and the entire queue.
        void ClearAll();

        // --- State ---

        // Returns true if a message is currently being displayed.
        bool IsShowing() const;

        // Returns true if there are queued messages waiting.
        bool HasQueued() const;

        // --- Tick ---

        // Must be called regularly. Handles expiry and queue advancement.
        // Needs world + pc for lazy widget creation via HandWidget::EnsureWidget.
        void Tick(SDK::UWorld* world, SDK::APlayerController* pc);

        // --- Control ---

        // Pause/unpause rendering. When paused, Tick() still expires messages
        // but doesn't render. Used by multiplexer when menu takes over.
        void SetPaused(bool paused) { paused_ = paused; }
        bool IsPaused() const { return paused_; }

        // If true, the widget is hidden when no message is showing.
        // If false, the widget stays visible but with empty text. Default: true.
        void SetAutoHide(bool autoHide) { autoHide_ = autoHide; }

        // Get the underlying HandWidget
        HandWidget* GetHandWidget() const { return handWidget_; }

    private:
        HandWidget* handWidget_ = nullptr;
        std::string debugName_;

        // Current message state
        bool showing_ = false;
        uint64_t expiryMs_ = 0;

        // Message queue (front = next to show)
        std::mutex queueMutex_;
        std::deque<NotificationMessage> queue_;

        bool paused_ = false;
        bool autoHide_ = true;

        int logCount_ = 0;
        static constexpr int kLogLimit = 200;

        // Helpers
        uint64_t NowMs() const;
        void RenderMessage(const NotificationMessage& msg, SDK::UWorld* world, SDK::APlayerController* pc);
        void ClearDisplay();
    };

} // namespace PortableWidget

// Forward declare SDK types needed by Tick signature
namespace SDK
{
    class UWorld;
    class APlayerController;
}
