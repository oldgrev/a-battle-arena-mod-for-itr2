/*
AILEARNINGS:
- HandWidgetMenu manages an interactive VR menu rendered on a HandWidget.
- Pages are registered with builders that populate menu items dynamically.
- Navigation stack allows back-button traversal through sub-menus.
- Anti-rebound: thumbstick must return to deadzone before accepting opposite direction.
- Draw size auto-adjusts based on item count to prevent clipping.
- The menu menu does NOT own the HandWidget lifetime — caller manages that.
*/

#include "HandWidgetMenu.hpp"
#include "HandWidget.hpp"
#include "Logging.hpp"

#include "..\CppSDK\SDK.hpp"

#include <sstream>
#include <cmath>
#include <algorithm>

namespace PortableWidget
{
    // =========================================================================
    // Constructor / Destructor
    // =========================================================================
    HandWidgetMenu::HandWidgetMenu(HandWidget* handWidget, const std::string& debugName)
        : handWidget_(handWidget), debugName_(debugName)
    {
        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Created, bound to HandWidget: "
                 << (handWidget_ ? handWidget_->GetDebugName() : "null"));
    }

    HandWidgetMenu::~HandWidgetMenu()
    {
        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Destroying");
        Close();
    }

    // =========================================================================
    // Page registration
    // =========================================================================
    void HandWidgetMenu::RegisterPage(MenuPageId id, const std::string& title, MenuPageBuilder builder)
    {
        // Check if page already registered (overwrite)
        for (auto& p : pages_)
        {
            if (p.first == id)
            {
                p.second.title = title;
                p.second.builder = builder;
                LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Page " << id << " (" << title << ") re-registered");
                return;
            }
        }

        pages_.push_back({id, PageInfo{title, builder}});
        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Page " << id << " (" << title << ") registered. Total pages: " << pages_.size());
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================
    void HandWidgetMenu::Open(SDK::UWorld* world, SDK::APlayerController* pc, MenuPageId startPage)
    {
        if (open_)
        {
            LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Already open, re-opening on page " << startPage);
        }

        currentPage_ = startPage;
        pageStack_.clear();
        selectedIndex_ = 0;
        lastNavDirection_ = MenuNavDirection::CENTER;

        open_ = true;

        BuildItems();
        Render(world, pc);

        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Opened on page " << startPage
                 << " with " << items_.size() << " items");
    }

    void HandWidgetMenu::Close()
    {
        if (!open_)
            return;

        open_ = false;
        currentPage_ = kMenuPageMain;
        pageStack_.clear();
        selectedIndex_ = 0;
        items_.clear();

        if (handWidget_)
        {
            handWidget_->DestroyWidget();
        }

        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] Closed");
    }

    bool HandWidgetMenu::Toggle(SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (open_)
        {
            Close();
            return false;
        }
        else
        {
            Open(world, pc);
            return true;
        }
    }

    // =========================================================================
    // Navigation
    // =========================================================================
    void HandWidgetMenu::NavigateToPage(MenuPageId page)
    {
        if (page != currentPage_)
        {
            pageStack_.push_back(currentPage_);
        }
        currentPage_ = page;
        selectedIndex_ = 0;
        BuildItems();

        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] NavigateToPage: " << page
                 << " items=" << items_.size() << " stack_depth=" << pageStack_.size());
    }

    void HandWidgetMenu::GoBack()
    {
        if (pageStack_.empty())
        {
            LOG_INFO("[HandWidgetMenu:" << debugName_ << "] GoBack: stack empty, closing");
            Close();
            return;
        }

        currentPage_ = pageStack_.back();
        pageStack_.pop_back();
        selectedIndex_ = 0;
        BuildItems();

        LOG_INFO("[HandWidgetMenu:" << debugName_ << "] GoBack: now on page " << currentPage_
                 << " items=" << items_.size());
    }

    bool HandWidgetMenu::OnNavigate(float thumbstickY)
    {
        if (!open_)
            return false;

        // Deadzone: reset to CENTER
        if (std::abs(thumbstickY) < navDeadzone_)
        {
            if (lastNavDirection_ != MenuNavDirection::CENTER)
            {
                if (logCount_++ < kLogLimit)
                    LOG_INFO("[HandWidgetMenu:" << debugName_ << ":OnNavigate] y=" << thumbstickY
                             << " deadzone -> CENTER");
            }
            lastNavDirection_ = MenuNavDirection::CENTER;
            return true; // suppress input while menu open
        }

        // Determine direction
        MenuNavDirection intended = (thumbstickY > 0) ? MenuNavDirection::UP : MenuNavDirection::DOWN;

        // Anti-rebound: must return to CENTER before reversing
        if (lastNavDirection_ != MenuNavDirection::CENTER && lastNavDirection_ != intended)
        {
            if (logCount_++ < kLogLimit)
                LOG_INFO("[HandWidgetMenu:" << debugName_ << ":OnNavigate] y=" << thumbstickY
                         << " REBOUND BLOCKED");
            return true;
        }

        // Rate limit
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNavTime_).count();
        if (elapsed < navRepeatMs_)
            return true;

        lastNavTime_ = now;
        lastNavDirection_ = intended;

        int prevIndex = selectedIndex_;

        if (intended == MenuNavDirection::DOWN)
        {
            selectedIndex_++;
            if (selectedIndex_ >= static_cast<int>(items_.size()))
                selectedIndex_ = 0;
        }
        else
        {
            selectedIndex_--;
            if (selectedIndex_ < 0)
                selectedIndex_ = static_cast<int>(items_.size()) - 1;
        }

        if (logCount_++ < kLogLimit)
            LOG_INFO("[HandWidgetMenu:" << debugName_ << ":OnNavigate] "
                     << prevIndex << " -> " << selectedIndex_
                     << " (of " << items_.size() << ")");

        return true; // suppress input
    }

    bool HandWidgetMenu::OnSelect()
    {
        if (!open_)
            return false;

        // Debounce
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSelectTime_).count();
        if (elapsed < 300)
            return true;
        lastSelectTime_ = now;

        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size()))
        {
            LOG_WARN("[HandWidgetMenu:" << debugName_ << ":OnSelect] selectedIndex " << selectedIndex_
                     << " out of range [0, " << items_.size() << ")");
            return true;
        }

        auto& item = items_[selectedIndex_];

        LOG_INFO("[HandWidgetMenu:" << debugName_ << ":OnSelect] Executing item " << selectedIndex_
                 << " (" << item.label << ")");

        try
        {
            if (item.actionFn)
                item.actionFn();
        }
        catch (...)
        {
            LOG_ERROR("[HandWidgetMenu:" << debugName_ << ":OnSelect] Exception in action for item: " << item.label);
        }

        // Rebuild items in case action changed state (e.g. navigated to new page)
        BuildItems();

        return true; // suppress input
    }

    // =========================================================================
    // Rendering
    // =========================================================================
    void HandWidgetMenu::Render(SDK::UWorld* world, SDK::APlayerController* pc)
    {
        if (!open_ || !handWidget_)
            return;

        if (!handWidget_->EnsureWidget(world, pc))
        {
            if (logCount_++ < kLogLimit)
                LOG_WARN("[HandWidgetMenu:" << debugName_ << "] Render: EnsureWidget failed");
            return;
        }

        // Build menu text
        std::wostringstream body;

        for (int i = 0; i < static_cast<int>(items_.size()); i++)
        {
            auto& item = items_[i];

            // Selection indicator
            if (i == selectedIndex_)
                body << L">> ";
            else
                body << L"   ";

            // Label
            body << std::wstring(item.label.begin(), item.label.end());

            // Status
            std::string status;
            try
            {
                if (item.statusFn)
                    status = item.statusFn();
            }
            catch (...) { status = "?"; }

            if (!status.empty())
            {
                body << L" [";
                body << std::wstring(status.begin(), status.end());
                body << L"]";
            }

            if (i < static_cast<int>(items_.size()) - 1)
                body << L"\n";
        }

        body << L"\n\n[grip+B/Y]=close  [stick]=nav  [B/Y]=select";

        // Set content
        std::wstring pageTitle = GetCurrentPageTitle();
        handWidget_->SetTitle(pageTitle);
        handWidget_->SetTitleColor(1.0f, 1.0f, 1.0f, 1.0f);
        handWidget_->SetBody(body.str());
        handWidget_->SetBodyColor(1.0f, 1.0f, 1.0f, 1.0f);
        handWidget_->SetButtons(L"", L"");

        // Auto-size based on item count
        int itemCount = static_cast<int>(items_.size());
        float height = (std::max)(baseHeight_, static_cast<float>(itemCount) * perItemHeight_);
        handWidget_->SetDrawSize(drawWidth_, height);

        // Pivot and offset for hand-mounted display
        handWidget_->SetPivot(0.5f, 0.95f);
        float forwardOffset = static_cast<float>(itemCount) * 0.50f;
        handWidget_->SetForwardOffset(forwardOffset);

        // Ensure visible and request redraw
        handWidget_->SetVisible(true);
        handWidget_->RequestRedraw();
    }

    // =========================================================================
    // Internal
    // =========================================================================
    void HandWidgetMenu::BuildItems()
    {
        items_.clear();

        PageInfo* page = FindPage(currentPage_);
        if (!page)
        {
            LOG_WARN("[HandWidgetMenu:" << debugName_ << "] BuildItems: page " << currentPage_ << " not found");
            return;
        }

        if (page->builder)
        {
            page->builder(items_, *this);
        }

        // Clamp selected index
        if (selectedIndex_ >= static_cast<int>(items_.size()))
            selectedIndex_ = items_.empty() ? 0 : static_cast<int>(items_.size()) - 1;
        if (selectedIndex_ < 0)
            selectedIndex_ = 0;

        if (logCount_++ < kLogLimit)
            LOG_INFO("[HandWidgetMenu:" << debugName_ << "] BuildItems: page=" << currentPage_
                     << " items=" << items_.size() << " selected=" << selectedIndex_);
    }

    std::wstring HandWidgetMenu::GetCurrentPageTitle() const
    {
        for (const auto& p : pages_)
        {
            if (p.first == currentPage_)
            {
                std::wstring title(p.second.title.begin(), p.second.title.end());
                return L"=== " + title + L" ===";
            }
        }
        return L"=== MENU ===";
    }

    HandWidgetMenu::PageInfo* HandWidgetMenu::FindPage(MenuPageId id)
    {
        for (auto& p : pages_)
        {
            if (p.first == id)
                return &p.second;
        }
        return nullptr;
    }

} // namespace PortableWidget
