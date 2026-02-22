

#include "ModFeedback.hpp"

/*
AILEARNINGS
Symptom (subtitle_test): Mod subtitle messages were "sent" (logs) but nothing showed in the HMD.
Trace evidence: our ProcessEvent target object was the CDO: `BP_RadiusPlayerCharacter_Gameplay.Default__BP_RadiusPlayerCharacter_Gameplay_C`.
Counter-evidence: game-generated empty-gun warning called the same `ShowSubtitles` function on the live in-level actor: `L_Forest.L_Forest.PersistentLevel.BP_RadiusPlayerCharacter_Gameplay_C_...`.
Root cause: our player lookup could return the class default object (CDO) via a naive GObjects scan fallback; calling gameplay events (like `ShowSubtitles`) on the CDO is a no-op.
Fix: `GameContext::GetPlayerCharacter()` now filters out default objects (`UObject::IsDefaultObject()`), prefers `PlayerController -> Pawn` / GameplayStatics paths, and only uses a world-matching scan as a last resort.
String lifetime gotcha: Dumper-7 `SDK::FString` is a non-owning view; pointing it at temporary buffers can UAF in deferred UI paths.
Fix: build messages in `std::wstring` and use a thread-local ring of backing buffers (`MakeStableFString`).
*/

#include "GameContext.hpp"
#include "Logging.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace Mod::ModFeedback
{
    namespace
    {
        struct PendingMessage
        {
            std::wstring Text;
            float Seconds;
            SDK::FLinearColor Color;
        };

        static std::mutex gPendingMutex;
        static std::deque<PendingMessage> gPending;
        static std::atomic<bool> gDraining{false};

        // -----------------------------------------------------------------
        // Subtitle test sequence (staggered)
        // -----------------------------------------------------------------
        struct SubtitleTestItem
        {
            std::wstring Text;
            float Seconds;
        };

        static std::mutex gTestMutex;
        static bool gTestActive = false;
        static uint64_t gTestNextTickMs = 0;
        static uint32_t gTestIntervalMs = 900;
        static size_t gTestIndex = 0;
        static std::vector<SubtitleTestItem> gTestItems;

        bool IsValidObject(const SDK::UObject* obj)
        {
            return obj && SDK::UKismetSystemLibrary::IsValid(obj);
        }

        // UC::FString is a non-owning view. Never point it at temporary buffers.
        // Use a small ring of persistent buffers so pointers stay valid long enough
        // for engine code that might defer consumption.
        static SDK::FString MakeStableFString(const std::wstring& value)
        {
            static thread_local std::array<std::wstring, 32> ring;
            static thread_local uint32_t ringIndex = 0;

            std::wstring& slot = ring[ringIndex++ % ring.size()];
            slot = value;
            return SDK::FString(slot.c_str());
        }

        static bool IsSuppressedWorldNameLower(const std::string& lower)
        {
            // Hard ignore in these maps: showing subtitles during load/menu is unstable and unwanted.
            return (lower.find("l_startup") != std::string::npos) || (lower.find("l_mainmenu") != std::string::npos);
        }

        static bool ShouldSuppressSubtitlesNow(std::string* outWorldName = nullptr)
        {
            SDK::UWorld* world = Mod::GameContext::GetWorld();
            if (!world)
            {
                if (outWorldName) *outWorldName = "<null>";
                return true;
            }

            std::string name = world->GetName();
            if (outWorldName) *outWorldName = name;

            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            return IsSuppressedWorldNameLower(name);
        }

        static void EnqueuePending(const std::wstring& text, float seconds, const SDK::FLinearColor& color)
        {
            std::lock_guard<std::mutex> lock(gPendingMutex);
            if (gPending.size() >= 32)
                gPending.pop_front();
            gPending.push_back(PendingMessage{text, seconds, color});
        }

        static uint64_t NowTickMs()
        {
            return static_cast<uint64_t>(GetTickCount64());
        }

        static std::wstring RepeatChar(wchar_t ch, size_t count)
        {
            std::wstring s;
            s.assign(count, ch);
            return s;
        }

        static void UpdateSubtitleTestSequence()
        {
            std::lock_guard<std::mutex> lock(gTestMutex);
            if (!gTestActive)
                return;

            const uint64_t now = NowTickMs();
            if (gTestNextTickMs != 0 && now < gTestNextTickMs)
                return;

            if (gTestIndex >= gTestItems.size())
            {
                gTestActive = false;
                LOG_INFO("[ModFeedback] subtitle_test complete (" << gTestItems.size() << " cases)");
                return;
            }

            // Emit exactly one per interval to keep the on-HMD result readable.
            const SubtitleTestItem& item = gTestItems[gTestIndex++];
            gTestNextTickMs = now + gTestIntervalMs;

            // IMPORTANT: call the public API (so suppression/queueing still applies).
            // We purposely keep the color parameter ignored for now.
            // (This is testing the game's ShowSubtitles behavior, not our alternate channels.)
            // NOLINTNEXTLINE(readability-suspicious-call-argument)
            ShowMessage(item.Text.c_str(), item.Seconds, SDK::FLinearColor{0.8f, 0.9f, 1.0f, 1.0f});
        }
    }

    void ShowMessage(const wchar_t* text, float seconds, const SDK::FLinearColor& color)
    {
        (void)color; // CRUFT NOTE: legacy multi-channel paths used color; ShowSubtitles doesn't.

        if (!text || !*text)
            return;

        // During Startup/MainMenu, we avoid spamming subtitles; queue for later instead.
        std::string worldName;
        if (ShouldSuppressSubtitlesNow(&worldName))
        {
            EnqueuePending(std::wstring(text), seconds, SDK::FLinearColor{0.0f, 1.0f, 1.0f, 1.0f});
            static int suppressedLogs = 0;
            if (suppressedLogs < 25)
            {
                ++suppressedLogs;
                LOG_INFO("[ModFeedback] Suppressed ShowSubtitles (world='" << worldName << "'); queued: " << SDK::FString(text).ToString());
            }
            return;
        }

        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
        if (!player)
        {
            static int missingPlayerLogs = 0;
            if (missingPlayerLogs < 25)
            {
                ++missingPlayerLogs;
                LOG_WARN("[ModFeedback] No player yet; ShowMessage skipped: " << SDK::FString(text).ToString());
            }
            return;
        }

        // Defensive: never attempt to send gameplay/UI events to the CDO.
        if (player->IsDefaultObject())
        {
            static int cdoLogs = 0;
            if (cdoLogs < 25)
            {
                ++cdoLogs;
                LOG_ERROR("[ModFeedback] BUG: GetPlayerCharacter returned Default__ (CDO); refusing ShowSubtitles. player='" << player->GetFullName() << "'");
            }
            Mod::GameContext::ClearCache();
            return;
        }

        // Force-enable subtitles so the mod can always communicate.
        if (!SDK::UGameplayStatics::AreSubtitlesEnabled())
        {
            SDK::UGameplayStatics::SetSubtitlesEnabled(true);
            LOG_INFO("[ModFeedback] Subtitles were disabled; forced enabled for mod feedback");
        }

        float duration = seconds;
        if (duration <= 0.05f)
            duration = 2.0f;
        if (duration > 20.0f)
            duration = 20.0f;

        // Avoid passing temporary/unstable buffers into the SDK (UC::FString is non-owning).
        std::wstring msgW(text);

        // Defensive cap: we don't know the engine's max length here; keep it bounded
        // so telnet commands can't accidentally allocate/format multi-kilobyte UI strings.
        constexpr size_t kMaxChars = 1024;
        if (msgW.size() > kMaxChars)
        {
            msgW.resize(kMaxChars);
            msgW.append(L"…");
        }

        SDK::FText msgText;
        {
            SDK::FString stable = MakeStableFString(msgW);
            msgText = SDK::UKismetTextLibrary::Conv_StringToText(stable);
        }

        // This is the call path seen in trace logs right before the headset-visible notification:
        // Function BP_RadiusPlayerCharacter_Gameplay_C.ShowSubtitles
        static int showLogs = 0;
        if (showLogs < 200)
        {
            ++showLogs;
            LOG_INFO("[ModFeedback] ShowSubtitles(System) player='" << player->GetFullName() << "' dur=" << duration << " chars=" << (int)msgW.size() << " text='" << SDK::FString(msgW.c_str()).ToString() << "'");
        }

        player->ShowSubtitles(SDK::ESubtitleInstigator::System, msgText, duration);
    }

    void StartSubtitleTestSequence(uint32_t intervalMs)
    {
        std::lock_guard<std::mutex> lock(gTestMutex);

        gTestItems.clear();
        gTestItems.reserve(24);

        // Purpose: explore what breaks (length, newlines, markup tokens) using the same
        // ShowSubtitles path the game uses for safety/no-mag feedback.
        gTestItems.push_back({L"[subtitle_test] baseline", 2.5f});
        gTestItems.push_back({L"[subtitle_test] newline\nsecond line", 3.0f});
        gTestItems.push_back({L"[subtitle_test] two newlines\nline2\nline3", 3.5f});

        // Markup-like tokens (the game seems to use <b> and {BTN_*} style placeholders).
        gTestItems.push_back({L"[subtitle_test] markup: <b>bold?</b>", 3.0f});
        gTestItems.push_back({L"[subtitle_test] token: {BTN_Top} {BTN_Bottom}", 3.0f});
        gTestItems.push_back({L"[subtitle_test] combined: <b>{BTN_Top}</b> to toggle", 3.5f});

        // Length ramps (single-line)
        gTestItems.push_back({L"[subtitle_test] len=32 " + RepeatChar(L'A', 32), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=64 " + RepeatChar(L'B', 64), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=128 " + RepeatChar(L'C', 128), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=256 " + RepeatChar(L'D', 256), 3.5f});
        gTestItems.push_back({L"[subtitle_test] len=512 " + RepeatChar(L'E', 512), 4.0f});

        // Unicode probes
        gTestItems.push_back({L"[subtitle_test] unicode: ✓ Ω Ж \u4E2D", 3.5f});
        gTestItems.push_back({L"[subtitle_test] unicode long: " + RepeatChar(L'\u4E2D', 120), 3.5f});

        // Very long (will be capped in ShowMessage)
        gTestItems.push_back({L"[subtitle_test] very long (cap expected): " + RepeatChar(L'X', 2000), 4.0f});

        gTestIntervalMs = (intervalMs < 200 ? 200 : intervalMs);
        gTestActive = true;
        gTestIndex = 0;
        gTestNextTickMs = 0;

        LOG_INFO("[ModFeedback] subtitle_test started (cases=" << gTestItems.size() << ", intervalMs=" << gTestIntervalMs << ")");
    }

    void StopSubtitleTestSequence()
    {
        std::lock_guard<std::mutex> lock(gTestMutex);
        gTestActive = false;
        gTestItems.clear();
        gTestIndex = 0;
        gTestNextTickMs = 0;
        LOG_INFO("[ModFeedback] subtitle_test stopped");
    }

    void DrainPending()
    {
        // Avoid recursion if draining triggers more feedback.
        bool expected = false;
        if (!gDraining.compare_exchange_strong(expected, true))
            return;

        // Only drain once we're out of suppressed maps.
        if (ShouldSuppressSubtitlesNow())
        {
            gDraining.store(false);
            return;
        }

        std::deque<PendingMessage> local;
        {
            std::lock_guard<std::mutex> lock(gPendingMutex);
            if (gPending.empty())
            {
                gDraining.store(false);
                // Still advance the staggered subtitle test sequence even when there are
                // no suppressed pending messages to drain.
                UpdateSubtitleTestSequence();
                return;
            }
            local.swap(gPending);
        }

        int drained = 0;
        for (const auto& m : local)
        {
            // Emit using the normal path (now that we're in a safe world).
            ShowMessage(m.Text.c_str(), m.Seconds, m.Color);
            if (++drained >= 8)
                break; // don't spam on the first tick after load
        }

        // If we didn't drain all, put the rest back.
        if ((int)local.size() > drained)
        {
            std::lock_guard<std::mutex> lock(gPendingMutex);
            for (size_t i = drained; i < local.size(); ++i)
            {
                if (gPending.size() >= 32)
                    break;
                gPending.push_back(local[i]);
            }
        }

        gDraining.store(false);

        // Also drive any staggered subtitle test sequence.
        UpdateSubtitleTestSequence();
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

    void PlaySound2D(SDK::USoundBase* sound, float volume, float pitch, bool isUiSound)
    {
        if (!sound)
            return;

        if (SDK::APlayerController* controller = Mod::GameContext::GetPlayerController())
        {
            controller->ClientPlaySound(sound, volume, pitch);
            return;
        }

        SDK::UObject* ctx = Mod::GameContext::GetWorldContext();
        if (!ctx)
            return;

        SDK::UGameplayStatics::PlaySound2D(ctx, sound, volume, pitch, 0.0f, nullptr, nullptr, isUiSound);
    }

    void PlaySoundAtLocation(SDK::USoundBase* sound, const SDK::FVector& location, float volume, float pitch)
    {
        if (!sound)
            return;

        if (SDK::APlayerController* controller = Mod::GameContext::GetPlayerController())
        {
            controller->ClientPlaySoundAtLocation(sound, location, volume, pitch);
            return;
        }

        SDK::UObject* ctx = Mod::GameContext::GetWorldContext();
        if (!ctx)
            return;

        SDK::UGameplayStatics::PlaySoundAtLocation(ctx, sound, location, SDK::FRotator{0.0, 0.0, 0.0}, volume, pitch, 0.0f, nullptr, nullptr, nullptr, nullptr);
    }
}
