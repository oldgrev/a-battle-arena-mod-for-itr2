
#include "ModFeedback.hpp"

/*
AILEARNINGS
Audio playback history:
  - UMediaPlayer + UMediaSoundComponent: worked for 2D but 3D attachment always produced
    centered/non-spatialized output regardless of bAllowSpatialization settings.
  - USoundWave raw-PCM direct injection (offset writes): engine audio mixdown never
    recognized the data; GetPlayState() showed Stopped immediately after Play().
  - USynthSamplePlayer + USoundWave: IsPlaying() returned true but no audible output;
    engine expects USoundWave to come from proper asset import pipeline.
  - Final working solution: bypass UE audio entirely. Parse .wav files ourselves, apply
    3D spatial processing (stereo pan/ILD, distance attenuation, LPF), and output through
    the Windows waveOut API. This is implemented in SpatialAudio.hpp/cpp.
Subtitle system (ShowSubtitles): kept as-is, routes through player character's HMD UI.
  - CDO trap: GetPlayerCharacter() can return Default__ (CDO); ShowSubtitles on CDO is a no-op.
  - FString lifetime: SDK::FString is non-owning; use MakeStableFString ring buffer.
*/

#include "GameContext.hpp"
#include "Logging.hpp"
#include "ModTuning.hpp"
#include "SpatialAudio.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Mod::ModFeedback
{
    namespace
    {
        // -----------------------------------------------------------------
        // Subtitle/message infrastructure (kept from original)
        // -----------------------------------------------------------------

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
        // Sound group system (kept from original)
        // -----------------------------------------------------------------

        static std::mutex gSoundGroupMutex;
        static std::unordered_map<std::string, std::vector<std::string>> gSoundGroups;
        static std::string gSoundGroupsSourceFolder;
        static std::mt19937 gSoundGroupRng{std::random_device{}()};

        // -----------------------------------------------------------------
        // Subtitle test sequence (kept from original)
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

        // -----------------------------------------------------------------
        // Actor -> SpatialAudio handle tracking for 3D position updates
        // -----------------------------------------------------------------

        struct TrackedActorSound
        {
            SDK::AActor*               actor  = nullptr;
            SpatialAudio::SoundHandle  handle = SpatialAudio::kInvalidHandle;
            std::string                descriptor;
        };

        static std::mutex gTrackedMutex;
        static std::vector<TrackedActorSound> gTrackedSounds;

        // -----------------------------------------------------------------
        // Helpers
        // -----------------------------------------------------------------

        static uint64_t NowTickMs()
        {
            return static_cast<uint64_t>(GetTickCount64());
        }

        static SDK::FString MakeStableFString(const std::wstring& value)
        {
            static thread_local std::array<std::wstring, 32> ring;
            static thread_local uint32_t ringIndex = 0;

            std::wstring& slot = ring[ringIndex++ % ring.size()];
            slot = value;
            return SDK::FString(slot.c_str());
        }

        static std::wstring ToWide(const std::string& value)
        {
            return std::wstring(value.begin(), value.end());
        }

        static std::string ToLowerCopy(const std::string& value)
        {
            std::string out = value;
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        static bool IsUrl(const std::string& entry)
        {
            if (entry.size() >= 7 && entry.compare(0, 7, "http://") == 0)  return true;
            if (entry.size() >= 8 && entry.compare(0, 8, "https://") == 0) return true;
            return false;
        }

        static std::string TrimWhitespace(const std::string& s)
        {
            const auto start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(start, end - start + 1);
        }

        static bool IsSupportedAudioExtension(const std::filesystem::path& path)
        {
            static const std::unordered_set<std::string> exts = {
                ".mp3", ".wav", ".ogg", ".flac", ".aac", ".m4a", ".wma"
            };
            const std::string ext = ToLowerCopy(path.extension().string());
            return exts.find(ext) != exts.end();
        }

        static std::string DeriveGroupName(const std::filesystem::path& path)
        {
            std::string stem = ToLowerCopy(path.stem().string());
            if (stem.empty())
                return "default";

            const std::size_t dash = stem.find('-');
            if (dash != std::string::npos && dash > 0)
                return stem.substr(0, dash);

            while (!stem.empty())
            {
                const unsigned char ch = static_cast<unsigned char>(stem.back());
                if (std::isdigit(ch) || stem.back() == '_' || stem.back() == '-')
                {
                    stem.pop_back();
                    continue;
                }
                break;
            }

            return stem.empty() ? "default" : stem;
        }

        static bool IsSuppressedWorldNameLower(const std::string& lower)
        {
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

            const SubtitleTestItem& item = gTestItems[gTestIndex++];
            gTestNextTickMs = now + gTestIntervalMs;

            ShowMessage(item.Text.c_str(), item.Seconds, SDK::FLinearColor{0.8f, 0.9f, 1.0f, 1.0f});
        }

        // -----------------------------------------------------------------
        // Actor sound tracking: get player view for listener state
        // -----------------------------------------------------------------

        static bool GetListenerState(
            SpatialAudio::Vec3& outPos,
            SpatialAudio::Vec3& outFwd,
            SpatialAudio::Vec3& outRight)
        {
            SDK::UWorld* world = Mod::GameContext::GetWorld();
            if (!world) return false;

            SDK::FVector viewLoc{};
            SDK::FRotator viewRot{};
            if (!Mod::GameContext::GetPlayerView(world, viewLoc, viewRot))
                return false;

            outPos = { static_cast<float>(viewLoc.X), static_cast<float>(viewLoc.Y), static_cast<float>(viewLoc.Z) };

            // Get forward and right vectors from the player rotation
            const SDK::FVector fwd = SDK::UKismetMathLibrary::GetForwardVector(viewRot);
            const SDK::FVector right = SDK::UKismetMathLibrary::GetRightVector(viewRot);

            outFwd   = { static_cast<float>(fwd.X), static_cast<float>(fwd.Y), static_cast<float>(fwd.Z) };
            outRight = { static_cast<float>(right.X), static_cast<float>(right.Y), static_cast<float>(right.Z) };
            return true;
        }

        // Update all tracked actor-sound positions and clean up stale entries.
        // Called from DrainPending (every tick). SpatialAudio::Tick internally
        // throttles position recomputation to ~10fps.
        static void UpdateTrackedActorPositions()
        {
            SpatialAudio::Vec3 listenerPos, listenerFwd, listenerRight;
            bool hasListener = GetListenerState(listenerPos, listenerFwd, listenerRight);

            if (hasListener)
                SpatialAudio::SetListenerState(listenerPos, listenerFwd, listenerRight);

            std::lock_guard<std::mutex> lock(gTrackedMutex);

            // Clean up entries where actor is invalid or sound has finished
            gTrackedSounds.erase(
                std::remove_if(gTrackedSounds.begin(), gTrackedSounds.end(),
                    [](const TrackedActorSound& t)
                    {
                        if (!SpatialAudio::IsPlaying(t.handle))
                        {
                            LOG_INFO("[ModFeedback] Tracked sound finished: " << t.descriptor);
                            return true;
                        }
                        if (t.actor && !SDK::UKismetSystemLibrary::IsValid(t.actor))
                        {
                            LOG_INFO("[ModFeedback] Tracked actor invalid, stopping sound: " << t.descriptor);
                            SpatialAudio::Stop(t.handle);
                            return true;
                        }
                        return false;
                    }),
                gTrackedSounds.end());

            // Update source positions for surviving entries
            for (auto& t : gTrackedSounds)
            {
                if (!t.actor || !SDK::UKismetSystemLibrary::IsValid(t.actor))
                    continue;
                SDK::FVector loc = t.actor->K2_GetActorLocation();
                SpatialAudio::SetSourcePosition(t.handle, { static_cast<float>(loc.X), static_cast<float>(loc.Y), static_cast<float>(loc.Z) });
            }

            // Drive the SpatialAudio tick (throttled internally to ~10fps)
            SpatialAudio::Tick();
        }

        // -----------------------------------------------------------------
        // Right-hand widget display — replaces ShowSubtitles as primary output
        // Uses W_GripDebug_R with a WBP_Confirmation widget, exactly like VRMenuSubsystem
        // uses W_GripDebug_L for the mod menu.
        // -----------------------------------------------------------------

        static SDK::UWBP_Confirmation_C* gRightWidget = nullptr;  // owned by UE GC
        static uint64_t gRightWidgetExpiryMs = 0;                 // 0 = no active message
        static SDK::ABP_RadiusPlayerCharacter_Gameplay_C* gRightWidgetOwnerPlayer = nullptr; // track which player owns the widget
        static int gRHLogCount = 0;
        constexpr int kRHLogLimit = 500;  // extensive logging for debugging

        // Helper: check if a UObject is still valid (not GC'd/pending kill)
        static bool IsWidgetValid(const SDK::UObject* obj)
        {
            if (!obj) return false;
            return SDK::UKismetSystemLibrary::IsValid(obj);
        }

        // Lazily create and attach the WBP_Confirmation widget to W_GripDebug_R.
        // Returns true if gRightWidget is ready to use.
        static bool EnsureRightHandWidget()
        {
            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
            if (!player || player->IsDefaultObject())
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] EnsureRightHandWidget: no valid player (player=" << (player ? "CDO" : "null") << ")");
                gRightWidget = nullptr;
                gRightWidgetOwnerPlayer = nullptr;
                return false;
            }

            SDK::UWidgetComponent* comp = player->W_GripDebug_R;
            if (!comp)
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] EnsureRightHandWidget: W_GripDebug_R is null on player");
                gRightWidget = nullptr;
                gRightWidgetOwnerPlayer = nullptr;
                return false;
            }

            // Check if cached widget is still valid AND belongs to current player
            if (gRightWidget && gRightWidgetOwnerPlayer == player && IsWidgetValid(gRightWidget))
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_INFO("[ModFeedback][RH] EnsureRightHandWidget: cached widget still valid");
                return true;
            }

            // Widget invalid, stale, or belongs to different player - need to recreate
            if (gRightWidget)
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_INFO("[ModFeedback][RH] EnsureRightHandWidget: cached widget invalid/stale, recreating (owner=" 
                        << (gRightWidgetOwnerPlayer == player ? "same" : "different") 
                        << " valid=" << (IsWidgetValid(gRightWidget) ? "yes" : "no") << ")");
                gRightWidget = nullptr;
                gRightWidgetOwnerPlayer = nullptr;
            }

            SDK::UWorld* world = Mod::GameContext::GetWorld();
            if (!world) return false;

            SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
            if (!pc) return false;

            SDK::UClass* widgetClass = SDK::UWBP_Confirmation_C::StaticClass();
            if (!widgetClass)
            {
                LOG_WARN("[ModFeedback][RH] WBP_Confirmation_C class not found");
                return false;
            }

            SDK::UUserWidget* widget = SDK::UWidgetBlueprintLibrary::Create(world, widgetClass, pc);
            if (!widget || !widget->IsA(widgetClass))
            {
                LOG_WARN("[ModFeedback][RH] Failed to create WBP_Confirmation_C widget");
                return false;
            }

            gRightWidget = static_cast<SDK::UWBP_Confirmation_C*>(widget);
            comp->SetWidget(gRightWidget);
            comp->SetVisibility(true, true);
            comp->SetHiddenInGame(false, true);
            comp->SetTwoSided(true);

            // Fixed draw size suitable for short messages
            SDK::FVector2D drawSize{ 500.0, 500.0 };
            comp->SetDrawSize(drawSize);

            // Set static header and hide Yes/No buttons
            {
                SDK::FString hdrStr(L"MOD");
                SDK::FText hdrText = SDK::UKismetTextLibrary::Conv_StringToText(hdrStr);
                gRightWidget->Title = hdrText;
                if (gRightWidget->Txt_Confirmation_Title)
                {
                    gRightWidget->Txt_Confirmation_Title->SetText(hdrText);
                    SDK::FSlateColor col;
                    col.SpecifiedColor = SDK::FLinearColor{0.5f, 0.85f, 1.0f, 1.0f};
                    gRightWidget->Txt_Confirmation_Title->SetColorAndOpacity(col);
                }
                SDK::FString emptyStr(L"");
                SDK::FText emptyText = SDK::UKismetTextLibrary::Conv_StringToText(emptyStr);
                gRightWidget->Yes_Text = emptyText;
                gRightWidget->No_Text = emptyText;
            }

            // Track ownership so we can detect player changes
            gRightWidgetOwnerPlayer = player;

            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] Right-hand message widget created and attached to W_GripDebug_R");
            return true;
        }

        // Display text on W_GripDebug_R for the given duration.
        // Returns true if the widget rendered the message (false = widget not available).
        static bool ShowOnRightWidget(const wchar_t* text, float seconds)
        {
            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] ShowOnRightWidget called: text_len=" << (text ? wcslen(text) : 0) << " seconds=" << seconds);

            if (!EnsureRightHandWidget())
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: EnsureRightHandWidget failed");
                return false;
            }
            
            if (!gRightWidget)
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: gRightWidget is null after EnsureRightHandWidget succeeded?!");
                return false;
            }

            // Re-validate widget hasn't been GC'd between EnsureRightHandWidget and now
            if (!IsWidgetValid(gRightWidget))
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: widget became invalid (GC'd?)");
                gRightWidget = nullptr;
                gRightWidgetOwnerPlayer = nullptr;
                return false;
            }

            std::wstring msg(text);
            SDK::FString msgStr = MakeStableFString(msg);
            SDK::FText msgText = SDK::UKismetTextLibrary::Conv_StringToText(msgStr);

            // Log what text we're trying to set (truncate for logging)
            std::wstring logMsg = msg.size() > 50 ? msg.substr(0, 47) + L"..." : msg;
            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] ShowOnRightWidget: setting text: \"" << SDK::FString(logMsg.c_str()).ToString() << "\"");

            gRightWidget->Description = msgText;
            if (gRightWidget->Txt_TextConfirm)
            {
                gRightWidget->Txt_TextConfirm->SetText(msgText);
                
                // Force text block to recognize the update
                // Try multiple approaches since UE widget updates can be finicky
                // Approach 1: Modify style to trigger a visual update
                SDK::FTextBlockStyle style = gRightWidget->Txt_TextConfirm->WidgetStyle;
                style.ColorAndOpacity.SpecifiedColor = SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f};
                gRightWidget->Txt_TextConfirm->SetWidgetStyle(style);
                
                // Approach 2: Force invalidation via render opacity (always valid)
                gRightWidget->Txt_TextConfirm->SetRenderOpacity(1.0f);
                
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_INFO("[ModFeedback][RH] ShowOnRightWidget: text set on Txt_TextConfirm");
            }
            else
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: Txt_TextConfirm is null, text set only on Description");
            }

            gRightWidgetExpiryMs = NowTickMs() + static_cast<uint64_t>(seconds * 1000.0f);

            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
            if (!player)
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: player became null after text set");
                return false;
            }

            SDK::UWidgetComponent* comp = player->W_GripDebug_R;
            if (!comp)
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] ShowOnRightWidget: W_GripDebug_R is null on player");
                return false;
            }

            // CRITICAL FIX: Always ensure visibility is ON before showing a message
            // TickRightWidget() hides the component when message expires, so we must re-enable it here
            bool wasHidden = !comp->IsVisible();
            comp->SetVisibility(true, true);
            comp->SetHiddenInGame(false, true);
            
            // FIX FOR STUCK TEXT: Re-set the widget on the component to force a full refresh
            // This ensures the widget component picks up the new text content
            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] ShowOnRightWidget: re-setting widget on component to force update");
            comp->SetWidget(nullptr);
            comp->SetWidget(gRightWidget);
            
            comp->RequestRedraw();

            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] ShowOnRightWidget: success (wasHidden=" << (wasHidden ? "yes" : "no") 
                    << " expiryMs=" << gRightWidgetExpiryMs << ")");

            return true;
        }

        // Called every tick. Clears the right-hand widget text once the message duration expires.
        static void TickRightWidget()
        {
            // First, check if widget is still valid
            if (gRightWidget && !IsWidgetValid(gRightWidget))
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_INFO("[ModFeedback][RH] TickRightWidget: cached widget became invalid (GC'd), clearing");
                gRightWidget = nullptr;
                gRightWidgetOwnerPlayer = nullptr;
                gRightWidgetExpiryMs = 0;
                return;
            }

            if (!gRightWidget || gRightWidgetExpiryMs == 0)
                return;
            if (NowTickMs() < gRightWidgetExpiryMs)
                return;

            if (gRHLogCount++ < kRHLogLimit)
                LOG_INFO("[ModFeedback][RH] TickRightWidget: message expired, clearing text and hiding widget");

            gRightWidgetExpiryMs = 0;

            SDK::FString emptyStr(L"");
            SDK::FText emptyText = SDK::UKismetTextLibrary::Conv_StringToText(emptyStr);
            gRightWidget->Description = emptyText;
            if (gRightWidget->Txt_TextConfirm)
                gRightWidget->Txt_TextConfirm->SetText(emptyText);

            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
            if (player && player->W_GripDebug_R)
            {
                player->W_GripDebug_R->SetVisibility(false, true);
                player->W_GripDebug_R->RequestRedraw();
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_INFO("[ModFeedback][RH] TickRightWidget: component hidden");
            }
            else
            {
                if (gRHLogCount++ < kRHLogLimit)
                    LOG_WARN("[ModFeedback][RH] TickRightWidget: could not hide component (player=" 
                        << (player ? "valid" : "null") << " comp=" 
                        << (player && player->W_GripDebug_R ? "valid" : "null") << ")");
            }
        }

        // Register a 3D sound handle as tracked against an actor for position updates.
        static void TrackActorSound(SDK::AActor* actor, SpatialAudio::SoundHandle handle, const std::string& descriptor)
        {
            std::lock_guard<std::mutex> lock(gTrackedMutex);
            gTrackedSounds.push_back({ actor, handle, descriptor });
        }

        // Play a wav file with 3D spatial audio attached to an actor.
        // Reads player view for initial listener state and registers for position tracking.
        static bool PlayWavSpatial3D(
            SDK::AActor* actor,
            const std::string& filePath,
            bool loop,
            float volume,
            std::string* outError)
        {
            if (!actor)
            {
                if (outError) *outError = "actor is null";
                return false;
            }

            // Get current listener state
            SpatialAudio::Vec3 listenerPos, listenerFwd, listenerRight;
            if (!GetListenerState(listenerPos, listenerFwd, listenerRight))
            {
                if (outError) *outError = "cannot get player view for listener state";
                return false;
            }

            // Get actor position
            SDK::FVector actorLoc = actor->K2_GetActorLocation();
            SpatialAudio::Vec3 sourcePos = { static_cast<float>(actorLoc.X), static_cast<float>(actorLoc.Y), static_cast<float>(actorLoc.Z) };

            std::string err;
            SpatialAudio::SoundHandle handle = SpatialAudio::Play3D(
                filePath, sourcePos, listenerPos, listenerFwd, listenerRight,
                volume, loop, &err);

            if (handle == SpatialAudio::kInvalidHandle)
            {
                if (outError) *outError = err;
                LOG_ERROR("[ModFeedback] PlayWavSpatial3D failed: " << err << " file='" << filePath << "'");
                return false;
            }

            TrackActorSound(actor, handle, filePath);
            LOG_INFO("[ModFeedback] PlayWavSpatial3D ok (handle=" << handle
                << ", file='" << filePath << "'"
                << ", actor='" << actor->GetName() << "'"
                << ", loop=" << (loop ? 1 : 0)
                << ", vol=" << volume << ")");
            return true;
        }

        // Resolve an actor selector string (player, npc_nearest, substring search).
        static SDK::AActor* ResolveActorSelector(const std::string& selectorRaw, std::string* outResolvedName, std::string* outError)
        {
            const std::string selector = ToLowerCopy(selectorRaw);
            if (selector.empty())
            {
                if (outError) *outError = "actor selector is empty";
                return nullptr;
            }

            SDK::UWorld* world = Mod::GameContext::GetWorld();
            if (!world)
            {
                if (outError) *outError = "world is null";
                return nullptr;
            }

            if (selector == "player" || selector == "self")
            {
                SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
                if (!player)
                {
                    if (outError) *outError = "player not found";
                    return nullptr;
                }
                if (outResolvedName) *outResolvedName = player->GetName();
                return player;
            }

            if (selector == "npc_nearest" || selector == "nearest_npc")
            {
                SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
                if (!player)
                {
                    if (outError) *outError = "player not found for nearest NPC search";
                    return nullptr;
                }

                SDK::TArray<SDK::AActor*> actors;
                SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::ARadiusAICharacterBase::StaticClass(), &actors);

                // TArray access
                struct TRaw { SDK::AActor** Data; int32_t Num; int32_t Max; };
                auto* raw = reinterpret_cast<const TRaw*>(&actors);
                SDK::AActor** data = raw->Data;
                const int32_t count = raw->Num;

                if (!data || count <= 0)
                {
                    if (outError) *outError = "no NPC actors found";
                    return nullptr;
                }

                const SDK::FVector playerLoc = player->K2_GetActorLocation();
                SDK::AActor* best = nullptr;
                double bestDistSq = (std::numeric_limits<double>::max)();
                for (int i = 0; i < count; ++i)
                {
                    SDK::AActor* a = data[i];
                    if (!a || !SDK::UKismetSystemLibrary::IsValid(a))
                        continue;
                    const SDK::FVector loc = a->K2_GetActorLocation();
                    const double dx = (double)loc.X - (double)playerLoc.X;
                    const double dy = (double)loc.Y - (double)playerLoc.Y;
                    const double dz = (double)loc.Z - (double)playerLoc.Z;
                    const double d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bestDistSq)
                    {
                        bestDistSq = d2;
                        best = a;
                    }
                }

                if (!best)
                {
                    if (outError) *outError = "nearest NPC could not be resolved";
                    return nullptr;
                }

                if (outResolvedName) *outResolvedName = best->GetName();
                return best;
            }

            // Substring match against all actors
            SDK::TArray<SDK::AActor*> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AActor::StaticClass(), &actors);
            struct TRaw { SDK::AActor** Data; int32_t Num; int32_t Max; };
            auto* raw = reinterpret_cast<const TRaw*>(&actors);
            SDK::AActor** data = raw->Data;
            const int32_t count = raw->Num;

            if (!data || count <= 0)
            {
                if (outError) *outError = "no actors found";
                return nullptr;
            }

            for (int i = 0; i < count; ++i)
            {
                SDK::AActor* a = data[i];
                if (!a || !SDK::UKismetSystemLibrary::IsValid(a))
                    continue;

                const std::string name = ToLowerCopy(a->GetName());
                const std::string full = ToLowerCopy(a->GetFullName());
                if (name.find(selector) != std::string::npos || full.find(selector) != std::string::npos)
                {
                    if (outResolvedName) *outResolvedName = a->GetName();
                    return a;
                }
            }

            if (outError) *outError = "actor selector did not match any actor";
            return nullptr;
        }

        // Helper: load a UE sound asset by soft path (for PlaySound2D / PlaySoundAtLocation)
        static SDK::USoundBase* LoadSoundAssetBySoftPath(const std::string& softObjectPath, std::string* outError)
        {
            if (softObjectPath.empty())
            {
                if (outError) *outError = "empty soft object path";
                return nullptr;
            }

            const SDK::FString pathF(ToWide(softObjectPath).c_str());
            const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
            const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
            SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);

            if (!loaded)
            {
                if (outError) *outError = "asset not found/load failed";
                return nullptr;
            }

            if (!loaded->IsA(SDK::USoundBase::StaticClass()))
            {
                if (outError) *outError = std::string("asset is not USoundBase: ") + loaded->GetFullName();
                return nullptr;
            }

            return static_cast<SDK::USoundBase*>(loaded);
        }

    } // anonymous namespace

    // =====================================================================
    // Subtitle / message display (unchanged)
    // =====================================================================

    void ShowMessage(const wchar_t* text, float seconds, const SDK::FLinearColor& color)
    {
        (void)color;

        if (!text || !*text)
            return;

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


        float duration = seconds;
        if (duration <= 3.0f)
            duration = 3.0f;
        if (duration > 20.0f)
            duration = 20.0f;

        std::wstring msgW(text);
        constexpr size_t kMaxChars = 1024;
        if (msgW.size() > kMaxChars)
        {
            msgW.resize(kMaxChars);
            msgW.append(L"\u2026");
        }

        SDK::FText msgText;
        {
            SDK::FString stable = MakeStableFString(msgW);
            msgText = SDK::UKismetTextLibrary::Conv_StringToText(stable);
        }

        // --- Primary: right-hand widget --- //
        bool wroteToWidget = false;
        try
        {
            wroteToWidget = ShowOnRightWidget(msgW.c_str(), duration);
        }
        catch (...)
        {
            LOG_WARN("[ModFeedback] Exception in ShowOnRightWidget; falling back to ShowSubtitles");
        }
        

        static int showLogs = 0;
        if (showLogs < 200)
        {
            ++showLogs;
            if (wroteToWidget)
                LOG_INFO("[ModFeedback] ShowOnRightWidget dur=" << duration << " chars=" << (int)msgW.size());
            else
                LOG_INFO("[ModFeedback] ShowSubtitles(fallback) player='" << player->GetFullName() << "' dur=" << duration << " chars=" << (int)msgW.size());
        }

        // --- Fallback: ShowSubtitles if widget not yet available --- //
        if (!wroteToWidget)
        {
            if (!SDK::UGameplayStatics::AreSubtitlesEnabled())
            {
                SDK::UGameplayStatics::SetSubtitlesEnabled(true);
                LOG_INFO("[ModFeedback] Subtitles were disabled; forced enabled for mod feedback");
            }
            player->ShowSubtitles(SDK::ESubtitleInstigator::System, msgText, duration);
        }
    }

    void StartSubtitleTestSequence(uint32_t intervalMs)
    {
        std::lock_guard<std::mutex> lock(gTestMutex);

        gTestItems.clear();
        gTestItems.reserve(24);

        gTestItems.push_back({L"[subtitle_test] baseline", 2.5f});
        gTestItems.push_back({L"[subtitle_test] newline\nsecond line", 3.0f});
        gTestItems.push_back({L"[subtitle_test] two newlines\nline2\nline3", 3.5f});
        gTestItems.push_back({L"[subtitle_test] markup: <b>bold?</b>", 3.0f});
        gTestItems.push_back({L"[subtitle_test] token: {BTN_Top} {BTN_Bottom}", 3.0f});
        gTestItems.push_back({L"[subtitle_test] combined: <b>{BTN_Top}</b> to toggle", 3.5f});
        gTestItems.push_back({L"[subtitle_test] len=32 " + RepeatChar(L'A', 32), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=64 " + RepeatChar(L'B', 64), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=128 " + RepeatChar(L'C', 128), 3.0f});
        gTestItems.push_back({L"[subtitle_test] len=256 " + RepeatChar(L'D', 256), 3.5f});
        gTestItems.push_back({L"[subtitle_test] len=512 " + RepeatChar(L'E', 512), 4.0f});
        gTestItems.push_back({L"[subtitle_test] unicode: \u2713 \u03A9 \u0416 \u4E2D", 3.5f});
        gTestItems.push_back({L"[subtitle_test] unicode long: " + RepeatChar(L'\u4E2D', 120), 3.5f});
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

    // =====================================================================
    // DrainPending – called every tick from ModMain
    // =====================================================================

    void DrainPending()
    {
        bool expected = false;
        if (!gDraining.compare_exchange_strong(expected, true))
            return;

        // Update 3D sound positions and clean up stale entries
        UpdateTrackedActorPositions();

        // Expire right-hand widget message if its duration has elapsed
        TickRightWidget();

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
                UpdateSubtitleTestSequence();
                return;
            }
            local.swap(gPending);
        }

        int drained = 0;
        for (const auto& m : local)
        {
            ShowMessage(m.Text.c_str(), m.Seconds, m.Color);
            if (++drained >= 8)
                break;
        }

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
        UpdateSubtitleTestSequence();
    }

    // =====================================================================
    // World text (debug)
    // =====================================================================

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

    // =====================================================================
    // UE-native sound asset playback (game built-in sounds, unchanged)
    // =====================================================================

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

        SDK::UGameplayStatics::PlaySoundAtLocation(ctx, sound, location, SDK::FRotator{0.0f, 0.0f, 0.0f}, volume, pitch, 0.0f, nullptr, nullptr, nullptr, nullptr);
    }

    bool PlaySoundAsset2D(const std::string& softObjectPath, float volume, float pitch, bool isUiSound, std::string* outError)
    {
        std::string err;
        SDK::USoundBase* sound = LoadSoundAssetBySoftPath(softObjectPath, &err);
        if (!sound)
        {
            if (outError) *outError = err;
            LOG_WARN("[ModFeedback] PlaySoundAsset2D failed for path '" << softObjectPath << "': " << err);
            return false;
        }
        PlaySound2D(sound, volume, pitch, isUiSound);
        return true;
    }

    bool PlaySoundAsset3DAtPlayer(const std::string& softObjectPath, float volume, float pitch, std::string* outError)
    {
        std::string err;
        SDK::USoundBase* sound = LoadSoundAssetBySoftPath(softObjectPath, &err);
        if (!sound)
        {
            if (outError) *outError = err;
            LOG_WARN("[ModFeedback] PlaySoundAsset3DAtPlayer failed for path '" << softObjectPath << "': " << err);
            return false;
        }

        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
        if (!player)
        {
            if (outError) *outError = "no local player character";
            return false;
        }

        const SDK::FVector location = player->K2_GetActorLocation();
        PlaySoundAtLocation(sound, location, volume, pitch);
        return true;
    }

    // =====================================================================
    // Media/WAV playback – now routed through SpatialAudio
    // =====================================================================

    bool PlayMediaFile2D(const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        // Only .wav files are supported by SpatialAudio. Non-wav files won't work.
        std::string ext = ToLowerCopy(std::filesystem::path(filePath).extension().string());
        if (ext != ".wav")
        {
            if (outError) *outError = "only .wav files supported (got " + ext + "); UE audio has been removed";
            LOG_WARN("[ModFeedback] PlayMediaFile2D: non-wav file unsupported: " << filePath);
            return false;
        }

        std::string err;
        SpatialAudio::SoundHandle handle = SpatialAudio::Play2D(filePath, volume, loop, &err);
        if (handle == SpatialAudio::kInvalidHandle)
        {
            if (outError) *outError = err;
            return false;
        }

        LOG_INFO("[ModFeedback] PlayMediaFile2D ok: " << filePath << " handle=" << handle);
        return true;
    }

    bool PlayMediaUrl2D(const std::string& url, bool loop, float volume, std::string* outError)
    {
        // URL streaming is no longer supported without UMediaPlayer.
        if (outError) *outError = "URL streaming not supported (UE audio removed); use local .wav files";
        LOG_WARN("[ModFeedback] PlayMediaUrl2D: URL playback not supported: " << url);
        return false;
    }

    bool PlayMediaFileAttached3D(const std::string& actorSelector, const std::string& filePath, bool loop, float volume, std::string* outResolvedActor, std::string* outError)
    {
        std::string resolvedName;
        std::string err;
        SDK::AActor* actor = ResolveActorSelector(actorSelector, &resolvedName, &err);
        if (!actor)
        {
            if (outError) *outError = err;
            return false;
        }

        if (!PlayWavSpatial3D(actor, filePath, loop, volume, outError))
            return false;

        if (outResolvedActor) *outResolvedActor = resolvedName;
        return true;
    }

    bool PlayMediaUrlAttached3D(const std::string& actorSelector, const std::string& url, bool loop, float volume, std::string* outResolvedActor, std::string* outError)
    {
        if (outError) *outError = "URL streaming not supported (UE audio removed); use local .wav files";
        LOG_WARN("[ModFeedback] PlayMediaUrlAttached3D: URL playback not supported: " << url);
        return false;
    }

    int StopAllMedia()
    {
        SpatialAudio::StopAll();
        std::lock_guard<std::mutex> lock(gTrackedMutex);
        int count = static_cast<int>(gTrackedSounds.size());
        gTrackedSounds.clear();
        LOG_INFO("[ModFeedback] StopAllMedia: cleared " << count << " tracked entries");
        return count;
    }

    std::string DescribeActiveMedia(std::size_t maxEntries)
    {
        return SpatialAudio::DescribeActive(maxEntries);
    }

    // =====================================================================
    // Sound group system (scanning unchanged)
    // =====================================================================

    bool ScanSoundGroupsFromFolder(const std::string& folderPath, std::string* outError)
    {
        if (folderPath.empty())
        {
            if (outError) *outError = "folder path is empty";
            return false;
        }

        std::error_code ec;
        const std::filesystem::path root(folderPath);
        if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec))
        {
            if (outError) *outError = "folder not found or not a directory";
            return false;
        }

        std::unordered_map<std::string, std::vector<std::string>> discovered;
        int txtFilesRead = 0;
        int txtEntriesAdded = 0;

        std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec);
        std::filesystem::recursive_directory_iterator endIt;
        for (; it != endIt; it.increment(ec))
        {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }

            const std::filesystem::path fp = it->path();
            const std::string extLower = ToLowerCopy(fp.extension().string());

            if (extLower == ".txt")
            {
                const std::string group = DeriveGroupName(fp);
                std::ifstream tf(fp);
                if (!tf.is_open()) continue;
                std::string line;
                int lineAdded = 0;
                while (std::getline(tf, line))
                {
                    line = TrimWhitespace(line);
                    if (line.empty() || line[0] == '#') continue;
                    discovered[group].push_back(line);
                    ++lineAdded;
                    ++txtEntriesAdded;
                }
                ++txtFilesRead;
                LOG_INFO("[ModFeedback] ScanSounds: parsed '" << fp.filename().string() << "' -> group '" << group << "' (" << lineAdded << " entries)");
            }
            else if (IsSupportedAudioExtension(fp))
            {
                const std::string group = DeriveGroupName(fp);
                discovered[group].push_back(fp.string());
            }
        }

        for (auto& entry : discovered)
        {
            std::sort(entry.second.begin(), entry.second.end());
        }

        int newGroups = 0;
        int newEntries = 0;
        {
            std::lock_guard<std::mutex> lock(gSoundGroupMutex);
            for (auto& kv : discovered)
            {
                auto& target = gSoundGroups[kv.first];
                const bool wasEmpty = target.empty();
                for (const auto& e : kv.second)
                {
                    if (std::find(target.begin(), target.end(), e) == target.end())
                    {
                        target.push_back(e);
                        ++newEntries;
                    }
                }
                if (wasEmpty && !target.empty()) ++newGroups;
            }
            gSoundGroupsSourceFolder = root.string();
        }

        LOG_INFO("[ModFeedback] Sound groups scanned from '" << root.string()
            << "' (discovered_groups=" << discovered.size()
            << ", txt_files=" << txtFilesRead
            << ", txt_entries=" << txtEntriesAdded
            << ", new_groups=" << newGroups
            << ", new_entries=" << newEntries << ")");
        return true;
    }

    std::string DescribeSoundGroups(std::size_t maxGroups, std::size_t maxEntriesPerGroup)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;
        std::string source;
        {
            std::lock_guard<std::mutex> lock(gSoundGroupMutex);
            source = gSoundGroupsSourceFolder;
            groups.reserve(gSoundGroups.size());
            for (const auto& kv : gSoundGroups)
                groups.push_back(kv);
        }

        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        std::ostringstream oss;
        oss << "Sound groups: " << groups.size();
        if (!source.empty())
            oss << " (source: " << source << ")";

        std::size_t shownGroups = 0;
        for (const auto& kv : groups)
        {
            if (shownGroups++ >= maxGroups)
            {
                oss << "\n  ...";
                break;
            }

            oss << "\n  " << kv.first << " (" << kv.second.size() << "): ";
            for (std::size_t i = 0; i < kv.second.size() && i < maxEntriesPerGroup; ++i)
            {
                if (i > 0) oss << ", ";
                oss << std::filesystem::path(kv.second[i]).filename().string();
            }

            if (kv.second.size() > maxEntriesPerGroup)
                oss << ", ...";
        }

        return oss.str();
    }

    bool PlayRandomSoundGroup2D(const std::string& groupName, bool loop, float volume, std::string* outChosenFile, std::string* outError)
    {
        const std::string groupKey = ToLowerCopy(groupName);
        if (groupKey.empty())
        {
            if (outError) *outError = "group name is empty";
            return false;
        }

        std::vector<std::string> entries;
        {
            std::lock_guard<std::mutex> lock(gSoundGroupMutex);
            const auto it = gSoundGroups.find(groupKey);
            if (it == gSoundGroups.end() || it->second.empty())
            {
                if (outError) *outError = "group not found or empty";
                return false;
            }
            entries = it->second;
        }

        std::shuffle(entries.begin(), entries.end(), gSoundGroupRng);

        const int maxRetries = (std::min)(Mod::Tuning::kSoundGroupRetryCount, (int)entries.size());
        std::string lastErr;
        for (int attempt = 0; attempt < maxRetries; ++attempt)
        {
            const std::string& entry = entries[attempt];
            std::string err;

            // URLs no longer supported; only local .wav files
            if (IsUrl(entry))
            {
                lastErr = "URL playback not supported";
                continue;
            }

            const bool ok = PlayMediaFile2D(entry, loop, volume, &err);
            if (ok)
            {
                if (outChosenFile) *outChosenFile = entry;
                LOG_INFO("[ModFeedback] PlayRandomSoundGroup2D ok (group='" << groupKey
                    << "', entry='" << entry << "', attempt=" << attempt + 1 << ")");
                return true;
            }

            lastErr = err;
            LOG_WARN("[ModFeedback] PlayRandomSoundGroup2D attempt " << attempt + 1
                << " failed (group='" << groupKey << "', entry='" << entry << "'): " << err);
        }

        if (outError) *outError = "all " + std::to_string(maxRetries) + " attempts failed; last: " + lastErr;
        return false;
    }

    // =====================================================================
    // Actor-attached playback (routes through SpatialAudio)
    // =====================================================================

    bool PlayMediaFileAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        return PlayWavSpatial3D(actor, filePath, loop, volume, outError);
    }

    bool PlayMediaUrlAttachedToActor(SDK::AActor* actor, const std::string& url, bool loop, float volume, std::string* outError)
    {
        if (outError) *outError = "URL streaming not supported (UE audio removed); use local .wav files";
        LOG_WARN("[ModFeedback] PlayMediaUrlAttachedToActor: URL playback not supported: " << url);
        return false;
    }

    bool PlayWavAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        return PlayWavSpatial3D(actor, filePath, loop, volume, outError);
    }

    bool PlaySoundAssetAttachedToActor(SDK::AActor* actor, const std::string& softObjectPath, float volume, std::string* outError)
    {
        // This uses a UE game-asset sound (e.g. MetaSoundSource). Still goes through
        // UE's SpawnSoundAttached since it's a game-native asset, not a custom wav.
        if (!actor)
        {
            if (outError) *outError = "actor is null";
            return false;
        }

        std::string err;
        SDK::USoundBase* sound = LoadSoundAssetBySoftPath(softObjectPath, &err);
        if (!sound)
        {
            if (outError) *outError = err;
            LOG_WARN("[ModFeedback] PlaySoundAssetAttachedToActor failed for path '" << softObjectPath << "': " << err);
            return false;
        }

        SDK::USceneComponent* root = actor->K2_GetRootComponent();
        if (!root)
        {
            if (outError) *outError = "actor has no root component";
            return false;
        }

        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        SDK::UAudioComponent* audComp = SDK::UGameplayStatics::SpawnSoundAttached(
            sound, root,
            SDK::FName{},
            SDK::FVector{0.0f, 0.0f, 0.0f},
            SDK::FRotator{0.0f, 0.0f, 0.0f},
            SDK::EAttachLocation::KeepRelativeOffset,
            false, volume, 1.0f, 0.0f,
            nullptr, nullptr, true);

        if (!audComp)
        {
            if (outError) *outError = "SpawnSoundAttached returned null";
            return false;
        }

        LOG_INFO("[ModFeedback] PlaySoundAssetAttachedToActor ok ('" << softObjectPath
            << "', actor=" << actor->GetName() << ", vol=" << volume << ")");
        return true;
    }

    bool PlayWavAttachedToPlayerWithTemplate(const std::string& /*templateSoftObjectPath*/, const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        // Template-based USoundWave playback is no longer needed with our own audio engine.
        // Route through 2D playback as a simple fallback.
        return PlayMediaFile2D(filePath, loop, volume, outError);
    }

    std::string DescribeLoadedSoundWaves(std::size_t /*maxEntries*/, const std::string& /*containsFilter*/, bool /*includeDefaultObjects*/)
    {
        return "USoundWave discovery removed (SpatialAudio engine in use)";
    }

    bool PlayWavAttachedToPlayerAutoTemplate(const std::string& filePath, bool loop, float volume, const std::string& /*containsFilter*/, std::string* /*outTemplateName*/, std::string* outError)
    {
        return PlayMediaFile2D(filePath, loop, volume, outError);
    }

    // =====================================================================
    // Actor query helpers
    // =====================================================================

    bool IsMediaPlayingForActor(SDK::AActor* actor)
    {
        if (!actor) return false;

        std::lock_guard<std::mutex> lock(gTrackedMutex);
        for (const auto& t : gTrackedSounds)
        {
            if (t.actor == actor && SpatialAudio::IsPlaying(t.handle))
                return true;
        }
        return false;
    }

    int StopMediaForActor(SDK::AActor* actor)
    {
        if (!actor) return 0;
        int stopped = 0;

        std::lock_guard<std::mutex> lock(gTrackedMutex);
        gTrackedSounds.erase(
            std::remove_if(gTrackedSounds.begin(), gTrackedSounds.end(),
                [actor, &stopped](const TrackedActorSound& t)
                {
                    if (t.actor != actor) return false;
                    SpatialAudio::Stop(t.handle);
                    ++stopped;
                    return true;
                }),
            gTrackedSounds.end());

        return stopped;
    }

    bool PlayRandomSoundGroupAttachedToActor(SDK::AActor* actor, const std::string& groupName, bool loop, float volume, std::string* outChosenEntry, std::string* outError)
    {
        if (!actor)
        {
            if (outError) *outError = "actor is null";
            return false;
        }

        const std::string groupKey = ToLowerCopy(groupName);
        if (groupKey.empty())
        {
            if (outError) *outError = "group name is empty";
            return false;
        }

        std::vector<std::string> entries;
        {
            std::lock_guard<std::mutex> lock(gSoundGroupMutex);
            const auto it = gSoundGroups.find(groupKey);
            if (it == gSoundGroups.end() || it->second.empty())
            {
                if (outError) *outError = std::string("group '") + groupKey + "' not found or empty";
                return false;
            }
            entries = it->second;
        }

        std::shuffle(entries.begin(), entries.end(), gSoundGroupRng);

        const int maxRetries = (std::min)(Mod::Tuning::kSoundGroupRetryCount, (int)entries.size());
        std::string lastErr;
        for (int attempt = 0; attempt < maxRetries; ++attempt)
        {
            const std::string& entry = entries[attempt];
            std::string err;

            if (IsUrl(entry))
            {
                lastErr = "URL playback not supported";
                LOG_WARN("[ModFeedback] PlayRandomSoundGroupAttachedToActor: skipping URL entry '" << entry << "'");
                continue;
            }

            // Only .wav files supported
            std::string ext = ToLowerCopy(std::filesystem::path(entry).extension().string());
            if (ext != ".wav")
            {
                lastErr = "only .wav files supported (got " + ext + ")";
                LOG_WARN("[ModFeedback] PlayRandomSoundGroupAttachedToActor: skipping non-wav entry '" << entry << "'");
                continue;
            }

            if (PlayWavSpatial3D(actor, entry, loop, volume, &err))
            {
                if (outChosenEntry) *outChosenEntry = entry;
                LOG_INFO("[ModFeedback] PlayRandomSoundGroupAttachedToActor ok (group='" << groupKey
                    << "', entry='" << entry << "', attempt=" << attempt + 1 << ")");
                return true;
            }

            lastErr = err;
            LOG_WARN("[ModFeedback] PlayRandomSoundGroupAttachedToActor attempt " << attempt + 1
                << " failed (group='" << groupKey << "', entry='" << entry << "'): " << err);
        }

        if (outError) *outError = "all " + std::to_string(maxRetries) + " attempts failed; last: " + lastErr;
        return false;
    }

    // =====================================================================
    // Init
    // =====================================================================

    void InitSoundSystem()
    {
        // Initialize the SpatialAudio engine
        std::string err;
        if (!SpatialAudio::Initialize(&err))
        {
            LOG_ERROR("[ModFeedback] SpatialAudio initialization failed: " << err);
        }
        else
        {
            LOG_INFO("[ModFeedback] SpatialAudio initialized successfully");
        }

        const std::filesystem::path soundsFolder = std::filesystem::current_path() / Mod::Tuning::kDefaultSoundsFolder;
        std::error_code ec;
        const bool exists = std::filesystem::exists(soundsFolder, ec) && std::filesystem::is_directory(soundsFolder, ec);
        if (exists)
        {
            LOG_INFO("[ModFeedback] Default sounds folder found: " << soundsFolder.string() << " -- scanning...");
            std::string scanErr;
            if (!ScanSoundGroupsFromFolder(soundsFolder.string(), &scanErr))
            {
                LOG_WARN("[ModFeedback] Sounds folder scan failed: " << scanErr);
            }
        }
        else
        {
            LOG_INFO("[ModFeedback] Default sounds folder NOT found at: " << soundsFolder.string()
                << " (create 'sounds/' next to the DLL to enable sound groups)");
        }
    }

    void ClearWidgetCache()
    {
        if (gRHLogCount++ < kRHLogLimit)
            LOG_INFO("[ModFeedback][RH] ClearWidgetCache called - clearing cached widget reference");
        
        gRightWidget = nullptr;
        gRightWidgetOwnerPlayer = nullptr;
        gRightWidgetExpiryMs = 0;
    }
}
