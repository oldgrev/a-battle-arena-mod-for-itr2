/*
AILEARNINGS:
- HandWidget is the low-level building block for all VR hand-mounted widget surfaces.
- It manages a single UWidgetComponent + WBP_Confirmation_C pair.
- The widget is created lazily via EnsureWidget() and destroyed via DestroyWidget().
- ForceRedraw() does SetWidget(null)+SetWidget(w) which is necessary when text changes
  aren't picked up by the component's render pipeline (common in VR/shipping builds).
- MakeStableFString uses a thread-local ring buffer so FString's non-owning pointer stays valid.
- All methods are heavily logged because this is exploratory VR mod code.
*/

#include "HandWidget.hpp"
#include "Logging.hpp"
#include "HookManager.hpp"   // For ScopedProcessEventGuard

#include "..\CppSDK\SDK.hpp"

#include <array>
#include <sstream>

namespace PortableWidget
{
    // =========================================================================
    // Stable FString ring buffer (thread-local, same pattern as VRMenuSubsystem)
    // =========================================================================
    SDK::FString HandWidget::MakeStableFString(const std::wstring& value)
    {
        static thread_local std::array<std::wstring, 64> ring;
        static thread_local uint32_t ringIndex = 0;

        std::wstring& slot = ring[ringIndex++ % ring.size()];
        slot = value;
        return SDK::FString(slot.c_str());
    }

    // =========================================================================
    // Helper: UObject validity check
    // =========================================================================
    bool HandWidget::IsUObjectValid(const void* obj)
    {
        if (!obj) return false;
        return SDK::UKismetSystemLibrary::IsValid(reinterpret_cast<const SDK::UObject*>(obj));
    }

    // =========================================================================
    // Logging helper
    // =========================================================================
    void HandWidget::Log(const std::string& level, const std::string& msg) const
    {
        std::ostringstream oss;
        oss << "[HandWidget:" << debugName_ << "] " << msg;
        if (level == "INFO")
            LOG_INFO(oss.str());
        else if (level == "WARN")
            LOG_WARN(oss.str());
        else if (level == "ERROR")
            LOG_ERROR(oss.str());
        else
            LOG_DEBUG(oss.str());
    }

    // =========================================================================
    // Constructor / Destructor
    // =========================================================================
    HandWidget::HandWidget(const std::string& debugName)
        : debugName_(debugName)
    {
        Log("INFO", "Created instance");
    }

    HandWidget::~HandWidget()
    {
        Log("INFO", "Destroying instance");
        DestroyWidget();
    }

    // =========================================================================
    // Bind / Unbind
    // =========================================================================
    void HandWidget::Bind(SDK::UWidgetComponent* component)
    {
        if (component_ == component)
        {
            Log("INFO", "Bind called with same component, no-op");
            return;
        }

        if (component_)
        {
            Log("INFO", "Bind called with new component, destroying old widget first");
            DestroyWidget();
        }

        component_ = component;
        if (component_)
        {
            std::ostringstream oss;
            oss << "Bound to component: " << component_->GetFullName();
            Log("INFO", oss.str());
        }
        else
        {
            Log("WARN", "Bound to null component");
        }
    }

    void HandWidget::Unbind()
    {
        Log("INFO", "Unbinding");
        DestroyWidget();
        component_ = nullptr;
    }

    bool HandWidget::IsBound() const
    {
        return component_ != nullptr && IsUObjectValid(component_);
    }

    // =========================================================================
    // Widget creation / destruction
    // =========================================================================
    bool HandWidget::EnsureWidget(SDK::UWorld* world, SDK::APlayerController* pc)
    {
        // Validate component
        if (!component_)
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "EnsureWidget: component is null");
            return false;
        }

        if (!IsUObjectValid(component_))
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "EnsureWidget: component is invalid (GC'd?)");
            component_ = nullptr;
            widget_ = nullptr;
            widgetCreated_ = false;
            return false;
        }

        // Check if existing widget is still valid
        if (widget_ && widgetCreated_)
        {
            if (IsUObjectValid(widget_))
            {
                return true; // Widget still good
            }
            else
            {
                if (logCount_++ < kLogLimit)
                    Log("WARN", "EnsureWidget: cached widget became invalid (GC'd), recreating");
                widget_ = nullptr;
                widgetCreated_ = false;
            }
        }

        // Need to create widget
        if (!world)
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "EnsureWidget: world is null");
            return false;
        }

        if (!pc)
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "EnsureWidget: player controller is null");
            return false;
        }

        SDK::UClass* widgetClass = SDK::UWBP_Confirmation_C::StaticClass();
        if (!widgetClass)
        {
            Log("ERROR", "EnsureWidget: WBP_Confirmation_C::StaticClass() returned null");
            return false;
        }

        Log("INFO", "EnsureWidget: Creating WBP_Confirmation_C widget...");

        Mod::ScopedProcessEventGuard guard;
        SDK::UUserWidget* rawWidget = SDK::UWidgetBlueprintLibrary::Create(world, widgetClass, pc);
        if (!rawWidget)
        {
            Log("ERROR", "EnsureWidget: WidgetBlueprintLibrary::Create returned null");
            return false;
        }

        if (!rawWidget->IsA(widgetClass))
        {
            Log("ERROR", "EnsureWidget: Created widget is not WBP_Confirmation_C");
            return false;
        }

        widget_ = static_cast<SDK::UWBP_Confirmation_C*>(rawWidget);
        Log("INFO", std::string("EnsureWidget: Widget created: ") + rawWidget->GetFullName());

        // Attach to component
        component_->SetWidget(widget_);
        component_->SetVisibility(true, true);
        component_->SetHiddenInGame(false, true);
        component_->SetTwoSided(true);

        // Default draw size
        SDK::FVector2D defaultSize{600.0, 700.0};
        component_->SetDrawSize(defaultSize);

        // Default pivot for hand-mounted (bottom-center, extends upward)
        SDK::FVector2D defaultPivot{0.5, 0.95};
        component_->SetPivot(defaultPivot);

        widgetCreated_ = true;

        // Clear button text by default (notifications/menus set their own)
        SetButtons(L"", L"");

        Log("INFO", "EnsureWidget: Widget created and attached successfully");
        return true;
    }

    void HandWidget::DestroyWidget()
    {
        if (!widgetCreated_ && !widget_)
        {
            Log("INFO", "DestroyWidget: No widget to destroy");
            return;
        }

        Log("INFO", "DestroyWidget: Destroying widget");

        // Clear the widget from the component
        if (component_ && IsUObjectValid(component_))
        {
            try
            {
                Mod::ScopedProcessEventGuard guard;
                component_->SetWidget(nullptr);
                component_->SetVisibility(false, true);
                Log("INFO", "DestroyWidget: Cleared component widget and hidden");
            }
            catch (...)
            {
                Log("ERROR", "DestroyWidget: Exception clearing component");
            }
        }

        // We can't properly destroy UObjects, just null out our pointers
        widget_ = nullptr;
        widgetCreated_ = false;

        Log("INFO", "DestroyWidget: Done");
    }

    bool HandWidget::IsWidgetValid() const
    {
        return widget_ != nullptr && widgetCreated_ && IsUObjectValid(widget_);
    }

    // =========================================================================
    // Content setters
    // =========================================================================
    void HandWidget::SetTitle(const std::wstring& title)
    {
        if (!IsWidgetValid())
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "SetTitle: widget not valid");
            return;
        }

        Mod::ScopedProcessEventGuard guard;

        SDK::FString fstr = MakeStableFString(title);
        SDK::FText ftext = SDK::UKismetTextLibrary::Conv_StringToText(fstr);

        widget_->Title = ftext;

        if (widget_->Txt_Confirmation_Title)
        {
            widget_->Txt_Confirmation_Title->SetText(ftext);
        }
        else
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "SetTitle: Txt_Confirmation_Title is null");
        }
    }

    void HandWidget::SetBody(const std::wstring& body)
    {
        if (!IsWidgetValid())
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "SetBody: widget not valid");
            return;
        }

        Mod::ScopedProcessEventGuard guard;

        SDK::FString fstr = MakeStableFString(body);
        SDK::FText ftext = SDK::UKismetTextLibrary::Conv_StringToText(fstr);

        widget_->Description = ftext;

        if (widget_->Txt_TextConfirm)
        {
            widget_->Txt_TextConfirm->SetText(ftext);

            // Force style update to trigger re-render
            SDK::FTextBlockStyle style = widget_->Txt_TextConfirm->WidgetStyle;
            style.ColorAndOpacity.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
            widget_->Txt_TextConfirm->SetWidgetStyle(style);
            widget_->Txt_TextConfirm->SetRenderOpacity(1.0f);
        }
        else
        {
            if (logCount_++ < kLogLimit)
                Log("WARN", "SetBody: Txt_TextConfirm is null");
        }
    }

    void HandWidget::SetTitleColor(float r, float g, float b, float a)
    {
        if (!IsWidgetValid()) return;

        Mod::ScopedProcessEventGuard guard;

        if (widget_->Txt_Confirmation_Title)
        {
            SDK::FSlateColor col;
            col.SpecifiedColor = SDK::FLinearColor{r, g, b, a};
            widget_->Txt_Confirmation_Title->SetColorAndOpacity(col);
        }
    }

    void HandWidget::SetBodyColor(float r, float g, float b, float a)
    {
        if (!IsWidgetValid()) return;

        Mod::ScopedProcessEventGuard guard;

        if (widget_->Txt_TextConfirm)
        {
            SDK::FTextBlockStyle style = widget_->Txt_TextConfirm->WidgetStyle;
            style.ColorAndOpacity.SpecifiedColor = SDK::FLinearColor{r, g, b, a};
            widget_->Txt_TextConfirm->SetWidgetStyle(style);
        }
    }

    void HandWidget::SetButtons(const std::wstring& yesText, const std::wstring& noText)
    {
        if (!IsWidgetValid()) return;

        Mod::ScopedProcessEventGuard guard;

        SDK::FString yStr = MakeStableFString(yesText);
        SDK::FText yText = SDK::UKismetTextLibrary::Conv_StringToText(yStr);
        widget_->Yes_Text = yText;

        SDK::FString nStr = MakeStableFString(noText);
        SDK::FText nText = SDK::UKismetTextLibrary::Conv_StringToText(nStr);
        widget_->No_Text = nText;
    }

    void HandWidget::SetContent(const std::wstring& title, const std::wstring& body,
                                const std::wstring& yesText, const std::wstring& noText)
    {
        SetTitle(title);
        SetBody(body);
        SetButtons(yesText, noText);
    }

    // =========================================================================
    // Appearance
    // =========================================================================
    void HandWidget::SetDrawSize(float width, float height)
    {
        if (!component_ || !IsUObjectValid(component_)) return;

        Mod::ScopedProcessEventGuard guard;

        SDK::FVector2D size{static_cast<double>(width), static_cast<double>(height)};
        component_->SetDrawSize(size);

        // std::ostringstream oss;
        // oss << "SetDrawSize: " << width << "x" << height;
        // Log("INFO", oss.str());
    }

    void HandWidget::SetPivot(float x, float y)
    {
        if (!component_ || !IsUObjectValid(component_)) return;

        Mod::ScopedProcessEventGuard guard;

        SDK::FVector2D pivot{static_cast<double>(x), static_cast<double>(y)};
        component_->SetPivot(pivot);
    }

    void HandWidget::SetForwardOffset(float cm)
    {
        if (!component_ || !IsUObjectValid(component_)) return;

        Mod::ScopedProcessEventGuard guard;

        // In the hand's local frame: X = forward (away from hand)
        SDK::FHitResult dummyHit{};
        component_->K2_SetRelativeLocation(
            SDK::FVector{static_cast<double>(cm), 0.0, 0.0}, false, &dummyHit, true);
    }

    void HandWidget::SetVisible(bool visible)
    {
        if (!component_ || !IsUObjectValid(component_)) return;

        Mod::ScopedProcessEventGuard guard;

        component_->SetVisibility(visible, true);
        component_->SetHiddenInGame(!visible, true);

        if (visible)
            RequestRedraw();
    }

    bool HandWidget::IsVisible() const
    {
        if (!component_ || !IsUObjectValid(component_)) return false;
        return component_->IsVisible();
    }

    void HandWidget::ForceRedraw()
    {
        if (!component_ || !IsUObjectValid(component_) || !widget_) return;

        Mod::ScopedProcessEventGuard guard;

        // The nuclear option: detach and re-attach widget to force full re-render
        component_->SetWidget(nullptr);
        component_->SetWidget(widget_);
        component_->RequestRedraw();

        Log("INFO", "ForceRedraw: Widget re-attached and redraw requested");
    }

    void HandWidget::RequestRedraw()
    {
        if (!component_ || !IsUObjectValid(component_)) return;

        Mod::ScopedProcessEventGuard guard;

        component_->RequestRedraw();
    }

} // namespace PortableWidget
