/*
AILEARNINGS:
- HandWidgetNotification is a layer on top of HandWidget that manages timed messages.
- Messages show for a duration then auto-expire. Queue allows sequential display.
- When paused (e.g. by multiplexer), messages still expire but don't render.
  When unpaused, the next queued message renders immediately.
- Auto-hide mode hides the widget component when no messages are showing, to avoid
  a blank/stale widget floating on the player's hand.
*/

#include "HandWidgetNotification.hpp"
#include "HandWidget.hpp"
#include "Logging.hpp"

#include "..\CppSDK\SDK.hpp"

#include <sstream>
#include <chrono>

namespace PortableWidget
{
    // =========================================================================
    // Timing helper
    // =========================================================================
    uint64_t HandWidgetNotification::NowMs() const
    {
        auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    }

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================
    HandWidgetNotification::HandWidgetNotification(HandWidget* handWidget, const std::string& debugName)
        : handWidget_(handWidget), debugName_(debugName)
    {
        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Created, bound to HandWidget: "
                 << (handWidget_ ? handWidget_->GetDebugName() : "null"));
    }

    HandWidgetNotification::~HandWidgetNotification()
    {
        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Destroying");
        ClearAll();
    }

    // =========================================================================
    // Show messages
    // =========================================================================
    void HandWidgetNotification::Show(const std::wstring& text, float durationSeconds)
    {
        NotificationMessage msg;
        msg.text = text;
        msg.durationSeconds = durationSeconds;
        msg.title = L"MOD";
        msg.priority = 0;

        // Clear queue and show immediately
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.clear();
            queue_.push_back(msg);
        }

        showing_ = false; // Force re-render on next tick
        expiryMs_ = 0;

        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Show: text_len=" << text.size()
                 << " dur=" << durationSeconds);
    }

    void HandWidgetNotification::ShowWithTitle(const std::wstring& title, const std::wstring& text, float durationSeconds)
    {
        NotificationMessage msg;
        msg.text = text;
        msg.durationSeconds = durationSeconds;
        msg.title = title;
        msg.priority = 0;

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.clear();
            queue_.push_back(msg);
        }

        showing_ = false;
        expiryMs_ = 0;

        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] ShowWithTitle: title_len=" << title.size()
                 << " text_len=" << text.size() << " dur=" << durationSeconds);
    }

    void HandWidgetNotification::Enqueue(const NotificationMessage& msg)
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.push_back(msg);

        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Enqueue: queue_size=" << queue_.size()
                 << " priority=" << msg.priority);
    }

    void HandWidgetNotification::ClearAll()
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.clear();
        }
        showing_ = false;
        expiryMs_ = 0;
        ClearDisplay();

        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] ClearAll");
    }

    // =========================================================================
    // State
    // =========================================================================
    bool HandWidgetNotification::IsShowing() const
    {
        return showing_ && NowMs() < expiryMs_;
    }

    bool HandWidgetNotification::HasQueued() const
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(queueMutex_));
        return !queue_.empty();
    }

    // =========================================================================
    // Tick
    // =========================================================================
    void HandWidgetNotification::Tick(SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (!handWidget_)
            return;

        uint64_t now = NowMs();

        // Check if current message has expired
        if (showing_ && now >= expiryMs_)
        {
            if (logCount_++ < kLogLimit)
                LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Message expired");
            showing_ = false;
            expiryMs_ = 0;
        }

        // If not showing, try to dequeue next message
        if (!showing_)
        {
            NotificationMessage nextMsg;
            bool hasNext = false;

            {
                std::lock_guard<std::mutex> lock(queueMutex_);
                if (!queue_.empty())
                {
                    nextMsg = queue_.front();
                    queue_.pop_front();
                    hasNext = true;
                }
            }

            if (hasNext)
            {
                if (!paused_)
                {
                    RenderMessage(nextMsg, world, pc);
                    showing_ = true;
                    expiryMs_ = now + static_cast<uint64_t>(nextMsg.durationSeconds * 1000.0f);

                    if (logCount_++ < kLogLimit)
                        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Showing message, expires in "
                                 << nextMsg.durationSeconds << "s");
                }
                else
                {
                    // Paused: message expires immediately without rendering
                    if (logCount_++ < kLogLimit)
                        LOG_INFO("[HandWidgetNotification:" << debugName_ << "] Paused, skipping message");
                }
            }
            else if (!paused_ && autoHide_ && handWidget_->IsVisible())
            {
                // No messages and not paused: hide widget
                ClearDisplay();
            }
        }
    }

    // =========================================================================
    // Internal rendering
    // =========================================================================
    void HandWidgetNotification::RenderMessage(const NotificationMessage& msg, SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (!handWidget_)
            return;

        // Ensure widget is created
        if (!handWidget_->EnsureWidget(world, pc))
        {
            if (logCount_++ < kLogLimit)
                LOG_WARN("[HandWidgetNotification:" << debugName_ << "] RenderMessage: EnsureWidget failed");
            return;
        }

        // Set content
        handWidget_->SetTitle(msg.title);
        handWidget_->SetTitleColor(0.5f, 0.85f, 1.0f, 1.0f); // Cyan-ish
        handWidget_->SetBody(msg.text);
        handWidget_->SetBodyColor(1.0f, 1.0f, 1.0f, 1.0f);
        handWidget_->SetButtons(L"", L"");

        // Notification-appropriate draw size
        handWidget_->SetDrawSize(500.0f, 500.0f);

        // Show and force redraw
        handWidget_->SetVisible(true);
        handWidget_->ForceRedraw();

        if (logCount_++ < kLogLimit)
            LOG_INFO("[HandWidgetNotification:" << debugName_ << "] RenderMessage: rendered notification");
    }

    void HandWidgetNotification::ClearDisplay()
    {
        if (!handWidget_)
            return;

        if (autoHide_ && handWidget_->IsWidgetValid())
        {
            handWidget_->SetVisible(false);
        }
    }

} // namespace PortableWidget
