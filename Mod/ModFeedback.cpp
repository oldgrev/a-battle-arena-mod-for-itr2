

#include "ModFeedback.hpp"

#include "GameContext.hpp"
#include "Logging.hpp"

namespace Mod::ModFeedback
{
    void ShowMessage(const wchar_t* text, float seconds, const SDK::FLinearColor& color)
    {
        if (!text || !*text)
            return;

        SDK::UObject* ctx = Mod::GameContext::GetWorldContext();
        if (!ctx)
        {
            static int missingCtxLogs = 0;
            if (missingCtxLogs < 10)
            {
                ++missingCtxLogs;
                LOG_WARN("[ModFeedback] No WorldContext yet; message skipped");
            }
            return;
        }

        SDK::FString msg(text);
        
        // // 1. Blueprint-level PrintString (often only visible on desktop spectator window)
        // SDK::UKismetSystemLibrary::PrintString(ctx, msg, true, true, color, seconds, SDK::FName());

        // // 2. High-level General Function Library Subtitles
        SDK::FText textObj = SDK::UKismetTextLibrary::Conv_StringToText(msg);
        // SDK::UFLGeneral::ShowSubtitles(ctx, SDK::ESubtitleInstigator::Guide, textObj, seconds);

        // // 3. Game popup message (Experimental, may be visible in VR)
        // SDK::UFLGeneral::ShowMessage(msg, SDK::FString(L"Mod Notification"));

        // 4. Fallback: Try player-specific call again with Guide instigator
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
        if (player)
        {
            player->ShowSubtitles(SDK::ESubtitleInstigator::Guide, textObj, seconds);
        }
        else
        {
            LOG_WARN("[ModFeedback] Player character not found; message may not be visible in VR");
        }

        LOG_INFO("[ModFeedback] Shown message: " << msg.ToString());
    }

    void ShowWorldText(const SDK::FVector& location, const wchar_t* text, float seconds, const SDK::FLinearColor& color)
    {
        if (!text || !*text)
            return;

        SDK::UObject* ctx = Mod::GameContext::GetWorldContext();
        if (!ctx)
            return;

        SDK::FString msg(text);
        SDK::UKismetSystemLibrary::DrawDebugString(ctx, location, msg, nullptr, color, seconds);
        LOG_INFO("[ModFeedback] Shown world text: " << msg.ToString());
    }
}
