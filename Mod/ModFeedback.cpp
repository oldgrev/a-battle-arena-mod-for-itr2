

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
Symptom (media playback): `media_file` / `media_url` / `playrandomsound` reported generic "media play failed" and produced no audible output.
Root cause: 2D media path instantiated a `UMediaPlayer` but did not reliably provide an audio sink component in-world, and diagnostics did not surface the player's runtime state.
Fix: spawn and register a `UMediaSoundComponent` attached to player root for 2D media playback, set `PlayOnOpen`, and include detailed player state (`ready/preparing/buffering/connecting/playing/url/player`) in logs and command errors.
Symptom (3D media crashes 2026-02-26): Friend NPCs with attached 3D audio caused intermittent crashes shortly after media playback started.
Root cause: Media components attached to actors were not cleaned up when actors became invalid or were destroyed. The gActiveMedia vector accumulated stale references with dangling actor pointers.
Fix: Added CleanupStaleMedia() function called from DrainPending() every frame. It checks each media instance for invalid players, invalid owner actors (for 3D attached audio), and naturally finished playback. Stale instances are properly closed (Player->Close()), components destroyed (K2_DestroyComponent), and removed from the tracking vector.
Additional timing fix: do not clean up newly-started players while they are still preparing. Each media entry records a start time and retains itself for at least kMediaStaleCleanupDelaySeconds before being eligible for automatic removal.
Symptom (3D audio not spatializing 2026-02-26): 3D audio attached to friend NPCs sounded centered on the player (equal volume in both ears) even when NPC was 3000 units away.
Root cause: SpawnAttachedMediaSoundComponent enabled bAllowSpatialization but did not configure attenuation settings. Without attenuation overrides, the sound component used default settings (no distance falloff).
Fix: Set bOverrideAttenuation = true and configured AttenuationOverrides struct with: bAttenuate=true, bSpatialize=true, Linear distance algorithm, Sphere shape, FalloffDistance=5000 units, dBAttenuationAtMax=-60dB. Also added low-pass filtering (bAttenuateWithLPF=true, LPF kicks in at 1000 units, fully muffled by 5000 units) for distance realism.
*/

#include "GameContext.hpp"
#include "Logging.hpp"
#include "ModTuning.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
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
        template <typename ElementType>
        struct TArrayRaw
        {
            ElementType* Data;
            int32_t NumElements;
            int32_t MaxElements;
        };

        template <typename ElementType>
        static int32_t GetTArrayNum(const SDK::TArray<ElementType>& arr)
        {
            const auto* raw = reinterpret_cast<const TArrayRaw<ElementType>*>(&arr);
            return raw ? raw->NumElements : 0;
        }

        template <typename ElementType>
        static ElementType* GetTArrayData(const SDK::TArray<ElementType>& arr)
        {
            const auto* raw = reinterpret_cast<const TArrayRaw<ElementType>*>(&arr);
            return raw ? raw->Data : nullptr;
        }

        struct PendingMessage
        {
            std::wstring Text;
            float Seconds;
            SDK::FLinearColor Color;
        };

        static std::mutex gPendingMutex;
        static std::deque<PendingMessage> gPending;
        static std::atomic<bool> gDraining{false};

        struct ActiveMediaInstance
        {
            SDK::UMediaPlayer* Player = nullptr;
            SDK::UMediaSoundComponent* SoundComponent = nullptr;
            SDK::AActor* OwnerActor = nullptr;
            std::string Descriptor;
            bool IsAttached3D = false;
            float StartTime = 0.0f; // world time when playback began
        };

        static std::mutex gMediaMutex;
        static std::vector<ActiveMediaInstance> gActiveMedia;

        // --- WAV-based 3D audio tracking (USoundWave + SpawnSoundAttached) ---
        struct ActiveWavInstance
        {
            SDK::UAudioComponent* AudioComp = nullptr; // returned by SpawnSoundAttached
            SDK::AActor*          OwnerActor = nullptr;
            uint8_t*              PcmBuffer  = nullptr; // heap buffer; freed when done
            std::string           Descriptor;
        };
        static std::mutex gWavMutex;
        static std::vector<ActiveWavInstance> gActiveWav;

        static std::mutex gSoundGroupMutex;
        static std::unordered_map<std::string, std::vector<std::string>> gSoundGroups;
        static std::string gSoundGroupsSourceFolder;
        static std::mt19937 gSoundGroupRng{std::random_device{}()};

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

        static std::wstring ToWide(const std::string& value)
        {
            return std::wstring(value.begin(), value.end());
        }

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

        static SDK::UMediaPlayer* SpawnMediaPlayer(std::string* outError)
        {
            SDK::UObject* outer = Mod::GameContext::GetWorldContext();
            if (!outer)
            {
                if (outError) *outError = "world context is null";
                return nullptr;
            }

            SDK::UObject* spawned = SDK::UGameplayStatics::SpawnObject(SDK::UMediaPlayer::StaticClass(), outer);
            if (!spawned || !spawned->IsA(SDK::UMediaPlayer::StaticClass()))
            {
                if (outError) *outError = "failed to spawn UMediaPlayer";
                return nullptr;
            }

            SDK::UMediaPlayer* player = static_cast<SDK::UMediaPlayer*>(spawned);
            player->PlayOnOpen = true;
            return player;
        }

        static std::string DescribeMediaPlayerState(SDK::UMediaPlayer* player)
        {
            if (!player)
                return "player=<null>";

            std::ostringstream oss;
            oss << "playerName=" << player->GetPlayerName().ToString()
                << " playing=" << (player->IsPlaying() ? 1 : 0)
                << " ready=" << (player->IsReady() ? 1 : 0)
                << " preparing=" << (player->IsPreparing() ? 1 : 0)
                << " buffering=" << (player->IsBuffering() ? 1 : 0)
                << " connecting=" << (player->IsConnecting() ? 1 : 0)
                << " paused=" << (player->IsPaused() ? 1 : 0)
                << " closed=" << (player->IsClosed() ? 1 : 0)
                << " hasError=" << (player->HasError() ? 1 : 0)
                << " canPause=" << (player->CanPause() ? 1 : 0)
                << " supportsSeeking=" << (player->SupportsSeeking() ? 1 : 0)
                << " url='" << player->GetUrl().ToString() << "'";
            return oss.str();
        }

        static bool StartMediaPlayback(SDK::UMediaPlayer* player, bool loop, float volume)
        {
            if (!player)
                return false;

            if (volume < 0.0f) volume = 0.0f;
            if (volume > 4.0f) volume = 4.0f;

            player->SetLooping(loop);
            player->SetNativeVolume(volume);

            if (!player->IsPlaying())
            {
                if (player->Play())
                    return true;

                if (player->IsPlaying())
                    return true;

                if (player->IsPreparing() || player->IsConnecting() || player->IsBuffering())
                    return true;

                if (player->Reopen())
                {
                    if (player->Play() || player->IsPlaying() || player->IsPreparing() || player->IsConnecting() || player->IsBuffering())
                        return true;
                }

                return false;
            }
            return true;
        }

        static bool PlayMediaCommonOpenResult(SDK::UMediaPlayer* player, bool opened, bool loop, float volume, const std::string& descriptor, std::string* outError)
        {
            if (!opened)
            {
                const std::string state = DescribeMediaPlayerState(player);
                if (outError) *outError = "media open failed; " + state;
                LOG_ERROR("[ModFeedback] Media open failed (" << descriptor << ") :: " << state);
                return false;
            }

            if (!StartMediaPlayback(player, loop, volume))
            {
                const std::string state = DescribeMediaPlayerState(player);
                if (outError) *outError = "media play failed; " + state;
                LOG_ERROR("[ModFeedback] Media play failed (" << descriptor << ") :: " << state);
                return false;
            }

            LOG_INFO("[ModFeedback] Media playback started (" << descriptor << ", loop=" << (loop ? "true" : "false") << ", volume=" << volume << ") :: " << DescribeMediaPlayerState(player));
            return true;
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

        // Returns true if the given entry string looks like an http(s) URL.
        static bool IsUrl(const std::string& entry)
        {
            if (entry.size() >= 7 && entry.compare(0, 7, "http://") == 0)  return true;
            if (entry.size() >= 8 && entry.compare(0, 8, "https://") == 0) return true;
            return false;
        }

        // Trim leading/trailing whitespace in place (for txt-line parsing).
        static std::string TrimWhitespace(const std::string& s)
        {
            const auto start = s.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(start, end - start + 1);
        }

        static double DistSq(const SDK::FVector& a, const SDK::FVector& b)
        {
            const double dx = (double)a.X - (double)b.X;
            const double dy = (double)a.Y - (double)b.Y;
            const double dz = (double)a.Z - (double)b.Z;
            return dx * dx + dy * dy + dz * dz;
        }

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
                SDK::AActor** data = GetTArrayData(actors);
                const int32_t count = GetTArrayNum(actors);
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
                    SDK::AActor* actor = data[i];
                    if (!actor || !SDK::UKismetSystemLibrary::IsValid(actor))
                        continue;

                    const SDK::FVector loc = actor->K2_GetActorLocation();
                    const double d2 = DistSq(loc, playerLoc);
                    if (d2 < bestDistSq)
                    {
                        bestDistSq = d2;
                        best = actor;
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

            SDK::TArray<SDK::AActor*> actors;
            SDK::UGameplayStatics::GetAllActorsOfClass(world, SDK::AActor::StaticClass(), &actors);
            SDK::AActor** data = GetTArrayData(actors);
            const int32_t count = GetTArrayNum(actors);
            if (!data || count <= 0)
            {
                if (outError) *outError = "no actors found";
                return nullptr;
            }

            for (int i = 0; i < count; ++i)
            {
                SDK::AActor* actor = data[i];
                if (!actor || !SDK::UKismetSystemLibrary::IsValid(actor))
                    continue;

                const std::string name = ToLowerCopy(actor->GetName());
                const std::string full = ToLowerCopy(actor->GetFullName());
                if (name.find(selector) != std::string::npos || full.find(selector) != std::string::npos)
                {
                    if (outResolvedName) *outResolvedName = actor->GetName();
                    return actor;
                }
            }

            if (outError) *outError = "actor selector did not match any actor";
            return nullptr;
        }

        static SDK::UMediaSoundComponent* SpawnAttachedMediaSoundComponent(SDK::AActor* actor, std::string* outError)
        {
            if (!actor)
            {
                if (outError) *outError = "target actor is null";
                return nullptr;
            }

            SDK::UObject* added = actor->AddComponentByClass(SDK::UMediaSoundComponent::StaticClass(), true, SDK::FTransform{}, false);
            if (!added || !added->IsA(SDK::UMediaSoundComponent::StaticClass()))
            {
                if (outError) *outError = "failed to add UMediaSoundComponent";
                return nullptr;
            }

            SDK::UMediaSoundComponent* mediaSound = static_cast<SDK::UMediaSoundComponent*>(added);

            SDK::USceneComponent* root = actor->K2_GetRootComponent();
            if (!root)
            {
                if (outError) *outError = "target actor has no root component";
                return nullptr;
            }

            const bool attached = mediaSound->K2_AttachToComponent(
                root,
                SDK::FName{},
                SDK::EAttachmentRule::KeepRelative,
                SDK::EAttachmentRule::KeepRelative,
                SDK::EAttachmentRule::KeepRelative,
                false);
            if (!attached)
            {
                if (outError) *outError = "failed to attach media sound component to actor root";
                return nullptr;
            }

            // Configure 3D spatialization and attenuation
            mediaSound->bAllowSpatialization = true;
            mediaSound->bIsUISound = false;
            
            // Enable attenuation override and configure spatial audio settings
            mediaSound->bOverrideAttenuation = true;
            auto& atten = mediaSound->AttenuationOverrides;
            
            // Zero out the struct first (important!)
            memset(&atten, 0, sizeof(atten));
            
            // Enable core spatial features
            atten.bAttenuate = true;
            atten.bSpatialize = true;
            atten.DistanceAlgorithm = SDK::EAttenuationDistanceModel::Linear;
            atten.AttenuationShape = SDK::EAttenuationShape::Sphere;
            
            // Set distance attenuation: full volume up to 500 units, fade to -60dB by 5000 units
            atten.FalloffDistance = 2000.0f;  // Max distance where sound is audible
            atten.dBAttenuationAtMax = -60.0f; // Volume reduction at max distance
            
            // Optionally add low-pass filtering at distance for realism
            atten.bAttenuateWithLPF = true;
            atten.LPFRadiusMin = 100.0f;
            atten.LPFRadiusMax = 2000.0f;
            atten.LPFFrequencyAtMin = 20000.0f; // Full frequency at close range
            atten.LPFFrequencyAtMax = 500.0f;   // Muffled at distance

            mediaSound->Activate(true);

            return mediaSound;
        }

        static SDK::UMediaSoundComponent* SpawnPlayerAnchoredMediaSoundComponent2D(std::string* outError)
        {
            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter();
            if (!player)
            {
                if (outError) *outError = "player not found for 2D media audio sink";
                return nullptr;
            }

            SDK::UMediaSoundComponent* mediaSound = SpawnAttachedMediaSoundComponent(player, outError);
            if (!mediaSound)
                return nullptr;

            mediaSound->bAllowSpatialization = false;
            mediaSound->bIsUISound = true;
            return mediaSound;
        }

        static void RegisterActiveMedia(SDK::UMediaPlayer* player, SDK::UMediaSoundComponent* soundComponent, SDK::AActor* ownerActor, const std::string& descriptor, bool isAttached3D)
        {
            float now = 0.0f;
            if (player)
            {
                now = SDK::UGameplayStatics::GetTimeSeconds(player);
            }
            std::lock_guard<std::mutex> lock(gMediaMutex);
            ActiveMediaInstance inst;
            inst.Player = player;
            inst.SoundComponent = soundComponent;
            inst.OwnerActor = ownerActor;
            inst.Descriptor = descriptor;
            inst.IsAttached3D = isAttached3D;
            inst.StartTime = now;
            gActiveMedia.push_back(inst);
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
            {
                return stem.substr(0, dash);
            }

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

            if (stem.empty())
                return "default";
            return stem;
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

    // =========================================================================
    // WAV-based 3D spatial audio (USoundWave + SpawnSoundAttached)
    // UMediaPlayer can't move/spatialiase properly; WAV file -> raw PCM into
    // USoundWave -> SpawnSoundAttached gives true 3D follow audio.
    //
    // Layout reference (UE 5.5, confirmed by dump offsets):
    //   USoundBase::duration     0x0100 float (EditConst, set via cast)
    //   USoundBase::TotalSamples 0x0108 float (EditConst, set via cast)
    //   USoundWave::NumChannels  0x036C int32  (public)
    //   USoundWave::SampleRate   0x0370 int32  (Protected, set via cast)
    //   USoundWave::Pad_3A0[0]   0x03A0 uint8* RawPCMData  (native non-UPROPERTY)
    //   USoundWave::Pad_3A0[8]   0x03A8 uint32 RawPCMDataSize (native non-UPROPERTY)
    //   These are the first two members of the native block right after
    //   InternalCurves (last UPROPERTY, ends at 0x03A0).
    // =========================================================================

    struct ParsedWav
    {
        std::vector<uint8_t> pcmData;
        uint16_t numChannels = 0;
        uint32_t sampleRate  = 0;
    };

    static bool ParseWavFile(const std::string& path, ParsedWav& out, std::string* outErr)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { if (outErr) *outErr = "cannot open wav: " + path; return false; }
        const auto fileSize = static_cast<size_t>(f.tellg());
        f.seekg(0);

        if (fileSize < 12) { if (outErr) *outErr = "wav too small"; return false; }

        char riff[4]; uint32_t riffSz; char wave[4];
        f.read(riff, 4);
        f.read(reinterpret_cast<char*>(&riffSz), 4);
        f.read(wave, 4);
        if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0)
        {
            if (outErr) *outErr = "not a RIFF/WAVE file";
            return false;
        }

        uint16_t numChannels = 0, bitsPerSample = 0;
        uint32_t sampleRate = 0;
        bool hasFmt = false;

        // Scan chunks — handles LIST/INFO and other chunks between fmt and data
        while (f.good() && static_cast<size_t>(f.tellg()) + 8 <= fileSize)
        {
            char id[4]; uint32_t chunkSz;
            f.read(id, 4);
            f.read(reinterpret_cast<char*>(&chunkSz), 4);
            if (!f) break;
            const auto chunkStart = f.tellg();

            if (std::memcmp(id, "fmt ", 4) == 0)
            {
                uint16_t audioFmt;
                f.read(reinterpret_cast<char*>(&audioFmt), 2);
                if (audioFmt != 1) { if (outErr) *outErr = "wav audioFormat != 1 (not PCM)"; return false; }
                f.read(reinterpret_cast<char*>(&numChannels), 2);
                f.read(reinterpret_cast<char*>(&sampleRate), 4);
                f.seekg(6, std::ios::cur); // skip byteRate (4) + blockAlign (2)
                f.read(reinterpret_cast<char*>(&bitsPerSample), 2);
                if (bitsPerSample != 16) { if (outErr) *outErr = "wav bitsPerSample != 16"; return false; }
                hasFmt = true;
            }
            else if (std::memcmp(id, "data", 4) == 0)
            {
                if (!hasFmt) { if (outErr) *outErr = "data chunk before fmt chunk"; return false; }
                out.pcmData.resize(chunkSz);
                f.read(reinterpret_cast<char*>(out.pcmData.data()), chunkSz);
                if (!f) { if (outErr) *outErr = "truncated data chunk"; return false; }
                out.numChannels = numChannels;
                out.sampleRate  = sampleRate;
                return true;
            }

            // Skip to next chunk (pad to even size)
            const auto next = static_cast<std::streampos>(static_cast<size_t>(chunkStart) + chunkSz + (chunkSz & 1));
            f.seekg(next);
        }

        if (outErr) *outErr = "no data chunk found in wav";
        return false;
    }

    // Native offsets of non-UPROPERTY fields in USoundWave (UE 5.5 layout)
    static constexpr ptrdiff_t kSoundWaveRawPCMDataOffset     = 0x03A0; // uint8*
    static constexpr ptrdiff_t kSoundWaveRawPCMDataSizeOffset = 0x03A8; // uint32
    static constexpr ptrdiff_t kSoundWaveSampleRateOffset     = 0x0370; // int32 (protected)
    static constexpr ptrdiff_t kSoundBaseDurationOffset       = 0x0100; // float (EditConst)
    static constexpr ptrdiff_t kSoundBaseTotalSamplesOffset   = 0x0108; // float (EditConst)

    static SDK::USoundAttenuation* CreateSpatialAttenuation3D()
    {
        SDK::UObject* outer = Mod::GameContext::GetWorldContext();
        if (!outer) return nullptr;
        SDK::UObject* spawned = SDK::UGameplayStatics::SpawnObject(SDK::USoundAttenuation::StaticClass(), outer);
        if (!spawned || !spawned->IsA(SDK::USoundAttenuation::StaticClass())) return nullptr;
        auto* att = static_cast<SDK::USoundAttenuation*>(spawned);

        // FBaseAttenuationSettings: sphere with inner radius and falloff distance
        att->Attenuation.AttenuationShapeExtents = SDK::FVector{Mod::Tuning::kFriendAmbientInnerRadius, 0.0f, 0.0f};
        att->Attenuation.FalloffDistance         = Mod::Tuning::kFriendAmbientAttenuationRadius - Mod::Tuning::kFriendAmbientInnerRadius;
        att->Attenuation.dBAttenuationAtMax      = -60.0f;

        // FSoundAttenuationSettings flags at offset 0x00C0 within the struct
        att->Attenuation.bAttenuate       = 1;
        att->Attenuation.bSpatialize      = 1;
        att->Attenuation.bAttenuateWithLPF = 1;
        att->Attenuation.LPFRadiusMin     = Mod::Tuning::kFriendAmbientLpfStartRadius;
        att->Attenuation.LPFRadiusMax     = Mod::Tuning::kFriendAmbientAttenuationRadius;

        return att;
    }

    // Load a 16-bit PCM wav file into a fresh USoundWave UObject.
    // The returned object is owned by the engine's GC; the PCM heap buffer is
    // tracked in ActiveWavInstance and freed after the audio component finishes.
    static SDK::USoundWave* LoadWavAsSoundWave(const ParsedWav& wav, bool loop, std::string* outErr)
    {
        SDK::UObject* outer = Mod::GameContext::GetWorldContext();
        if (!outer) { if (outErr) *outErr = "world context null"; return nullptr; }

        SDK::UObject* spawned = SDK::UGameplayStatics::SpawnObject(SDK::USoundWave::StaticClass(), outer);
        if (!spawned || !spawned->IsA(SDK::USoundWave::StaticClass()))
        {
            if (outErr) *outErr = "failed to spawn USoundWave";
            return nullptr;
        }
        auto* sw = static_cast<SDK::USoundWave*>(spawned);

        // --- Set reflected UPROPERTY fields ---
        sw->NumChannels = static_cast<int32_t>(wav.numChannels);
        sw->bLooping    = loop ? 1 : 0;

        // Protected / EditConst fields written via offset
        const float totalSamples = static_cast<float>(wav.pcmData.size() / (wav.numChannels * 2u));
        const float duration     = totalSamples / static_cast<float>(wav.sampleRate);
        auto* base = reinterpret_cast<uint8_t*>(sw);
        *reinterpret_cast<int32_t*>(base + kSoundWaveSampleRateOffset)    = static_cast<int32_t>(wav.sampleRate);
        *reinterpret_cast<float*>  (base + kSoundBaseDurationOffset)      = duration;
        *reinterpret_cast<float*>  (base + kSoundBaseTotalSamplesOffset)   = totalSamples;

        // --- Write native PCM pointer and size ---
        const uint32_t pcmSize = static_cast<uint32_t>(wav.pcmData.size());
        uint8_t* heapPcm = new uint8_t[pcmSize];
        std::memcpy(heapPcm, wav.pcmData.data(), pcmSize);

        *reinterpret_cast<uint8_t**>(base + kSoundWaveRawPCMDataOffset)     = heapPcm;
        *reinterpret_cast<uint32_t*>(base + kSoundWaveRawPCMDataSizeOffset) = pcmSize;

        return sw;
    }

    void CleanupStaleWav()
    {
        std::lock_guard<std::mutex> lock(gWavMutex);
        int cleaned = 0;
        gActiveWav.erase(std::remove_if(gActiveWav.begin(), gActiveWav.end(),
            [&cleaned](ActiveWavInstance& inst)
            {
                // Actor gone?
                if (inst.OwnerActor && !SDK::UKismetSystemLibrary::IsValid(inst.OwnerActor))
                {
                    if (inst.AudioComp && SDK::UKismetSystemLibrary::IsValid(inst.AudioComp))
                    {
                        inst.AudioComp->Stop();
                        inst.AudioComp->K2_DestroyComponent(inst.AudioComp);
                    }
                    delete[] inst.PcmBuffer;
                    inst.PcmBuffer = nullptr;
                    ++cleaned;
                    return true;
                }
                // Audio component invalid or done playing?
                if (!inst.AudioComp || !SDK::UKismetSystemLibrary::IsValid(inst.AudioComp))
                {
                    delete[] inst.PcmBuffer;
                    inst.PcmBuffer = nullptr;
                    ++cleaned;
                    return true;
                }
                if (!inst.AudioComp->IsPlaying())
                {
                    inst.AudioComp->K2_DestroyComponent(inst.AudioComp);
                    delete[] inst.PcmBuffer;
                    inst.PcmBuffer = nullptr;
                    ++cleaned;
                    return true;
                }
                return false;
            }
        ), gActiveWav.end());

        if (cleaned > 0)
            LOG_INFO("[ModFeedback] CleanupStaleWav: freed " << cleaned << " wav instance(s)");
    }

    void CleanupStaleMedia()
    {
        std::lock_guard<std::mutex> lock(gMediaMutex);
        int cleaned = 0;
        gActiveMedia.erase(std::remove_if(gActiveMedia.begin(), gActiveMedia.end(),
            [&cleaned](const ActiveMediaInstance& inst)
            {
                // Check if the media player is invalid or finished
                if (!inst.Player || !SDK::UKismetSystemLibrary::IsValid(inst.Player))
                {
                    ++cleaned;
                    return true;
                }
                
                // Check if the owner actor (for 3D attached audio) became invalid
                if (inst.IsAttached3D && inst.OwnerActor && !SDK::UKismetSystemLibrary::IsValid(inst.OwnerActor))
                {
                    LOG_INFO("[ModFeedback] CleanupStaleMedia: actor invalid, stopping media (" << inst.Descriptor << ")");
                    inst.Player->Close();
                    if (inst.SoundComponent && SDK::UKismetSystemLibrary::IsValid(inst.SoundComponent))
                    {
                        inst.SoundComponent->Deactivate();
                        inst.SoundComponent->K2_DestroyComponent(inst.SoundComponent);
                    }
                    ++cleaned;
                    return true;
                }
                
                // Do not aggressively clean media that just started; allow time to prepare/play.
                float nowTime = SDK::UGameplayStatics::GetTimeSeconds(inst.Player);
                if (nowTime - inst.StartTime < Mod::Tuning::kMediaStaleCleanupDelaySeconds)
                {
                    return false;
                }

                // Check if media finished playing naturally or failed to ever start (not ready).
                if (!inst.Player->IsPlaying() && !inst.Player->IsPreparing() && !inst.Player->IsReady())
                {
                    if (inst.SoundComponent && SDK::UKismetSystemLibrary::IsValid(inst.SoundComponent))
                    {
                        inst.SoundComponent->Deactivate();
                        inst.SoundComponent->K2_DestroyComponent(inst.SoundComponent);
                    }
                    ++cleaned;
                    return true;
                }
                
                return false;
            }
        ), gActiveMedia.end());
        
        if (cleaned > 0)
        {
            LOG_INFO("[ModFeedback] CleanupStaleMedia: cleaned " << cleaned << " media instance(s)");
        }
    }

    void DrainPending()
    {
        // Avoid recursion if draining triggers more feedback.
        bool expected = false;
        if (!gDraining.compare_exchange_strong(expected, true))
            return;

        // Clean up stale media instances periodically
        CleanupStaleMedia();
        CleanupStaleWav();

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

    bool PlayMediaFile2D(const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        std::string err;
        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player)
        {
            if (outError) *outError = err;
            return false;
        }

        SDK::UMediaSoundComponent* mediaSound = SpawnPlayerAnchoredMediaSoundComponent2D(&err);
        if (!mediaSound)
        {
            if (outError) *outError = err;
            LOG_ERROR("[ModFeedback] media_file 2D sink creation failed for '" << filePath << "': " << err);
            return false;
        }
        mediaSound->SetMediaPlayer(player);

        const bool opened = player->OpenFile(SDK::FString(ToWide(filePath).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("file: ") + filePath, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, nullptr, std::string("2d file: ") + filePath, false);
        return true;
    }

    bool PlayMediaUrl2D(const std::string& url, bool loop, float volume, std::string* outError)
    {
        std::string err;
        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player)
        {
            if (outError) *outError = err;
            return false;
        }

        SDK::UMediaSoundComponent* mediaSound = SpawnPlayerAnchoredMediaSoundComponent2D(&err);
        if (!mediaSound)
        {
            if (outError) *outError = err;
            LOG_ERROR("[ModFeedback] media_url 2D sink creation failed for '" << url << "': " << err);
            return false;
        }
        mediaSound->SetMediaPlayer(player);

        const bool opened = player->OpenUrl(SDK::FString(ToWide(url).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("url: ") + url, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, nullptr, std::string("2d url: ") + url, false);
        return true;
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

        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player)
        {
            if (outError) *outError = err;
            return false;
        }

        SDK::UMediaSoundComponent* mediaSound = SpawnAttachedMediaSoundComponent(actor, &err);
        if (!mediaSound)
        {
            if (outError) *outError = err;
            return false;
        }

        mediaSound->SetMediaPlayer(player);
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        const bool opened = player->OpenFile(SDK::FString(ToWide(filePath).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("file3d: ") + filePath + " -> " + resolvedName, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, actor, std::string("3d file: ") + filePath + " -> " + resolvedName, true);
        if (outResolvedActor) *outResolvedActor = resolvedName;
        return true;
    }

    bool PlayMediaUrlAttached3D(const std::string& actorSelector, const std::string& url, bool loop, float volume, std::string* outResolvedActor, std::string* outError)
    {
        std::string resolvedName;
        std::string err;
        SDK::AActor* actor = ResolveActorSelector(actorSelector, &resolvedName, &err);
        if (!actor)
        {
            if (outError) *outError = err;
            return false;
        }

        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player)
        {
            if (outError) *outError = err;
            return false;
        }

        SDK::UMediaSoundComponent* mediaSound = SpawnAttachedMediaSoundComponent(actor, &err);
        if (!mediaSound)
        {
            if (outError) *outError = err;
            return false;
        }

        mediaSound->SetMediaPlayer(player);
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        const bool opened = player->OpenUrl(SDK::FString(ToWide(url).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("url3d: ") + url + " -> " + resolvedName, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, actor, std::string("3d url: ") + url + " -> " + resolvedName, true);
        if (outResolvedActor) *outResolvedActor = resolvedName;
        return true;
    }

    int StopAllMedia()
    {
        std::lock_guard<std::mutex> lock(gMediaMutex);
        int stopped = 0;
        for (const ActiveMediaInstance& instance : gActiveMedia)
        {
            if (instance.Player && SDK::UKismetSystemLibrary::IsValid(instance.Player))
            {
                instance.Player->Close();
                ++stopped;
            }

            if (instance.SoundComponent && SDK::UKismetSystemLibrary::IsValid(instance.SoundComponent))
            {
                instance.SoundComponent->Deactivate();
                instance.SoundComponent->K2_DestroyComponent(instance.SoundComponent);
            }
        }
        gActiveMedia.clear();
        return stopped;
    }

    std::string DescribeActiveMedia(std::size_t maxEntries)
    {
        std::lock_guard<std::mutex> lock(gMediaMutex);
        std::ostringstream oss;
        oss << "Active media: " << gActiveMedia.size();

        std::size_t shown = 0;
        for (const ActiveMediaInstance& instance : gActiveMedia)
        {
            if (shown++ >= maxEntries)
            {
                oss << "\n  ...";
                break;
            }

            const bool validPlayer = (instance.Player && SDK::UKismetSystemLibrary::IsValid(instance.Player));
            oss << "\n  [" << shown << "] "
                << (instance.IsAttached3D ? "3D" : "2D")
                << " " << (validPlayer ? "playing" : "invalid")
                << " :: " << instance.Descriptor;
        }

        return oss.str();
    }

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

        // discovered accumulates entries from both audio files and .txt files.
        // Groups are cumulative: spotted-man1.wav + spotted.txt both end up in group 'spotted'.
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
                // Each non-empty, non-comment line is a file path or URL.
                // The group name comes from the .txt file's stem (same DeriveGroupName logic).
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

        // Sort each group's entries for determinism.
        for (auto& entry : discovered)
        {
            std::sort(entry.second.begin(), entry.second.end());
        }

        int newGroups = 0;
        int newEntries = 0;
        {
            std::lock_guard<std::mutex> lock(gSoundGroupMutex);
            // Merge cumulatively into existing gSoundGroups so calling scan twice
            // (or scanning multiple folders) accumulates rather than replaces entries.
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
            {
                groups.push_back(kv);
            }
        }

        std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b)
        {
            return a.first < b.first;
        });

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
                if (i > 0)
                    oss << ", ";
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

        // Copy the group entries under lock so we can iterate without holding the lock during playback.
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

        // Shuffle to get random order for retry.
        std::shuffle(entries.begin(), entries.end(), gSoundGroupRng);

        const int maxRetries = (std::min)(Mod::Tuning::kSoundGroupRetryCount, (int)entries.size());
        std::string lastErr;
        for (int attempt = 0; attempt < maxRetries; ++attempt)
        {
            const std::string& entry = entries[attempt];
            std::string err;
            // Discern URL vs file path for correct playback method.
            const bool ok = IsUrl(entry)
                ? PlayMediaUrl2D(entry, loop, volume, &err)
                : PlayMediaFile2D(entry, loop, volume, &err);

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
        LOG_ERROR("[ModFeedback] PlayRandomSoundGroup2D exhausted retries (group='" << groupKey << "')");
        return false;
    }

    // ---------------------------------------------------------------------------
    // Actor-attached media helpers (actor pointer variants, no string selector)
    // ---------------------------------------------------------------------------

    bool PlayMediaFileAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        if (!actor)
        {
            if (outError) *outError = "actor is null";
            return false;
        }
        std::string err;
        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player) { if (outError) *outError = err; return false; }

        SDK::UMediaSoundComponent* mediaSound = SpawnAttachedMediaSoundComponent(actor, &err);
        if (!mediaSound) { if (outError) *outError = err; return false; }

        mediaSound->SetMediaPlayer(player);
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        const bool opened = player->OpenFile(SDK::FString(ToWide(filePath).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("file3d@actor: ") + filePath, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, actor, std::string("3d file@actor: ") + filePath, true);
        return true;
    }

    bool PlayMediaUrlAttachedToActor(SDK::AActor* actor, const std::string& url, bool loop, float volume, std::string* outError)
    {
        if (!actor)
        {
            if (outError) *outError = "actor is null";
            return false;
        }
        std::string err;
        SDK::UMediaPlayer* player = SpawnMediaPlayer(&err);
        if (!player) { if (outError) *outError = err; return false; }

        SDK::UMediaSoundComponent* mediaSound = SpawnAttachedMediaSoundComponent(actor, &err);
        if (!mediaSound) { if (outError) *outError = err; return false; }

        mediaSound->SetMediaPlayer(player);
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        const bool opened = player->OpenUrl(SDK::FString(ToWide(url).c_str()));
        if (!PlayMediaCommonOpenResult(player, opened, loop, volume, std::string("url3d@actor: ") + url, outError))
            return false;

        RegisterActiveMedia(player, mediaSound, actor, std::string("3d url@actor: ") + url, true);
        return true;
    }

    // Play a 16-bit PCM .wav file attached to a moving actor for true 3D spatial audio.
    // Uses SpawnSoundAttached (not UMediaPlayer) so the sound follows the actor in 3D space.
    bool PlayWavAttachedToActor(SDK::AActor* actor, const std::string& filePath, bool loop, float volume, std::string* outError)
    {
        if (!actor) { if (outError) *outError = "actor is null"; return false; }

        std::string err;
        ParsedWav wav;
        if (!ParseWavFile(filePath, wav, &err))
        {
            if (outError) *outError = err;
            LOG_ERROR("[ModFeedback] PlayWavAttachedToActor ParseWavFile failed: " << err);
            return false;
        }

        SDK::USoundWave* sw = LoadWavAsSoundWave(wav, loop, &err);
        if (!sw)
        {
            if (outError) *outError = err;
            LOG_ERROR("[ModFeedback] PlayWavAttachedToActor LoadWavAsSoundWave failed: " << err);
            return false;
        }

        SDK::USoundAttenuation* att = CreateSpatialAttenuation3D();
        // att may be null; SpawnSoundAttached accepts null attenuation (falls back to asset attenuation)

        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f;

        SDK::USceneComponent* root = actor->K2_GetRootComponent();
        if (!root)
        {
            if (outError) *outError = "actor has no root component";
            return false;
        }

        SDK::UAudioComponent* audComp = SDK::UGameplayStatics::SpawnSoundAttached(
            sw,
            root,
            SDK::FName{},
            SDK::FVector{0.0f, 0.0f, 0.0f},
            SDK::FRotator{0.0f, 0.0f, 0.0f},
            SDK::EAttachLocation::KeepRelativeOffset,
            false,   // bStopWhenAttachedToDestroyed
            volume,
            1.0f,    // pitch
            0.0f,    // start time
            att,
            nullptr, // no concurrency
            !loop    // bAutoDestroy when non-looping
        );

        if (!audComp)
        {
            if (outError) *outError = "SpawnSoundAttached returned null";
            LOG_ERROR("[ModFeedback] PlayWavAttachedToActor SpawnSoundAttached failed for: " << filePath);
            return false;
        }

        // Retrieve the heap buffer we put into sw so we can free it after playback
        uint8_t* pcmBuf = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(sw) + kSoundWaveRawPCMDataOffset);

        {
            std::lock_guard<std::mutex> lock(gWavMutex);
            gActiveWav.push_back({audComp, actor, pcmBuf, filePath});
        }

        LOG_INFO("[ModFeedback] PlayWavAttachedToActor ok (" << filePath << ", loop=" << loop
            << ", vol=" << volume << ", ch=" << wav.numChannels
            << ", sr=" << wav.sampleRate << ")");
        return true;
    }

    bool IsMediaPlayingForActor(SDK::AActor* actor)
    {
        if (!actor) return false;
        {
            std::lock_guard<std::mutex> lock(gMediaMutex);
            for (const auto& inst : gActiveMedia)
            {
                if (inst.OwnerActor != actor) continue;
                if (inst.Player && SDK::UKismetSystemLibrary::IsValid(inst.Player) && inst.Player->IsPlaying())
                    return true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(gWavMutex);
            for (const auto& inst : gActiveWav)
            {
                if (inst.OwnerActor != actor) continue;
                if (inst.AudioComp && SDK::UKismetSystemLibrary::IsValid(inst.AudioComp) && inst.AudioComp->IsPlaying())
                    return true;
            }
        }
        return false;
    }

    int StopMediaForActor(SDK::AActor* actor)
    {
        if (!actor) return 0;
        int stopped = 0;
        {
            std::lock_guard<std::mutex> lock(gMediaMutex);
            gActiveMedia.erase(std::remove_if(gActiveMedia.begin(), gActiveMedia.end(),
                [actor, &stopped](const ActiveMediaInstance& inst)
                {
                    if (inst.OwnerActor != actor) return false;
                    if (inst.Player && SDK::UKismetSystemLibrary::IsValid(inst.Player))
                    {
                        inst.Player->Close();
                        ++stopped;
                    }
                    if (inst.SoundComponent && SDK::UKismetSystemLibrary::IsValid(inst.SoundComponent))
                    {
                        inst.SoundComponent->Deactivate();
                        inst.SoundComponent->K2_DestroyComponent(inst.SoundComponent);
                    }
                    return true;
                }
            ), gActiveMedia.end());
        }
        {
            std::lock_guard<std::mutex> lock(gWavMutex);
            gActiveWav.erase(std::remove_if(gActiveWav.begin(), gActiveWav.end(),
                [actor, &stopped](ActiveWavInstance& inst)
                {
                    if (inst.OwnerActor != actor) return false;
                    if (inst.AudioComp && SDK::UKismetSystemLibrary::IsValid(inst.AudioComp))
                    {
                        inst.AudioComp->Stop();
                        inst.AudioComp->K2_DestroyComponent(inst.AudioComp);
                        ++stopped;
                    }
                    delete[] inst.PcmBuffer;
                    inst.PcmBuffer = nullptr;
                    return true;
                }
            ), gActiveWav.end());
        }
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
            // .wav files use SpawnSoundAttached for true 3D spatial audio;
            // URLs and other file types fall through to the UMediaPlayer path.
            const bool isWav = !IsUrl(entry) && entry.size() >= 4
                && (entry.compare(entry.size() - 4, 4, ".wav") == 0
                 || entry.compare(entry.size() - 4, 4, ".WAV") == 0);
            const bool ok = isWav
                ? PlayWavAttachedToActor(actor, entry, loop, volume, &err)
                : (IsUrl(entry)
                    ? PlayMediaUrlAttachedToActor(actor, entry, loop, volume, &err)
                    : PlayMediaFileAttachedToActor(actor, entry, loop, volume, &err));

            if (ok)
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
        LOG_ERROR("[ModFeedback] PlayRandomSoundGroupAttachedToActor exhausted retries (group='" << groupKey << "')");
        return false;
    }

    void InitSoundSystem()
    {
        const std::filesystem::path soundsFolder = std::filesystem::current_path() / Mod::Tuning::kDefaultSoundsFolder;
        std::error_code ec;
        const bool exists = std::filesystem::exists(soundsFolder, ec) && std::filesystem::is_directory(soundsFolder, ec);
        if (exists)
        {
            LOG_INFO("[ModFeedback] Default sounds folder found: " << soundsFolder.string() << " -- scanning...");
            std::string err;
            if (!ScanSoundGroupsFromFolder(soundsFolder.string(), &err))
            {
                LOG_WARN("[ModFeedback] Sounds folder scan failed: " << err);
            }
        }
        else
        {
            LOG_INFO("[ModFeedback] Default sounds folder NOT found at: " << soundsFolder.string()
                << " (create 'sounds/' next to the DLL to enable sound groups)");
        }
    }
}
