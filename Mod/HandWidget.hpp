#pragma once

/*
AILEARNINGS:
- UWidgetComponent is the 3D widget surface attached to a bone (e.g. left/right hand).
- WBP_Confirmation_C is the only UMG widget known to work as a runtime-created widget on these surfaces.
  It has: Txt_Confirmation_Title (UTextBlock*), Txt_TextConfirm (UMultiLineEditableText*),
  Title/Description/Yes_Text/No_Text (FText properties), and Setupwidget/UpdateText methods.
- SetWidget(nullptr) then SetWidget(w) forces the component to re-render stale content.
- SetPivot(0.5, 0.95) anchors the widget near the bottom so it extends upward from the hand.
- K2_SetRelativeLocation shifts the widget in the component's local frame (X = forward = "up" for hand).
- FString is non-owning. MakeStableFString uses a ring buffer to keep wstring data alive.
- UKismetSystemLibrary::IsValid checks for pending-kill/GC'd objects.
- UWidgetBlueprintLibrary::Create needs a world context + player controller.
- The game GCs widgets aggressively during level transitions. Always re-validate cached pointers.
*/

#include <string>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>

// Full SDK include required because FString is imported into SDK via 'using namespace UC;'
// and forward-declaring it in namespace SDK creates a different type.
#include "..\CppSDK\SDK.hpp"

namespace PortableWidget
{
    // =========================================================================
    // HandWidget — Low-level management of a single UWidgetComponent surface.
    //
    // Responsibilities:
    //   - Create/destroy WBP_Confirmation_C on a given UWidgetComponent
    //   - Set title, body text, draw size, pivot, visibility
    //   - Validate that the cached widget hasn't been GC'd
    //   - Provide stable FString allocation
    //
    // This class is instance-based. Create one per UWidgetComponent you want
    // to manage. It holds no game-specific logic.
    // =========================================================================
    class HandWidget
    {
    public:
        // Construct with a debug name for logging (e.g. "LeftHand", "RightHand")
        explicit HandWidget(const std::string& debugName = "HandWidget");
        ~HandWidget();

        // Non-copyable, movable
        HandWidget(const HandWidget&) = delete;
        HandWidget& operator=(const HandWidget&) = delete;
        HandWidget(HandWidget&&) = default;
        HandWidget& operator=(HandWidget&&) = default;

        // --- Lifecycle ---

        // Bind to a UWidgetComponent. Does NOT create the widget yet.
        // Can be called again to rebind (will destroy previous widget first).
        void Bind(SDK::UWidgetComponent* component);

        // Unbind and destroy any created widget.
        void Unbind();

        // Returns true if bound to a component.
        bool IsBound() const;

        // Returns the bound component (may be null/invalid).
        SDK::UWidgetComponent* GetComponent() const { return component_; }

        // --- Widget creation/destruction ---

        // Ensure the WBP_Confirmation_C widget is created and attached.
        // Requires a valid world and player controller.
        // Returns true if widget is ready to use.
        bool EnsureWidget(SDK::UWorld* world, SDK::APlayerController* pc);

        // Destroy the widget and hide the component.
        void DestroyWidget();

        // Returns true if the widget has been created and is still valid.
        bool IsWidgetValid() const;

        // --- Content ---

        // Set the title text (top line, typically bold/colored).
        void SetTitle(const std::wstring& title);

        // Set the body text (multi-line content area).
        void SetBody(const std::wstring& body);

        // Set title color (affects the Txt_Confirmation_Title TextBlock).
        void SetTitleColor(float r, float g, float b, float a = 1.0f);

        // Set body text color.
        void SetBodyColor(float r, float g, float b, float a = 1.0f);

        // Set Yes/No button text (or empty to hide them).
        void SetButtons(const std::wstring& yesText, const std::wstring& noText);

        // Convenience: set title + body + buttons in one call.
        void SetContent(const std::wstring& title, const std::wstring& body,
                        const std::wstring& yesText = L"", const std::wstring& noText = L"");

        // --- Appearance ---

        // Set draw size in UMG units. Default is (600, 700).
        void SetDrawSize(float width, float height);

        // Set pivot point. (0.5, 0.95) is good for hand-mounted extending upward.
        void SetPivot(float x, float y);

        // Set a Z/forward offset in cm to lift widget away from attachment point.
        void SetForwardOffset(float cm);

        // Show or hide the widget component.
        void SetVisible(bool visible);

        // Check if currently visible.
        bool IsVisible() const;

        // Force a full redraw cycle (SetWidget(null) then SetWidget(w) + RequestRedraw).
        void ForceRedraw();

        // Request a lightweight redraw (RequestRedraw only).
        void RequestRedraw();

        // --- Utility ---

        // Get the debug name.
        const std::string& GetDebugName() const { return debugName_; }

        // Create a stable FString from a wstring (ring buffer, thread-local).
        static SDK::FString MakeStableFString(const std::wstring& value);

    private:
        std::string debugName_;
        SDK::UWidgetComponent* component_ = nullptr;
        SDK::UWBP_Confirmation_C* widget_ = nullptr;
        bool widgetCreated_ = false;

        // Cached state for logging throttling
        int logCount_ = 0;
        static constexpr int kLogLimit = 500;

        // Helper: validate a UObject is still alive
        static bool IsUObjectValid(const void* obj);

        // Helper: log with prefix
        void Log(const std::string& level, const std::string& msg) const;
    };

} // namespace PortableWidget
