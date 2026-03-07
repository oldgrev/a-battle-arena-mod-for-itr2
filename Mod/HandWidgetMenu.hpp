#pragma once

/*
AILEARNINGS:
- HandWidgetMenu is a portable, instance-based menu system that renders on a HandWidget.
- It supports multiple pages with navigation stack (back button support).
- Menu items have label, status callback, action callback, and navigation flag.
- Navigation (up/down/select/back/toggle) methods are called from external input hooks.
- The menu does NOT register its own input hooks — the host mod does that and calls
  the navigation methods. This keeps the menu portable.
- Anti-rebound for thumbstick is built into OnNavigate to prevent flick-bounce.
- Draw size auto-scales based on number of items.
*/

#include <string>
#include <vector>
#include <functional>
#include <chrono>

namespace SDK
{
    class UWorld;
    class APlayerController;
}

namespace PortableWidget
{
    // Forward declare
    class HandWidget;

    // =========================================================================
    // Menu item definition (same concept as VRMenuItem but portable)
    // =========================================================================
    struct MenuItem
    {
        std::string label;                                       // Display name
        std::function<std::string()> statusFn;                   // Returns current state text
        std::function<void()> actionFn;                          // Execute on select
        bool isNavigation = false;                               // True = navigates to sub-page
    };

    // =========================================================================
    // Menu page identifier — extensible via integer IDs
    // =========================================================================
    using MenuPageId = int;
    constexpr MenuPageId kMenuPageMain = 0;

    // =========================================================================
    // Menu page builder callback
    //   Called when entering a page to populate the items vector.
    //   Signature: void(std::vector<MenuItem>& items, HandWidgetMenu& menu)
    //   The builder receives a reference to the menu so it can call NavigateToPage.
    // =========================================================================
    class HandWidgetMenu; // forward
    using MenuPageBuilder = std::function<void(std::vector<MenuItem>&, HandWidgetMenu&)>;

    // =========================================================================
    // Navigation direction for anti-rebound
    // =========================================================================
    enum class MenuNavDirection { CENTER, UP, DOWN };

    // =========================================================================
    // HandWidgetMenu — Interactive menu on a HandWidget.
    //
    // Usage:
    //   auto hw = std::make_shared<HandWidget>("LeftHand");
    //   hw->Bind(someWidgetComponent);
    //   HandWidgetMenu menu(hw);
    //   menu.RegisterPage(0, "Main", [](auto& items, auto& menu) {
    //       items.push_back({"God Mode", ...});
    //   });
    //   menu.Open();  // Opens on page 0
    //
    //   // In input hooks:
    //   menu.OnToggle();     // Open/close
    //   menu.OnNavigate(y);  // Thumbstick Y
    //   menu.OnSelect();     // B/Y button
    // =========================================================================
    class HandWidgetMenu
    {
    public:
        explicit HandWidgetMenu(HandWidget* handWidget, const std::string& debugName = "Menu");
        ~HandWidgetMenu();

        // Non-copyable
        HandWidgetMenu(const HandWidgetMenu&) = delete;
        HandWidgetMenu& operator=(const HandWidgetMenu&) = delete;

        // --- Page registration ---

        // Register a page builder. Call before Open().
        void RegisterPage(MenuPageId id, const std::string& title, MenuPageBuilder builder);

        // --- Lifecycle ---

        // Open the menu on the given page (default: main page).
        // Needs world + pc for widget creation.
        void Open(SDK::UWorld* world, SDK::APlayerController* pc, MenuPageId startPage = kMenuPageMain);

        // Close the menu and hide the widget.
        void Close();

        // Toggle open/close. Returns true if menu is now open.
        bool Toggle(SDK::UWorld* world, SDK::APlayerController* pc);

        // Check if menu is open.
        bool IsOpen() const { return open_; }

        // --- Navigation (called from input hooks) ---

        // Navigate to a specific page (pushes current to stack).
        void NavigateToPage(MenuPageId page);

        // Go back one page (pops from stack).
        void GoBack();

        // Navigate up/down. Returns true if input was consumed.
        // thumbstickY > 0 = up, < 0 = down.
        bool OnNavigate(float thumbstickY);

        // Select the currently highlighted item. Returns true if consumed.
        bool OnSelect();

        // --- Rendering ---

        // Update the widget display. Call from tick when menu is open.
        void Render(SDK::UWorld* world, SDK::APlayerController* pc);

        // --- State ---

        int GetSelectedIndex() const { return selectedIndex_; }
        int GetItemCount() const { return static_cast<int>(items_.size()); }
        MenuPageId GetCurrentPage() const { return currentPage_; }

        HandWidget* GetHandWidget() const { return handWidget_; }
        const std::string& GetDebugName() const { return debugName_; }

        // --- Configuration ---

        // Base draw width/height and per-item height scaling.
        void SetDrawWidth(float w) { drawWidth_ = w; }
        void SetBaseHeight(float h) { baseHeight_ = h; }
        void SetPerItemHeight(float h) { perItemHeight_ = h; }

        // Navigation repeat rate in ms.
        void SetNavRepeatMs(int ms) { navRepeatMs_ = ms; }

        // Deadzone for thumbstick navigation.
        void SetNavDeadzone(float dz) { navDeadzone_ = dz; }

    private:
        HandWidget* handWidget_ = nullptr;
        std::string debugName_;

        bool open_ = false;
        MenuPageId currentPage_ = kMenuPageMain;
        std::vector<MenuPageId> pageStack_;
        int selectedIndex_ = 0;

        std::vector<MenuItem> items_;

        // Page registry: id -> (title, builder)
        struct PageInfo
        {
            std::string title;
            MenuPageBuilder builder;
        };
        std::vector<std::pair<MenuPageId, PageInfo>> pages_;

        // Navigation anti-rebound
        MenuNavDirection lastNavDirection_ = MenuNavDirection::CENTER;
        std::chrono::steady_clock::time_point lastNavTime_{};
        std::chrono::steady_clock::time_point lastSelectTime_{};

        // Draw config
        float drawWidth_ = 600.0f;
        float baseHeight_ = 700.0f;
        float perItemHeight_ = 50.0f;
        int navRepeatMs_ = 250;
        float navDeadzone_ = 0.5f;

        int logCount_ = 0;
        static constexpr int kLogLimit = 500;

        // Internal
        void BuildItems();
        std::wstring GetCurrentPageTitle() const;
        PageInfo* FindPage(MenuPageId id);
    };

} // namespace PortableWidget
