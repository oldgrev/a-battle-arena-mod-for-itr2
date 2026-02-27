// AI SHOULD:
// ALWAYS INCLUDE ALL LEARNINGS IN THIS AREA. 
//  If you implement code, find it's wrong and have to modify it, then add as a learing to not do things that way in the future. e.g. using types that don't exist.
//  A learning isn't just describing what this code does, it's when you've stuffed up and had to change it
//
// THIS IS FOR A VR GAME MOD FOR INTO THE RADIUS 2
// EVERY TIME I HAVE TO COMPILE AND TEST, IT'S A PAIN IN THE NECK. THIS IS NOT SOME 2D MOD WHERE I CAN JUST RELOAD ASSETS
// SO, DO NOT WASTE TIME ON CODE THAT IS A MINOR UPDATE THAT MIGHT NOT EVEN WORK.
// YOU'RE BETTER BITING OFF MORE AND MAYBE IT RUNS BAD OR CRASHES ON LOAD, THAN WASTING WITH A BUILD, COPY, LOAD, DON VR HEADSET, BLINDLY USE KEYBOARD TO ENABLE CHEATS etc.
// AT THE SAME TIME, NEW CODE SHOULD LOG, SO IF IT BREAKS, WE KNOW WHERE.
// I HAVE 3 HOURS IN THE EVENING TO WORK ON THIS, I AM LEANING ON AI TO HELP ACCELERATE HERE, BUT IF THE AI FEELS OUT OF IT'S DEPTH, IT SHOULDN'T PUT FORWARD THINGS ITS NOT CONFIDENT ABOUT
//
//
// AILEARNINGS HERE:
// - When implementing the ProcessEvent hook, I initially forgot to include a reentry guard. This caused infinite recursion when the hook called originalFn, which also went through the hook. Adding a thread-local boolean reentryGuard fixed this issue.
// - Reentry guard scope matters: guarding the entire named-hook handler (including calling originalFn) prevents nested ProcessEvent calls from being intercepted.
//   That broke unlimited ammo because TryExtractNextItem can be invoked during ServerShootProjectile's original call chain. Fix: only guard the resync SDK calls (ForceResyncFirearm), not the whole handler.
// - SDK::UObject::GObjects is a TUObjectArrayWrapper. You MUST use '->' to access Num() or other TUObjectArray members (e.g. GObjects->Num()), not '.', otherwise it won't compile.
// - Compilation gotcha: the dumped SDK "*_parameters.hpp" structs live under `namespace SDK::Params`, but that namespace isn't guaranteed visible everywhere via the umbrella include.
//   Fix: for ProcessEvent hooks, prefer defining a minimal local params struct that matches the function signature (as we already do for other hooks).
// - Trace-mode compile failure: a large diagnostics block was accidentally inserted inside the `UnsafeTArrayNum` template function body, causing cascading C2760 syntax errors and missing identifiers.
//   Fix: restore `UnsafeTArrayNum` to a minimal layout-based implementation and keep trace code + HookManager::Trace_* methods at top-level scope.
// - Trace JSON log compile error (C2001 newline in constant): an extra quote in the JSON builder (`... << "\\\""";`) accidentally created an unterminated string literal.
//   Fix: ensure the JSON line ends with exactly one closing quote for the field (`... << "\\\"";`).
// - Tablet diag compile error: some SDK actor types (e.g. `SDK::ARadiusGrippableActorBase`) don't expose `GetClass()` helpers in this generator version.
//   Fix: avoid `GetClass()` and rely on `GetFullName()` (which includes class/name context) or access the UClass pointer only if the SDK actually exposes it.
// - Trace/notification gotcha: ProcessEvent tracing will show `BP_RadiusPlayerCharacter_Gameplay_C.ShowSubtitles`, but it will NOT show `UFLGeneral::ShowMessage` because that's a native static call (not a reflected ProcessEvent).
//   Fix: when bridging notifications, log an explicit `[NotifBridge] MIRROR ...` line so we can prove popup emission in one VR run.
// - UE4 Interface casting: SDK interfaces (e.g. IRadiusDataComponentInterface) cannot be used via reinterpret_cast<IInterface*>(UObject*) because interface vtables differ from UObject vtables.
//   Fix: use ProcessEvent directly: find the UFunction via `obj->Class->GetFunction("InterfaceName", "FunctionName")`, define a local params struct matching the SDK Params:: layout, and call `obj->ProcessEvent(fn, &params)`.
// - TWeakObjectPtr resolution: This SDK version's TUObjectArray has `GetByIndex(int32)` returning UObject*, NOT `GetItemByIndex()`. FUObjectItem has no `GetSerialNumber()`.
//   Fix: use `GObjects->GetByIndex(objectIndex)` directly and skip serial number validation.
// - SDK::FMemory::Realloc does not exist in this SDK version. The SDK uses CRT malloc/realloc internally (see UnrealContainers.hpp).
//   Fix: use CRT `realloc()` for TArray buffer growth in manual array manipulation.
// - AutoMag hook: OnItemHolsterAttachChanged_Event only fires on BPC_ItemHolster_C subclasses (weapon mag slots, hands). Body pouch holsters
//   are native URadiusHolsterComponent instances WITHOUT a BPC layer, so that event NEVER fires for them.
//   Fix: hook SetHolsteredActor instead, which fires via ProcessEvent for ALL holster types (native + BPC). Filter out weapon mag slots
//   (UBPC_RadiusMagazineSlot_C, crashes) and hand holsters (UBPC_PlayerCharacterHandHolster_C, spam). The existing Hook_SetHolsteredActor
//   was already catching tablet holster events, confirming it works for native holsters.
// - AutoMag holster classification: checking object name for "Magazine" substring caused false positive on BPC_RadiusMagazineSlot_C (weapon mag slot).
//   Fix: use IsA() class checks to exclude known-bad holster types rather than string matching.
// - AutoMag crash: filling a magazine while being loaded into a weapon mag slot can crash. Weapon mag slots do complex things on attach.
//   Fix: ALWAYS exclude UBPC_RadiusMagazineSlot_C from AutoMag processing, and wrap ALL AutoMag logic in try/catch.
// - AutoMag capacity: GetAmmoContainerStaticData UFunction not found on some DataComponent classes. The interface function "RadiusDataComponentInterface"
//   may not be implemented by all DataComponent subclasses.
//   Fix: add a known-capacity lookup table keyed by magazine tag substrings (PM.Short→8, AK-74→30, etc.) and a tag-suffix parser as fallbacks.
//


#include "HookManager.hpp"
#include "Cheats.hpp"
#include "ModMain.hpp"
#include "CommandQueue.hpp"
#include "Logging.hpp"
#include "ArenaSubsystem.hpp"
#include "VRMenuSubsystem.hpp"

#include "ModFeedback.hpp"
#include "GameContext.hpp"

#include <Windows.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>
#include <unordered_set>

namespace Mod
{
    // Fix for C2039: ScopedProcessEventGuard is in Mod namespace, not HookManager.
    // Ensure the compiler sees ScopedProcessEventGuard within Mod namespace before usage in HookManager methods.

    // Forward decl to avoid include cycles; implemented in Mod/CommandHandler.cpp
    class Cheats;
    Cheats *GetCheats();

    namespace
    {
        constexpr int kUnlimitedMagazineTarget = 30;
        SDK::FGameplayTag gLastObservedAmmoTag{};
        bool gHasLastObservedAmmoTag = false;

        // Track when we are inside a held-weapon firing chain so that nested ProcessEvent calls
        // (e.g. TryExtractNextItem) can be reliably scoped to the currently firing weapon.
        // This avoids touching shop-spawned / world weapons and reduces VR trial-and-error.
        static thread_local SDK::URadiusFirearmComponent *gActiveShotComponent = nullptr;
        static thread_local SDK::AActor *gActiveShotOwner = nullptr;

        template <typename ElementType>
        static int32_t UnsafeTArrayNum(const UC::TArray<ElementType> &array)
        {
            // We intentionally avoid calling SDK TArray helpers here. In this codebase, some generated
            // TArray instantiations used in params/rep structs don't expose the expected methods in all TUs.
            // UE TArray layout is stable: {Data pointer, Num, Max}.
            struct Layout
            {
                ElementType *Data;
                int32_t NumElements;
                int32_t MaxElements;
            };

            return reinterpret_cast<const Layout *>(&array)->NumElements;
        }

        // ---------------------------------------------------------------------
        // Diagnostics: "log literally everything" ProcessEvent trace mode.
        // ---------------------------------------------------------------------
        struct TraceEvent
        {
            SDK::UObject* Object;
            SDK::UFunction* Function;
            uint32_t ThreadId;
        };

        static std::atomic<bool> gTraceEnabled{false};
        static std::atomic<uint32_t> gTraceWriteIndex{0};
        static constexpr uint32_t kTraceRingSize = 8192;
        static std::array<TraceEvent, kTraceRingSize> gTraceRing{};

        // Counts by function pointer (resolve name on dump) to avoid string work in the hot path.
        static std::mutex gTraceMutex;
        static std::unordered_map<SDK::UFunction*, uint32_t> gTraceFnCounts;
        static std::string gTraceFilterLower; // if non-empty: only trace when function full name contains this substring
        static std::string gTraceObjectFilterLower; // if non-empty: only trace when object full name contains this substring

        // ---------------------------------------------------------------------
        // Diagnostics toggles (kept cheap in ProcessEvent hot path)
        // ---------------------------------------------------------------------
        static std::atomic<bool> gTabletInteractionDiagEnabled{false};
        static std::atomic<bool> gNotifBridgeEnabled{false};
        static std::atomic<bool> gNotifBridgePlaySound{false};

        // Last tablet interaction summary for TCP retrieval.
        static std::mutex gTabletInteractionMutex;
        static std::string gLastTabletInteraction;

        // UC::FString is non-owning in this runtime. Keep buffers stable.
        static SDK::FString MakeStableFString(const std::wstring& value)
        {
            static thread_local std::array<std::wstring, 32> ring;
            static thread_local uint32_t ringIndex = 0;
            std::wstring& slot = ring[ringIndex++ % ring.size()];
            slot = value;
            return SDK::FString(slot.c_str());
        }

        static std::wstring WidenLossy(const std::string& s)
        {
            return std::wstring(s.begin(), s.end());
        }

        struct TraceFileWriter
        {
            std::mutex Mutex;
            std::ofstream File;
            std::string Path;
            std::string Buffer;
            uint64_t BytesWritten = 0;
            uint64_t LastFlushTickMs = 0;

            static std::string GetTempTracePath()
            {
                char tempPath[MAX_PATH] = {};
                DWORD len = GetTempPathA(MAX_PATH, tempPath);
                if (len == 0 || len >= MAX_PATH)
                {
                    return std::string("C:\\itr2_trace.log");
                }

                std::string path(tempPath);
                if (!path.empty() && path.back() != '\\')
                    path.push_back('\\');
                path += "itr2_trace.log";
                return path;
            }

            static std::string CurrentTimeString()
            {
                SYSTEMTIME st;
                GetLocalTime(&st);
                char buffer[64] = {};
                snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                return std::string(buffer);
            }

            static uint64_t NowTickMs()
            {
                return static_cast<uint64_t>(GetTickCount64());
            }

            static std::string JsonEscape(const std::string& s)
            {
                std::string out;
                out.reserve(s.size() + 8);
                for (char ch : s)
                {
                    switch (ch)
                    {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(ch) < 0x20)
                        {
                            char buf[8] = {};
                            snprintf(buf, sizeof(buf), "\\u%04x", (unsigned int)(unsigned char)ch);
                            out += buf;
                        }
                        else
                        {
                            out.push_back(ch);
                        }
                        break;
                    }
                }
                return out;
            }

            void EnsureOpenLocked(bool truncate)
            {
                if (Path.empty())
                    Path = GetTempTracePath();

                if (File.is_open())
                {
                    if (!truncate)
                        return;
                    File.close();
                }

                if (truncate)
                    File.open(Path, std::ios::out | std::ios::trunc);
                else
                    File.open(Path, std::ios::out | std::ios::app);

                if (!File.is_open())
                {
                    // Fall back: disable buffering so callers still progress without crashing.
                    Buffer.clear();
                }
            }

            void AppendLineLocked(const std::string& line)
            {
                Buffer.append(line);
                Buffer.push_back('\n');

                const uint64_t now = NowTickMs();
                const bool sizeFlush = Buffer.size() >= 256 * 1024;
                const bool timeFlush = (LastFlushTickMs == 0) || (now - LastFlushTickMs >= 500);
                if (sizeFlush || timeFlush)
                    FlushLocked();
            }

            void FlushLocked()
            {
                if (!File.is_open())
                {
                    LastFlushTickMs = NowTickMs();
                    Buffer.clear();
                    return;
                }

                if (!Buffer.empty())
                {
                    File.write(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
                    File.flush();
                    BytesWritten += Buffer.size();
                    Buffer.clear();
                }

                LastFlushTickMs = NowTickMs();
            }

            void WriteMarkerLocked(const char* type, const std::string& detailsJson)
            {
                EnsureOpenLocked(false);
                std::ostringstream oss;
                oss << "{\"type\":\"" << type << "\",\"ts\":\"" << CurrentTimeString() << "\"";
                if (!detailsJson.empty())
                    oss << "," << detailsJson;
                oss << "}";
                AppendLineLocked(oss.str());
            }

            void ResetFile()
            {
                std::lock_guard<std::mutex> lock(Mutex);
                EnsureOpenLocked(true);
                Buffer.clear();
                BytesWritten = 0;
                LastFlushTickMs = 0;
                WriteMarkerLocked("TRACE_RESET", "\"note\":\"trace file truncated\"");
                FlushLocked();
            }

            void OnEnabledChanged(bool enabled, const std::string& fnFilter, const std::string& objFilter)
            {
                std::lock_guard<std::mutex> lock(Mutex);
                EnsureOpenLocked(false);

                std::ostringstream details;
                details << "\"path\":\"" << JsonEscape(Path) << "\""
                        << ",\"enabled\":" << (enabled ? "true" : "false")
                        << ",\"fnFilter\":\"" << JsonEscape(fnFilter) << "\""
                        << ",\"objFilter\":\"" << JsonEscape(objFilter) << "\"";

                WriteMarkerLocked(enabled ? "TRACE_ENABLED" : "TRACE_DISABLED", details.str());
                FlushLocked();

                if (!enabled && File.is_open())
                {
                    File.close();
                }
            }

            void Flush()
            {
                std::lock_guard<std::mutex> lock(Mutex);
                EnsureOpenLocked(false);
                FlushLocked();
            }

            std::string GetPathCopy()
            {
                std::lock_guard<std::mutex> lock(Mutex);
                if (Path.empty())
                    Path = GetTempTracePath();
                return Path;
            }
        };

        static TraceFileWriter gTraceFile;

        static std::string ToLowerCopy(std::string s)
        {
            for (char& c : s) c = (char)tolower((unsigned char)c);
            return s;
        }

        static bool TracePassesFilter(const std::string& fnFullLower, const std::string& objFullLower)
        {
            std::string filter;
            std::string objFilter;
            {
                std::lock_guard<std::mutex> lock(gTraceMutex);
                filter = gTraceFilterLower;
                objFilter = gTraceObjectFilterLower;
            }

            if (!filter.empty())
            {
                if (fnFullLower.find(filter) == std::string::npos)
                    return false;
            }
            if (!objFilter.empty())
            {
                if (objFullLower.find(objFilter) == std::string::npos)
                    return false;
            }
            return true;
        }

        static void TraceOnProcessEvent(SDK::UObject* obj, SDK::UFunction* fn)
        {
            if (!gTraceEnabled.load(std::memory_order_relaxed))
                return;
            if (!obj || !fn)
                return;

            // In trace mode we intentionally do string work so that the output is self-contained.
            // This keeps the trace log usable even if the process crashes.
            const std::string fnFull = fn->GetFullName();
            const std::string objFull = obj->GetFullName();
            const std::string fnLower = ToLowerCopy(fnFull);
            const std::string objLower = ToLowerCopy(objFull);

            if (!TracePassesFilter(fnLower, objLower))
                return;

            const uint32_t index = gTraceWriteIndex.fetch_add(1, std::memory_order_relaxed);
            gTraceRing[index % kTraceRingSize] = TraceEvent{obj, fn, GetCurrentThreadId()};

            {
                std::lock_guard<std::mutex> lock(gTraceMutex);
                ++gTraceFnCounts[fn];
            }

            // Persist full event data (crash survivable) in a dedicated trace file.
            {
                // Keep the JSON line small and grep-friendly.
                std::ostringstream oss;
                oss << "{\"type\":\"ProcessEvent\",\"ts\":\"" << TraceFileWriter::CurrentTimeString() << "\""
                    << ",\"seq\":" << index
                    << ",\"tid\":" << GetCurrentThreadId()
                    << ",\"objPtr\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(obj) << std::dec << "\""
                    << ",\"fnPtr\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(fn) << std::dec << "\""
                    << ",\"obj\":\"" << TraceFileWriter::JsonEscape(objFull) << "\""
                    << ",\"fn\":\"" << TraceFileWriter::JsonEscape(fnFull) << "\"";
                oss << "}";

                std::lock_guard<std::mutex> lock(gTraceFile.Mutex);
                gTraceFile.EnsureOpenLocked(false);
                gTraceFile.AppendLineLocked(oss.str());
            }
        }

        static void TraceReset()
        {
            gTraceWriteIndex.store(0, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(gTraceMutex);
            gTraceFnCounts.clear();
        }

        static std::string TraceDumpSummary(int topN, int lastN)
        {
            if (topN <= 0) topN = 30;
            if (lastN <= 0) lastN = 50;

            std::vector<std::pair<SDK::UFunction*, uint32_t>> counts;
            {
                std::lock_guard<std::mutex> lock(gTraceMutex);
                counts.reserve(gTraceFnCounts.size());
                for (auto& kv : gTraceFnCounts) counts.push_back(kv);
            }

            std::sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.second > b.second; });

            std::ostringstream oss;
            oss << "TraceEnabled=" << (gTraceEnabled.load() ? "true" : "false")
                << " RingWrite=" << gTraceWriteIndex.load() << "\n";

            {
                std::lock_guard<std::mutex> lock(gTraceMutex);
                oss << "Filter='" << gTraceFilterLower << "'\n";
            }

            oss << "Top functions:\n";
            const int limit = std::min<int>(topN, (int)counts.size());
            for (int i = 0; i < limit; ++i)
            {
                SDK::UFunction* fn = counts[i].first;
                const uint32_t c = counts[i].second;
                oss << "  [" << c << "] " << (fn ? fn->GetFullName() : std::string("<null>")) << "\n";
            }

            oss << "Last events:\n";
            const uint32_t write = gTraceWriteIndex.load(std::memory_order_relaxed);
            const uint32_t available = std::min<uint32_t>(write, kTraceRingSize);
            const uint32_t take = std::min<uint32_t>((uint32_t)lastN, available);
            for (uint32_t i = 0; i < take; ++i)
            {
                const uint32_t idx = (write - 1 - i) % kTraceRingSize;
                const TraceEvent& e = gTraceRing[idx];
                const std::string objName = e.Object ? e.Object->GetFullName() : std::string("<null>");
                const std::string fnName = e.Function ? e.Function->GetFullName() : std::string("<null>");
                oss << "  T" << e.ThreadId << " " << objName << " :: " << fnName << "\n";
            }
            return oss.str();
        }

        // ProcessEvent reentry guard used to bypass hooks when we intentionally call SDK/Blueprint functions.
        // IMPORTANT: do NOT keep this enabled while calling originalFn for a gameplay event (e.g. ServerShootProjectile),
        // otherwise nested ProcessEvent calls (e.g. TryExtractNextItem) won't be intercepted.
        static thread_local bool gProcessEventReentryGuard = false;

        struct ScopedShotContext
        {
            SDK::URadiusFirearmComponent *prevComponent;
            SDK::AActor *prevOwner;
            ScopedShotContext(SDK::URadiusFirearmComponent *component, SDK::AActor *owner)
                : prevComponent(gActiveShotComponent), prevOwner(gActiveShotOwner)
            {
                gActiveShotComponent = component;
                gActiveShotOwner = owner;
            }
            ~ScopedShotContext()
            {
                gActiveShotComponent = prevComponent;
                gActiveShotOwner = prevOwner;
            }
        };

        static SDK::AActor *TryGetOwningActor(SDK::UObject *object)
        {
            if (!object)
                return nullptr;

            if (object->IsA(SDK::AActor::StaticClass()))
            {
                return static_cast<SDK::AActor *>(object);
            }

            if (object->IsA(SDK::UActorComponent::StaticClass()))
            {
                auto *component = static_cast<SDK::UActorComponent *>(object);
                return component ? component->GetOwner() : nullptr;
            }

            return nullptr;
        }

        static bool IsHeldRadiusItemActor(const SDK::AActor *actor)
        {
            if (!actor)
                return false;

            // Approach 1: ARadiusItemBase grip controller pointers.
            if (actor->IsA(SDK::ARadiusItemBase::StaticClass()))
            {
                const auto *item = static_cast<const SDK::ARadiusItemBase *>(actor);
                // Approach 1a: ask the controller if it considers the actor held.
                // This avoids false positives where a controller pointer is cached but the actor is no longer in-hand.
                if (item->GripControllerPrimary && item->GripControllerPrimary->GetIsHeld(actor))
                    return true;
                if (item->GripControllerSecondary && item->GripControllerSecondary->GetIsHeld(actor))
                    return true;
            }

            return false;
        }

        static bool IsHeldContext(SDK::UObject *object)
        {
            // If we are inside a firing chain for a held weapon, allow the nested hooks.
            if (gActiveShotComponent || gActiveShotOwner)
                return true;

            SDK::AActor *owner = TryGetOwningActor(object);
            return IsHeldRadiusItemActor(owner);
        }

        struct ServerUpdateWithAmmoParams
        {
            SDK::FFirearmStateRep Rep;
            SDK::FFirearmAmmoStateRep AmmoRep;
        };

        struct ServerShootProjectileParams
        {
            SDK::FFirearmComponentShotExtendedRep Rep;
        };

        struct ServerInsertAmmoParams
        {
            SDK::AActor *OtherActor;
            SDK::FFirearmAmmoStateRep Rep;
        };

        struct BoolReturnParam
        {
            bool ReturnValue;
        };

        struct IntReturnParam
        {
            int32_t ReturnValue;
        };

        struct TryExtractNextItemParams
        {
            SDK::FGameplayTag ExtractedItemTag;
            bool ReturnValue;
        };

        struct GetCurrentAmmoNumberParams
        {
            int32_t Number;
            bool ReturnValue;
        };

        struct SetSliderLockParams
        {
            bool bSet;
            bool bForce;
            bool ReturnValue;
        };

        struct OnSliderLockParams
        {
            bool bSliderLock;
        };

        struct UpdateBulletAmmoParams
        {
            SDK::FGameplayTag AmmoTypeID;
            bool bIsShell_0;
        };

        struct MuzzleShootProjectileParams
        {
            SDK::FGameplayTag GameplayTag;
            bool ReturnValue;
        };

        struct ChangeStatsParams
        {
            float Delta;
        };

        struct SetHolsteredActorParams
        {
            SDK::ARadiusGrippableActorBase* ActorToHolster;
        };

        struct PutItemToContainerParams
        {
            SDK::AActor* ItemActor;                    // 0x00
            SDK::FTransform RelativeTransform;         // 0x08 (0x60 bytes)
            bool ReturnValue;                          // 0x68
        };

        // RequestAttackRole parameters (coordination subsystem).  We only care
        // about the return value, but matching layout prevents stack corruption.
        struct RequestAttackRoleParams
        {
            SDK::ARadiusAIControllerBase* AIController;
            SDK::TDelegate<void()> AttackRoleDelegate;
            bool ReturnValue;
        };

        void ForceResyncFirearm(SDK::URadiusFirearmComponent *component)
        {
            if (!component)
            {
                return;
            }

            //LOG_INFO("[HookManager][ForceResyncFirearm] Starting resync on component=" << component);
            
            //LOG_INFO("[HookManager][ForceResyncFirearm] Calling InitializeMagazine");
            component->InitializeMagazine();
            
            //LOG_INFO("[HookManager][ForceResyncFirearm] Calling DeliverAmmoFromMagToChamber");
            component->DeliverAmmoFromMagToChamber();
            
            //LOG_INFO("[HookManager][ForceResyncFirearm] Calling SetWeaponCocked(true)");
            component->SetWeaponCocked(true);
            
            //LOG_INFO("[HookManager][ForceResyncFirearm] Calling SetSliderLock(false, true)");
            component->SetSliderLock(false, true);
            
            //LOG_INFO("[HookManager][ForceResyncFirearm] Resync complete");
        }

        bool Hook_TryExtractNextItem(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)function;
            (void)originalFn;

            if (!parms)
            {
                return false;
            }
            // if unlimited ammo is disabled follow normal flow
            if (!HookManager::Get().IsUnlimitedAmmoEnabled())
            {
                // call original function to maintain normal behavior
                originalFn(object, function, parms);
                return true; // Indicate that we've handled the call
            }



            auto *extractParams = reinterpret_cast<TryExtractNextItemParams *>(parms);
            if (!gHasLastObservedAmmoTag)
            {
                static int noTagLogs = 0;
                if (noTagLogs < 10)
                {
                    noTagLogs++;
                }
                return false;
            }

            extractParams->ReturnValue = true;
            extractParams->ExtractedItemTag = gLastObservedAmmoTag;
            return true;

            
        }

        bool Hook_ServerShootProjectile(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)function;
            if (!object || !parms || !originalFn)
            {
                return false;
            }

            auto *component = static_cast<SDK::URadiusFirearmComponent *>(object);
            auto *params = reinterpret_cast<ServerShootProjectileParams *>(parms);
            // only apply cheat if unlimited ammo is enabled, otherwise follow normal flow
            if (!HookManager::Get().IsUnlimitedAmmoEnabled())
            {
                originalFn(object, function, parms);
                return true; // Indicate that we've handled the call
            }

            // Only apply cheat to held weapons.
            SDK::AActor *owner = TryGetOwningActor(component);
            if (!IsHeldRadiusItemActor(owner))
            {
                static int notHeldLogs = 0;
                if (notHeldLogs < 20)
                {
                    notHeldLogs++;
                    const std::string ownerName = owner ? owner->GetName() : std::string("<null>");
                }
                return false;
            }

            // Capture ammo tag for later use in TryExtractNextItem
            gLastObservedAmmoTag = params->Rep.AmmoTag;
            gHasLastObservedAmmoTag = true;

            // Notify Arena of shot
            auto* arena = Arena::ArenaSubsystem::Get();
            if (arena && arena->IsActive())
            {
                arena->RecordBulletFired();
            }

            const int32_t magLeft = params->Rep.MagazineAmmoLeft;
            const int32_t ammoInBarrelCount = UnsafeTArrayNum(params->Rep.AmmoInBarrel);

      /*      LOG_INFO("[HookManager][ServerShootProjectile] Calling originalFn (MagLeft=" << magLeft
                                                                                         << ", AmmoInBarrel=" << ammoInBarrelCount << ")");*/
            {
                // Scope nested hooks to this held weapon during the original firing call chain.
                ScopedShotContext ctx(component, owner);
                originalFn(object, function, parms);
            }

            // After the server-shot RPC returns, resync only when it makes sense.
            // Requirement: only resync if there is > 1 bullet in magazine AND a bullet in the chamber/barrel.
            const bool shouldResync = (magLeft > 1) && (ammoInBarrelCount > 0);
            if (!shouldResync)
            {
                static int skipResyncLogs = 0;
                
                return true;
            }

            {
                // Guard only the SDK calls so nested ProcessEvent during originalFn still gets intercepted.
                ScopedProcessEventGuard guard;
                ForceResyncFirearm(component);
            }
            return true;
        }

        bool Hook_DurabilityNoOp(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)parms;
            (void)originalFn;

            // Allow toggling durability bypass independently of unlimited ammo.
            if (!HookManager::Get().IsDurabilityBypassEnabled())
            {
                return false;
            }

            // Only bypass durability for held weapons.
            if (!IsHeldContext(object))
            {
                static int notHeldLogs = 0;
                if (notHeldLogs < 10)
                {
                    notHeldLogs++;
                    const std::string fn = function ? function->GetName() : std::string("<null>");
                    LOG_INFO("[HookManager][DurabilityHook] Allow original (not held): " << fn);
                }
                return false;
            }

            // POC: just skip all durability change paths when enabled.
            // These are invoked on firearm actors (e.g. BP_PM_C) during firing.
            static int logs = 0;
            if (logs < 20)
            {
                logs++;
                const std::string cls = (object && object->Class) ? object->Class->GetName() : std::string("<null>");
                const std::string fn = function ? function->GetName() : std::string("<null>");
                LOG_INFO("[HookManager][DurabilityHook] Skip " << fn << " on " << cls);
            }
            return true;
        }

        bool Hook_ChangeHunger(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)object;
            (void)function;
            (void)originalFn;

            if (!parms || !HookManager::Get().IsHungerDisabled())
            {
                return false;
            }

            auto *params = reinterpret_cast<ChangeStatsParams *>(parms);
            if (params->Delta < 0.0f)
            {
                Mod::ModFeedback::ShowMessage(L"Hunger/Fatigue Bypass Active", 2.0f, {1.0f, 0.5f, 0.0f, 1.0f});
                return true; // Skip negative change
            }
            return false;
        }

        bool Hook_ChangeStamina(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)object;
            (void)function;
            (void)originalFn;

            if (!parms || !HookManager::Get().IsFatigueDisabled())
            {
                return false;
            }

            auto *params = reinterpret_cast<ChangeStatsParams *>(parms);
            if (params->Delta < 0.0f)
            {
                Mod::ModFeedback::ShowMessage(L"Hunger/Fatigue Bypass Active", 2.0f, {1.0f, 0.5f, 0.0f, 1.0f});
                return true; // Skip negative change
            }
            return false;
        }

        // Hook to force every RequestAttackRole call to succeed; useful for
        // observing how NPCs behave when they always think they can be aggressive.
        bool Hook_RequestAttackRole(SDK::UObject *object, SDK::UFunction *function, void *parms, HookManager::ProcessEventFn originalFn)
        {
            (void)object;
            (void)function;
            if (!parms || !originalFn)
            {
                return false;
            }

            auto *p = reinterpret_cast<RequestAttackRoleParams*>(parms);
            // let the normal logic run in case it queues delegates etc.
            originalFn(object, function, parms);
            p->ReturnValue = true;

            static int logs = 0;
            if (logs < 20)
            {
                logs++;
                LOG_INFO("[HookManager][RequestAttackRole] forced true");
            }
            return true;
        }

        // Diagnostics: capture what the game uses for VR-visible notifications.
        // These are gated behind trace mode so they don't add noise/perf cost during normal play.
        struct Player_ShowSubtitles_Params
        {
            SDK::ESubtitleInstigator SubtitleInstigator;
            uint8_t Pad_1[0x7];
            SDK::FText Message;
            float Duration;
        };

        struct FLGeneral_ShowSubtitles_Params
        {
            const SDK::UObject* WorldContextObject;
            SDK::ESubtitleInstigator SubtitleInstigator;
            uint8_t Pad_9[0x7];
            SDK::FText Message;
            float Duration;
            uint8_t Pad_24[0x4];
        };

        struct FLGeneral_ShowMessage_Params
        {
            SDK::FString Message;
            SDK::FString Title;
        };

        bool Hook_LogShowSubtitles(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)originalFn;
            const bool doDiag = gTraceEnabled.load(std::memory_order_relaxed);
            const bool doBridge = gNotifBridgeEnabled.load(std::memory_order_relaxed);
            if (!doDiag && !doBridge)
                return false;

            if (!object || !function || !parms)
                return false;

            static int diagLogs = 0;
            const bool allowDiagLog = doDiag && (diagLogs < 200);

            const std::string fnFull = function->GetFullName();
            SDK::FText msgText{};
            SDK::ESubtitleInstigator inst = SDK::ESubtitleInstigator::System;
            float duration = 0.0f;


            if (fnFull.find("FLGeneral.ShowSubtitles") != std::string::npos)
            {
                auto* p = reinterpret_cast<FLGeneral_ShowSubtitles_Params*>(parms);
                msgText = p->Message;
                inst = p->SubtitleInstigator;
                duration = p->Duration;
            }
            else
            {
                auto* p = reinterpret_cast<Player_ShowSubtitles_Params*>(parms);
                msgText = p->Message;
                inst = p->SubtitleInstigator;
                duration = p->Duration;
            }

            SDK::FString msgStr;
            {
                ScopedProcessEventGuard guard;
                msgStr = SDK::UKismetTextLibrary::Conv_TextToString(msgText);
            }

            const std::string msgStd = msgStr.ToString();

            if (allowDiagLog)
            {
                ++diagLogs;
                LOG_INFO("[NotifDiag] ShowSubtitles inst=" << (int)inst << " dur=" << duration
                                                          << " obj=" << object->GetFullName()
                                                          << " fn=" << fnFull
                                                          << " text='" << msgStd << "'");
            }

            // CRUFT (should be removed): NotifBridge mirrors subtitles into FLGeneral popups.
            // It was useful during investigation but adds noise/confusion now that we're
            // standardizing on PlayerCharacter->ShowSubtitles as the sole mod feedback path.
            if (doBridge)
            {
                // Avoid reflecting our own diagnostic tags back into popups.
                const bool looksLikeModTagged = (msgStd.find("[PlayerSub:") != std::string::npos)
                    || (msgStd.find("[SubActor]") != std::string::npos)
                    || (msgStd.find("[FLGeneral:") != std::string::npos)
                    || (msgStd.find("[Mod]") != std::string::npos);

                static uint64_t lastTickMs = 0;
                static std::string lastText;
                const uint64_t now = TraceFileWriter::NowTickMs();

                // Rate-limit: allow repeats if text changes, or every 750ms.
                const bool shouldEmit = (!looksLikeModTagged)
                    && (!msgStd.empty())
                    && ((msgStd != lastText) || (lastTickMs == 0) || (now - lastTickMs >= 750));

                if (shouldEmit)
                {
                    lastTickMs = now;
                    lastText = msgStd;

                    // CRUFT (should be removed): popup emission path.
                    ScopedProcessEventGuard guard;
                    SDK::FString popupMsg = MakeStableFString(WidenLossy(msgStd));
                    SDK::UFLGeneral::ShowMessage(popupMsg, SDK::FString(L"Game"));

                    // Decisive proof in the log (VR testing is expensive): show exactly what we mirrored.
                    // Keep it rate-limited by the same gating as the popup emission.
                    LOG_INFO("[NotifBridge] MIRROR inst=" << (int)inst << " dur=" << duration
                                                        << " srcObj=" << object->GetFullName()
                                                        << " srcFn=" << fnFull
                                                        << " text='" << msgStd << "'");

                    if (gNotifBridgePlaySound.load(std::memory_order_relaxed))
                    {
                        if (SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player = Mod::GameContext::GetPlayerCharacter())
                        {
                            if (player->RadioStartSound)
                                Mod::ModFeedback::PlaySound2D(player->RadioStartSound, 0.9f, 1.35f, true);
                        }
                    }
                }
            }
            return false;
        }

        bool Hook_LogShowMessage(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)originalFn;
            // CRUFT (should be removed): legacy diagnostics for FLGeneral popup messages.
            // Now that mod feedback is standardized on PlayerCharacter->ShowSubtitles, this is
            // mostly historical and contributes to notification-related noise.
            if (!gTraceEnabled.load(std::memory_order_relaxed))
                return false;

            if (!object || !function || !parms)
                return false;

            const std::string fnFull = function->GetFullName();
            if (fnFull.find("FLGeneral.ShowMessage") == std::string::npos)
                return false;

            static int logs = 0;
            if (logs >= 200)
                return false;

            auto* p = reinterpret_cast<FLGeneral_ShowMessage_Params*>(parms);
            ++logs;
            LOG_INFO("[NotifDiag] ShowMessage obj=" << object->GetFullName()
                                                    << " fn=" << fnFull
                                                    << " title='" << p->Title.ToString() << "'"
                                                    << " msg='" << p->Message.ToString() << "'");
            return false;
        }

        // Forward declarations for AutoMag (defined further below after helpers).
        static void AutoMag_ProcessHolster(SDK::UObject* object, SDK::ARadiusGrippableActorBase* actorToHolster);
        static void AutoMag_ProcessContainer(SDK::UObject* object, SDK::AActor* itemActor);

        // Tablet discovery shared state (implemented below; declared here so hooks can call it).
        static std::mutex& TabletMutexRef();
        static std::string& LastHolsteredActorRef();
        static std::string& LastHolsteredActorClassRef();

        // Tablet discovery: log what actor is being holstered into the AutoReturnHolster slot.
        // This is intended to identify the actual tablet actor class/widget chain for later UI integration.
        bool Hook_SetHolsteredActor(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)function;
            (void)originalFn;

            // DIAGNOSTIC: Log every call to see if this hook fires at all
            static int callCount = 0;
            if (callCount < 10)
            {
                ++callCount;
                LOG_INFO("[AutoMag][DIAG] Hook_SetHolsteredActor called #" << callCount
                         << " object=" << (object ? object->GetName() : "<null>")
                         << " class=" << (object && object->Class ? object->Class->GetName() : "<null-class>"));
            }

            if (!object || !parms)
                return false;

            auto* params = reinterpret_cast<SetHolsteredActorParams*>(parms);

            // ---- AutoMag: refill magazines placed in body pouches ----
            // SetHolsteredActor fires on URadiusHolsterComponent for ALL holster
            // types including body pouches (native holsters without BPC_ layer),
            // weapon magazine slots (BPC_RadiusMagazineSlot_C), hand holsters, etc.
            // AutoMag_ProcessHolster handles classification and filtering.
            if (HookManager::Get().IsAutoMagEnabled())
            {
                if (!params->ActorToHolster)
                {
                    static int nullActorLogs = 0;
                    if (nullActorLogs < 5)
                    {
                        ++nullActorLogs;
                        LOG_INFO("[AutoMag][DIAG] SetHolsteredActor: ActorToHolster is NULL");
                    }
                }
                else
                {
                    try
                    {
                        AutoMag_ProcessHolster(object, params->ActorToHolster);
                    }
                    catch (...)
                    {
                        LOG_ERROR("[AutoMag] EXCEPTION in AutoMag_ProcessHolster - prevented crash");
                    }
                }
            }
            else
            {
                static int disabledLogs = 0;
                if (disabledLogs < 3)
                {
                    ++disabledLogs;
                    LOG_INFO("[AutoMag][DIAG] SetHolsteredActor: AutoMag is DISABLED");
                }
            }

            // ---- Existing tablet diagnostic logic ----
            if (!object->IsA(SDK::UBPC_AutoReturnHolster_C::StaticClass()))
                return false;

            auto* holster = static_cast<SDK::UBPC_AutoReturnHolster_C*>(object);

            // Persist the most recent holstered actor so TCP commands can query it.
            if (params->ActorToHolster)
            {
                const std::string actorFull = params->ActorToHolster->GetFullName();
                const std::string classFull = std::string("<unavailable>");
                {
                    std::lock_guard<std::mutex> lock(TabletMutexRef());
                    LastHolsteredActorRef() = actorFull;
                    LastHolsteredActorClassRef() = classFull;
                }
            }

            static int logs = 0;
            if (logs < 50)
            {
                ++logs;
                const std::string ownerName = holster->GetOwner() ? holster->GetOwner()->GetFullName() : std::string("<null>");
                const std::string actorName = params->ActorToHolster ? params->ActorToHolster->GetFullName() : std::string("<null>");
                const std::string unloosableName = holster->UnloosableActor ? holster->UnloosableActor->GetFullName() : std::string("<null>");
                LOG_INFO("[HookManager][TabletDiag] SetHolsteredActor: HolsterOwner=" << ownerName
                                                                         << " ActorToHolster=" << actorName
                                                                         << " UnloosableActor=" << unloosableName);
            }

            return false; // Never override; diagnostic only.
        }

        // Hook: PutItemToContainer (IItemContainerInterface)
        // This is what BODY POUCHES actually use - the container system, not SetHolsteredActor.
        bool Hook_PutItemToContainer(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)function;

            // DIAGNOSTIC: Log every call
            static int callCount = 0;
            if (callCount < 10)
            {
                ++callCount;
                LOG_INFO("[AutoMag][DIAG] Hook_PutItemToContainer called #" << callCount
                         << " object=" << (object ? object->GetName() : "<null>")
                         << " class=" << (object && object->Class ? object->Class->GetName() : "<null-class>"));
            }

            if (!object || !parms || !originalFn)
                return false;

            auto* params = reinterpret_cast<PutItemToContainerParams*>(parms);

            // Modify magazine BEFORE calling original, so container reads the filled state
            if (HookManager::Get().IsAutoMagEnabled() && params->ItemActor)
            {
                try
                {
                    AutoMag_ProcessContainer(object, params->ItemActor);
                }
                catch (...)
                {
                    LOG_ERROR("[AutoMag] EXCEPTION in AutoMag_ProcessContainer - prevented crash");
                }
            }

            // NOW call original to place the modified magazine
            originalFn(object, function, parms);

            return true; // Suppress default invocation (we already called original)
        }

        // DIAGNOSTIC: Monitor ALL ProcessEvent calls on magazine and container actors
        static void AutoMag_DiagnosticMonitor(SDK::UObject* object, SDK::UFunction* function)
        {
            if (!HookManager::Get().IsAutoMagEnabled())
                return;

            static int logCount = 0;
            if (logCount >= 50) // Limit to avoid spam
                return;

            if (!object || !function)
                return;

            const std::string objName = object->GetName();
            const std::string className = object->Class ? object->Class->GetName() : "";
            const std::string funcName = function->GetName();

            // Log if this is a magazine actor or container-related object
            if (objName.find("Magazine") != std::string::npos ||
                objName.find("MagPouch") != std::string::npos ||
                className.find("Magazine") != std::string::npos ||
                className.find("Container") != std::string::npos ||
                className.find("Holster") != std::string::npos)
            {
                ++logCount;
                LOG_INFO("[AutoMag][DIAG][ProcessEvent] obj=" << objName 
                         << " class=" << className 
                         << " func=" << funcName);
            }
        }

        static void TabletInteraction_Record(const std::string& summary)
        {
            std::lock_guard<std::mutex> lock(gTabletInteractionMutex);
            gLastTabletInteraction = summary;
        }

        // Tablet interactions: grip and UI delegates.
        bool Hook_Tablet_OnGrip(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)parms;
            (void)originalFn;

            if (!gTabletInteractionDiagEnabled.load(std::memory_order_relaxed))
                return false;
            if (!object || !function)
                return false;

            const std::string objFull = object->GetFullName();
            const std::string fnFull = function->GetFullName();

            // Strong scoping to avoid logging every grippable object.
            const bool isTablet = (objFull.find("BP_Tablet_C") != std::string::npos) || (fnFull.find("BP_Tablet_C") != std::string::npos);
            if (!isTablet)
                return false;

            static int logs = 0;
            if (logs < 300)
            {
                ++logs;
                const std::string outer = (object->Outer ? object->Outer->GetFullName() : std::string("<null>"));
                const std::string s = std::string("[TabletDiag] OnGrip obj=") + objFull + " outer=" + outer + " fn=" + fnFull;
                LOG_INFO(s);
                TabletInteraction_Record(s);
            }
            return false;
        }

        bool Hook_Tablet_OnGripRelease(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)parms;
            (void)originalFn;

            if (!gTabletInteractionDiagEnabled.load(std::memory_order_relaxed))
                return false;
            if (!object || !function)
                return false;

            const std::string objFull = object->GetFullName();
            const std::string fnFull = function->GetFullName();
            const bool isTablet = (objFull.find("BP_Tablet_C") != std::string::npos) || (fnFull.find("BP_Tablet_C") != std::string::npos);
            if (!isTablet)
                return false;

            static int logs = 0;
            if (logs < 300)
            {
                ++logs;
                const std::string outer = (object->Outer ? object->Outer->GetFullName() : std::string("<null>"));
                const std::string s = std::string("[TabletDiag] OnGripRelease obj=") + objFull + " outer=" + outer + " fn=" + fnFull;
                LOG_INFO(s);
                TabletInteraction_Record(s);
            }
            return false;
        }

        bool Hook_Tablet_UiDelegate(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)parms;
            (void)originalFn;

            if (!gTabletInteractionDiagEnabled.load(std::memory_order_relaxed))
                return false;
            if (!object || !function)
                return false;

            const std::string objFull = object->GetFullName();
            const std::string fnFull = function->GetFullName();

            // Scope to tablet UI widgets only.
            const bool isTabletUi = (objFull.find("WBP_Tablet_UI") != std::string::npos)
                || (objFull.find("Tablet") != std::string::npos)
                || (fnFull.find("WBP_Tablet_UI") != std::string::npos);
            if (!isTabletUi)
                return false;

            static int logs = 0;
            if (logs < 400)
            {
                ++logs;
                const std::string outer = (object->Outer ? object->Outer->GetFullName() : std::string("<null>"));
                const std::string s = std::string("[TabletDiag] UiEvent obj=") + objFull + " outer=" + outer + " fn=" + fnFull;
                LOG_INFO(s);
                TabletInteraction_Record(s);
            }
            return false;
        }

        // Accessors for the function below (file-local by design).
        static std::mutex& TabletMutexRef()
        {
            static std::mutex m;
            return m;
        }

        static std::string& LastHolsteredActorRef()
        {
            static std::string s;
            return s;
        }

        static std::string& LastHolsteredActorClassRef()
        {
            static std::string s;
            return s;
        }

        // -----------------------------------------------------------------
        // AutoMag: when a magazine is placed in a mag pouch, refill ammo.
        // Hooks SetHolsteredActor on URadiusHolsterComponent, which fires for
        // ALL holster types (body pouches, weapon mag slots, hands, etc.).
        // Body pouch holsters are native URadiusHolsterComponent instances
        // without a BPC_ layer. OnItemHolsterAttachChanged_Event only fires
        // on BPC_ItemHolster_C subclasses, so it MISSES body pouches entirely.
        //
        // Param layout (from IntoTheRadius2_parameters.hpp):
        //   0x00 ARadiusGrippableActorBase* ActorToHolster
        //
        // Strategy:
        //  1. Check autoMag enabled + ActorToHolster non-null.
        //  2. Skip weapon mag slots (UBPC_RadiusMagazineSlot_C) - causes crash.
        //  3. Skip hand holsters (UBPC_PlayerCharacterHandHolster_C) - spam.
        //  4. Check item tag contains "Item.Magazine".
        //  5. Get URadiusItemDynamicData via DataComponent weak ptr.
        //  6. Determine ammo tag from existing stack or static data.
        //  7. Determine capacity from static data or known-capacity table.
        //  8. Fill StackedItems.Items TArray to capacity.
        //  9. Wrap ALL of this in try/catch - crashes are unacceptable.
        // -----------------------------------------------------------------

        // Helper: Get URadiusItemDynamicData from an ARadiusItemBase actor.
        // ARadiusItemBase::DataComponent is at offset 0x05F0.
        // URadiusDataComponent::ItemDynamicDataPtr (TWeakObjectPtr) is at offset 0x0430.
        static SDK::URadiusItemDynamicData* GetDynamicDataFromActor(SDK::AActor* actor)
        {
            if (!actor) return nullptr;

            // Check it's actually a RadiusItemBase (or derived).
            if (!actor->IsA(SDK::ARadiusItemBase::StaticClass()))
                return nullptr;

            auto* itemBase = static_cast<SDK::ARadiusItemBase*>(actor);
            SDK::URadiusDataComponent* dataComp = itemBase->DataComponent;
            if (!dataComp) return nullptr;

            // ItemDynamicDataPtr is a TWeakObjectPtr<URadiusItemDynamicData> at 0x0430.
            // TWeakObjectPtr stores {int32 ObjectIndex, int32 ObjectSerialNumber}.
            // Resolve via GObjects.GetByIndex().
            struct WeakObjPtrLayout { int32_t ObjectIndex; int32_t ObjectSerialNumber; };
            auto* raw = reinterpret_cast<const WeakObjPtrLayout*>(&dataComp->ItemDynamicDataPtr);

            if (raw->ObjectIndex < 0) return nullptr;

            SDK::UObject* resolved = SDK::UObject::GObjects->GetByIndex(raw->ObjectIndex);
            if (!resolved) return nullptr;

            return static_cast<SDK::URadiusItemDynamicData*>(resolved);
        }

        // Known magazine capacities by tag substring (fallback when GetAmmoContainerStaticData fails).
        // Ordered most-specific first. Checked via itemTag.find(key).
        static const std::pair<const char*, int32_t> kKnownMagCapacities[] = {
            { "PM.Short",             8  },
            { "PM.Long",              12 },
            { "Mosin.ArchangelMag10", 10 },
            { "Mosin.ArchangelMag5",  5  },
            { "AK-74",                30 },
            { "AK-103",               30 },
            { "G36",                  30 },
            { "SR25",                 20 },
            { "Saiga",                8  },
            { "SVD",                  10 },
            { "VPO",                  10 },
            { "VSS",                  20 },
            { "PPSH",                 35 },
            { "MP5",                  30 },
            { "Glock",                17 },
            { "TT",                   8  },
        };

        // Lookup capacity from the known-capacity table by matching against the item type tag.
        static int32_t AutoMag_LookupKnownCapacity(const std::string& itemTag)
        {
            for (const auto& [key, cap] : kKnownMagCapacities)
            {
                if (itemTag.find(key) != std::string::npos)
                    return cap;
            }
            return 0; // Unknown
        }

        // Helper: fill a magazine's StackedItems directly, then trigger visual update
        static bool AutoMag_FillMagazine(SDK::AActor* magazineActor, int32_t capacity, SDK::FGameplayTag ammoTag)
        {
            if (!magazineActor || capacity <= 0) 
            {
                LOG_ERROR("[AutoMag][Fill] Invalid params: actor=" << (void*)magazineActor << " capacity=" << capacity);
                return false;
            }

            SDK::URadiusItemDynamicData* dynData = GetDynamicDataFromActor(magazineActor);
            if (!dynData)
            {
                LOG_ERROR("[AutoMag][Fill] No DynamicData on magazine");
                return false;
            }

            // StackedItems at offset 0x0150
            struct TArrayLayout
            {
                SDK::FStackedItem* Data;
                int32_t NumElements;
                int32_t MaxElements;
            };

            uint8_t* dynBase = reinterpret_cast<uint8_t*>(dynData);
            TArrayLayout* stackedArr = reinterpret_cast<TArrayLayout*>(dynBase + 0x0150);

            const int32_t currentCount = stackedArr->NumElements;
            const int32_t maxElements = stackedArr->MaxElements;
            
            LOG_INFO("[AutoMag][Fill] Current=" << currentCount 
                     << " Max=" << maxElements
                     << " Target=" << capacity);
            
            if (currentCount >= capacity)
            {
                LOG_INFO("[AutoMag][Fill] Already at capacity");
                return false;
            }

            // Don't reallocate - just fill what we have space for
            int32_t fillTo = (maxElements < capacity) ? maxElements : capacity;
            
            if (!stackedArr->Data && fillTo > 0)
            {
                LOG_ERROR("[AutoMag][Fill] Data pointer is NULL but need to fill");
                return false;
            }

            // Fill from current to fillTo
            for (int32_t i = currentCount; i < fillTo; ++i)
            {
                stackedArr->Data[i].ItemTag = ammoTag;
                stackedArr->Data[i].bIsShell = false;
            }
            stackedArr->NumElements = fillTo;

            LOG_INFO("[AutoMag][Fill] Filled from " << currentCount << " to " << fillTo);

            // Get stack component and trigger visual update
            SDK::URadiusItemStackComponent* stackComp = nullptr;
            {
                ScopedProcessEventGuard guard;
                stackComp = static_cast<SDK::URadiusItemStackComponent*>(
                    magazineActor->GetComponentByClass(SDK::URadiusItemStackComponent::StaticClass()));
            }

            if (stackComp)
            {
                // FireOnStackChanged triggers the visual update
                try
                {
                    ScopedProcessEventGuard guard;
                    stackComp->FireOnStackChanged();
                    LOG_INFO("[AutoMag][Fill] Triggered visual update via FireOnStackChanged");
                }
                catch (...)
                {
                    LOG_WARN("[AutoMag][Fill] Exception calling FireOnStackChanged");
                }
            }
            else
            {
                LOG_WARN("[AutoMag][Fill] No stack component found, visual update may not occur");
            }

            return true;
        }

        // Core automag processing. Called from Hook_SetHolsteredActor.
        // object = URadiusHolsterComponent* (the holster the item is being placed into)
        // actorToHolster = the item actor being placed
        static void AutoMag_ProcessHolster(SDK::UObject* object, SDK::ARadiusGrippableActorBase* actorToHolster)
        {
            // ---- Classify holster: skip weapon mag slots and hand holsters ----
            // Weapon mag slots (BPC_RadiusMagazineSlot_C) caused crashes previously.
            // Hand holsters would trigger on every pickup which is noisy.
            const std::string holsterClass = object->Class ? object->Class->GetName() : "<null-class>";
            const std::string holsterName  = object->GetName();

            // Get the owner actor of this holster component for context.
            SDK::AActor* holsterOwner = nullptr;
            if (object->IsA(SDK::UActorComponent::StaticClass()))
                holsterOwner = static_cast<SDK::UActorComponent*>(object)->GetOwner();
            const std::string ownerName = holsterOwner ? holsterOwner->GetName() : "<null-owner>";

            // SKIP: weapon magazine slots - inserting mag into gun. Known crash source.
            if (object->IsA(SDK::UBPC_RadiusMagazineSlot_C::StaticClass()))
            {
                LOG_INFO("[AutoMag] SKIP weapon mag slot: " << holsterClass << " on " << ownerName);
                return;
            }
            // SKIP: hand holsters - picking up a magazine.
            if (object->IsA(SDK::UBPC_PlayerCharacterHandHolster_C::StaticClass()))
                return;
            // SKIP: mouth slot.
            if (object->IsA(SDK::UBPC_MouthSlot_C::StaticClass()))
                return;

            // ---- Get item dynamic data ----
            auto* itemActor = static_cast<SDK::AActor*>(actorToHolster);
            SDK::URadiusItemDynamicData* dynData = GetDynamicDataFromActor(itemActor);
            if (!dynData)
            {
                static int failLogs = 0;
                if (failLogs < 30)
                {
                    ++failLogs;
                    LOG_INFO("[AutoMag] No dynamic data for actor: " << itemActor->GetName()
                             << " in holster: " << holsterClass << " (" << holsterName << ") on " << ownerName);
                }
                return;
            }

            const std::string itemTag = dynData->ItemType.TagName.ToString();

            // ---- Item must be a magazine ----
            if (itemTag.find("Item.Magazine") == std::string::npos)
                return;  // Not a magazine, silently skip.

            LOG_INFO("[AutoMag] Magazine " << itemTag << " placed in holster: "
                     << holsterClass << " (" << holsterName << ") on owner: " << ownerName);

            // ---- Get existing ammo tag from current stacked items ----
            SDK::FGameplayTag ammoTag{};
            bool hasAmmoTag = false;
            {
                // Read StackedItems directly from raw offset (avoids ProcessEvent).
                struct TArrayLayout {
                    SDK::FStackedItem* Data;
                    int32_t NumElements;
                    int32_t MaxElements;
                };
                uint8_t* dynBase = reinterpret_cast<uint8_t*>(dynData);
                auto* stackedArr = reinterpret_cast<const TArrayLayout*>(dynBase + 0x0150);
                if (stackedArr->Data && stackedArr->NumElements > 0)
                {
                    ammoTag = stackedArr->Data[0].ItemTag;
                    hasAmmoTag = true;
                    LOG_INFO("[AutoMag] Existing ammo tag from stack (" << stackedArr->NumElements
                             << " rounds): " << ammoTag.TagName.ToString());
                }
                else
                {
                    LOG_INFO("[AutoMag] Magazine is empty (NumElements=" << stackedArr->NumElements << ")");
                }
            }

            // ---- Get magazine capacity ----
            int32_t capacity = 0;

            // Method 1: Try GetAmmoContainerStaticData via ProcessEvent on DataComponent.
            {
                ScopedProcessEventGuard guard;
                auto* itemBase = static_cast<SDK::ARadiusItemBase*>(itemActor);
                if (itemBase->DataComponent)
                {
                    SDK::UFunction* getAmmoFn = nullptr;
                    try
                    {
                        getAmmoFn = itemBase->DataComponent->Class->GetFunction(
                            "RadiusDataComponentInterface", "GetAmmoContainerStaticData");
                    }
                    catch (...) { getAmmoFn = nullptr; }

                    if (getAmmoFn)
                    {
                        // Params layout (0x01E8): { FAmmoContainerStaticData @0; bool ReturnValue @0x01E0; pad[7] }
                        struct GetAmmoContainerParams
                        {
                            SDK::FAmmoContainerStaticData OutData;  // 0x0000 (0x01E0)
                            bool ReturnValue;                       // 0x01E0 (0x0001)
                            uint8_t Pad[0x7];                       // 0x01E1 (0x0007)
                        };
                        GetAmmoContainerParams gp{};

                        try
                        {
                            auto flgs = getAmmoFn->FunctionFlags;
                            getAmmoFn->FunctionFlags |= 0x400; // FUNC_Native
                            itemBase->DataComponent->ProcessEvent(getAmmoFn, &gp);
                            getAmmoFn->FunctionFlags = flgs;
                        }
                        catch (...)
                        {
                            gp.ReturnValue = false;
                            LOG_WARN("[AutoMag] ProcessEvent GetAmmoContainerStaticData threw");
                        }

                        if (gp.ReturnValue)
                        {
                            capacity = gp.OutData.Parameters.HolderCapacity;
                            LOG_INFO("[AutoMag] StaticData: HolderCapacity=" << capacity
                                     << "  ChamberCapacity=" << gp.OutData.Parameters.ChamberCapacity);

                            if (!hasAmmoTag)
                            {
                                struct TArrayTagLayout { SDK::FGameplayTag* Data; int32_t Num; int32_t Max; };
                                auto* raw = reinterpret_cast<const TArrayTagLayout*>(&gp.OutData.Parameters.AcceptedAmmoTypes);
                                if (raw->Data && raw->Num > 0)
                                {
                                    ammoTag = raw->Data[0];
                                    hasAmmoTag = true;
                                    LOG_INFO("[AutoMag] Ammo tag from AcceptedAmmoTypes: " << ammoTag.TagName.ToString());
                                }
                            }
                        }
                        else
                        {
                            LOG_INFO("[AutoMag] GetAmmoContainerStaticData returned false for " << itemTag);
                        }
                    }
                    else
                    {
                        LOG_INFO("[AutoMag] GetAmmoContainerStaticData UFunction not found on DataComponent class: "
                                 << (itemBase->DataComponent->Class ? itemBase->DataComponent->Class->GetName() : "<null>"));
                    }
                }
            }

            // Method 2: Known-capacity lookup table (by magazine tag substring).
            if (capacity <= 0)
            {
                capacity = AutoMag_LookupKnownCapacity(itemTag);
                if (capacity > 0)
                {
                    LOG_INFO("[AutoMag] Capacity from known-mag table: " << capacity << " for " << itemTag);
                }
            }

            // Method 3: Try parsing a number from the end of the tag (e.g. "G36.30" -> 30).
            if (capacity <= 0)
            {
                size_t lastDot = itemTag.rfind('.');
                if (lastDot != std::string::npos)
                {
                    try { capacity = std::stoi(itemTag.substr(lastDot + 1)); }
                    catch (...) { capacity = 0; }
                }
                if (capacity > 0)
                    LOG_INFO("[AutoMag] Capacity parsed from tag suffix: " << capacity);
            }

            // Method 4: Ultimate fallback.
            if (capacity <= 0)
            {
                capacity = 30;
                LOG_WARN("[AutoMag] Using fallback capacity of " << capacity << " for " << itemTag);
            }

            if (!hasAmmoTag)
            {
                LOG_ERROR("[AutoMag] No ammo tag available for " << itemTag << " - cannot refill");
                return;
            }

            // ---- Do the actual refill ----
            bool refilled = AutoMag_FillMagazine(itemActor, capacity, ammoTag);

            if (refilled)
            {
                Mod::ModFeedback::ShowMessage(
                    L"[AutoMag] Magazine refilled!", 2.0f,
                    SDK::FLinearColor{0.2f, 1.0f, 0.6f, 1.0f});
            }
        }

        // AutoMag_ProcessContainer: handles magazines placed via the container system (body pouches).
        // object = the container object (UObject implementing IItemContainerInterface)
        // itemActor = the item being placed
        static void AutoMag_ProcessContainer(SDK::UObject* object, SDK::AActor* itemActor)
        {
            const std::string containerClass = object->Class ? object->Class->GetName() : "<null-class>";
            const std::string containerName  = object->GetName();

            // CRITICAL: Exclude weapon magazine slots to prevent crashes
            if (containerClass.find("BPC_RadiusMagazineSlot_C") != std::string::npos)
            {
                return;
            }

            // Get item dynamic data
            SDK::URadiusItemDynamicData* dynData = GetDynamicDataFromActor(itemActor);
            if (!dynData)
            {
                return;
            }

            const std::string itemTag = dynData->ItemType.TagName.ToString();

            // Item must be a magazine
            if (itemTag.find("Item.Magazine") == std::string::npos)
            {
                return;  // Not a magazine
            }

            // Check if container is a mag pouch
            // Use class-based check for inventory slots that hold magazines
            const bool isMagPouch = 
                (containerClass.find("BPC_InvSlot_") != std::string::npos && containerClass.find("Magazine") != std::string::npos) ||
                (containerName.find("MagPouch") != std::string::npos);

            if (!isMagPouch)
            {
                return;
            }

            // Get existing ammo tag from current stacked items
            SDK::FGameplayTag ammoTag{};
            bool hasAmmoTag = false;
            int32_t currentAmmo = 0;
            {
                struct TArrayLayout {
                    SDK::FStackedItem* Data;
                    int32_t NumElements;
                    int32_t MaxElements;
                };
                uint8_t* dynBase = reinterpret_cast<uint8_t*>(dynData);
                auto* stackedArr = reinterpret_cast<const TArrayLayout*>(dynBase + 0x0150);
                if (stackedArr->Data && stackedArr->NumElements > 0)
                {
                    ammoTag = stackedArr->Data[0].ItemTag;
                    hasAmmoTag = true;
                    currentAmmo = stackedArr->NumElements;
                }
                else
                {
                }
            }

            // Get capacity AND ammo tag from static data
            int32_t capacity = 0;
            {
                ScopedProcessEventGuard guard;
                auto* itemBase = static_cast<SDK::ARadiusItemBase*>(itemActor);
                if (itemBase->DataComponent)
                {
                    SDK::UFunction* getAmmoFn = nullptr;
                    try { getAmmoFn = itemBase->DataComponent->Class->GetFunction("RadiusDataComponentInterface", "GetAmmoContainerStaticData"); }
                    catch (...) { getAmmoFn = nullptr; }

                    if (getAmmoFn)
                    {
                        struct GetAmmoContainerParams { SDK::FAmmoContainerStaticData OutData; bool ReturnValue; uint8_t Pad[0x7]; };
                        GetAmmoContainerParams gp{};
                        try
                        {
                            auto flgs = getAmmoFn->FunctionFlags;
                            getAmmoFn->FunctionFlags |= 0x400;
                            itemBase->DataComponent->ProcessEvent(getAmmoFn, &gp);
                            getAmmoFn->FunctionFlags = flgs;
                        }
                        catch (...) { gp.ReturnValue = false; }

                        if (gp.ReturnValue)
                        {
                            capacity = gp.OutData.Parameters.HolderCapacity;
                            
                            // ALWAYS try to get ammo tag from AcceptedAmmoTypes (especially for empty mags)
                            struct TArrayTagLayout { SDK::FGameplayTag* Data; int32_t Num; int32_t Max; };
                            auto* raw = reinterpret_cast<const TArrayTagLayout*>(&gp.OutData.Parameters.AcceptedAmmoTypes);
                            if (raw->Data && raw->Num > 0)
                            {
                                if (!hasAmmoTag)
                                {
                                    ammoTag = raw->Data[0];
                                    hasAmmoTag = true;
                                }
                                else
                                {
                                }
                            }
                        }
                    }
                }
            }

            if (capacity <= 0)
            {
                capacity = AutoMag_LookupKnownCapacity(itemTag);
                // if (capacity > 0)
                //     LOG_INFO("[AutoMag][Container] Capacity from lookup table: " << capacity);
            }

            if (capacity <= 0)
            {
                size_t lastDot = itemTag.rfind('.');
                if (lastDot != std::string::npos)
                {
                    try { capacity = std::stoi(itemTag.substr(lastDot + 1)); }
                    catch (...) { capacity = 0; }
                }
                // if (capacity > 0)
                //     LOG_INFO("[AutoMag][Container] Capacity from tag suffix: " << capacity);
            }

            if (capacity <= 0)
            {
                capacity = 30;
            }

            // Fallback ammo tag mapping if static data lookup failed
            if (!hasAmmoTag)
            {
                
                // Common magazine -> ammo type mappings
                if (itemTag.find("PM.Short") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.9x18.FMJ")};
                else if (itemTag.find("FORT17") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.9x19.FMJ")};
                else if (itemTag.find("AK-74") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.5-45x39.FMJ")};
                else if (itemTag.find("AKM") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.7-62x39.FMJ")};
                else if (itemTag.find("G36") != std::string::npos || itemTag.find("M4A1") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.5-56x45.FMJ")};
                else if (itemTag.find("SR25") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.7-62x51.FMJ")};
                else if (itemTag.find("VSS") != std::string::npos || itemTag.find("Vintorez") != std::string::npos)
                    ammoTag = SDK::FGameplayTag{SDK::UKismetStringLibrary::Conv_StringToName(L"Item.Ammo.9x39.SP5")};
                else
                {
                    return;
                }
                
                hasAmmoTag = true;
            }

            // Fill magazine
            bool refilled = AutoMag_FillMagazine(itemActor, capacity, ammoTag);
            if (refilled)
            {
                Mod::ModFeedback::ShowMessage(
                    L"[AutoMag] Magazine refilled!", 2.0f,
                    SDK::FLinearColor{0.2f, 1.0f, 0.6f, 1.0f});
            }
            else
            {
            }
        }

        // =================================================================
        // VR Menu Input Hooks
        // =================================================================
        // These intercept VR controller input events dispatched through
        // ProcessEvent on the player character. When the VR menu is open,
        // the movement/trigger hooks return true to suppress game input.
        //
        // FInputActionValue layout (0x20 bytes, opaque):
        //   For buttons: first 4 bytes are a float (0.0 or 1.0)
        //   For 2D axis: first 8 bytes are two floats (X, Y)
        //   We log raw bytes on first intercept to verify.
        // =================================================================

        // Helper: dump first N bytes of FInputActionValue for diagnostics
        static void LogInputActionValueBytes(const char* hookName, const void* parms)
        {
            static int logCount = 0;
            if (logCount >= 10) return;  // Only log a few times
            ++logCount;

            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(parms);
            std::ostringstream oss;
            oss << "[VRMenu:Input] " << hookName << " raw bytes[0..31]: ";
            for (int i = 0; i < 32; ++i)
            {
                oss << std::hex << std::setw(2) << std::setfill('0') << (int)bytes[i];
                if (i < 31) oss << " ";
            }
            // Also interpret as floats
            const float* asFloat = reinterpret_cast<const float*>(parms);
            oss << "  floats[0..3]: " << std::dec;
            for (int i = 0; i < 4; ++i)
            {
                oss << asFloat[i];
                if (i < 3) oss << ", ";
            }
            LOG_INFO(oss.str());
        }

        // Toggle menu: Left Y/B button (IA_Button2_Left)
        // _17 = Started, _18 = Completed. We act on Started only.
        bool Hook_VRMenu_ToggleButton(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)originalFn;
            if (!object || !function || !parms)
                return false;

            // Only act on _17 (Started trigger), not _18 (Completed)
            const std::string fnName = function->GetName();
            if (fnName.find("_17") == std::string::npos)
                return false;

            LogInputActionValueBytes("ToggleButton", parms);

            auto* menu = Mod::VRMenuSubsystem::Get();
            if (menu)
            {
                menu->OnToggleMenu();
                LOG_INFO("[VRMenu:Input] Toggle button pressed (Started). Menu is now: " 
                         << (menu->IsMenuOpen() ? "OPEN" : "CLOSED"));
            }

            return false;  // Don't suppress the Y/B button from the game
        }

        // Navigation: Left thumbstick (IA_Movement)
        // _0 = Started, _1 = Triggered (continuous). We want _1 for ongoing stick input.
        // When menu is open, we read Y axis for up/down and suppress game movement.
        bool Hook_VRMenu_Movement(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)originalFn;
            if (!object || !function || !parms)
                return false;

            auto* menu = Mod::VRMenuSubsystem::Get();
            if (!menu || !menu->IsMenuOpen())
                return false;  // Menu closed — let game handle movement normally

            // Log raw bytes for the first few intercepts to diagnose layout
            LogInputActionValueBytes("Movement", parms);

            // FInputActionValue is at offset 0 in the params struct.
            // For a 2D axis: interpret first 8 bytes as two floats (X, Y).
            const float* axisValues = reinterpret_cast<const float*>(parms);
            float thumbstickY = axisValues[1];  // Y axis = forward/back = up/down in menu

            menu->OnNavigate(thumbstickY);

            return true;  // SUPPRESS game movement while menu is open
        }

        // Select: Left trigger (IA_Trigger_Left)
        // When menu is open, execute the selected item.
        bool Hook_VRMenu_TriggerLeft(SDK::UObject* object, SDK::UFunction* function, void* parms, HookManager::ProcessEventFn originalFn)
        {
            (void)originalFn;
            if (!object || !function || !parms)
                return false;

            auto* menu = Mod::VRMenuSubsystem::Get();
            if (!menu || !menu->IsMenuOpen())
                return false;  // Menu closed — let game handle trigger normally

            LogInputActionValueBytes("TriggerLeft", parms);

            // For trigger: first float is the analog value (0.0 to 1.0).
            // Only select on a firm press.
            const float* triggerValue = reinterpret_cast<const float*>(parms);
            if (triggerValue[0] > 0.5f)
            {
                menu->OnSelect();
                LOG_INFO("[VRMenu:Input] Trigger pressed (value=" << triggerValue[0] << "), executing selected item");
            }

            return true;  // SUPPRESS game trigger (no shooting) while menu is open
        }

    } // anonymous namespace

    // Static member initialization
    HookManager::ProcessEventFn HookManager::originalProcessEvent_ = nullptr;
    std::unordered_map<void **, HookManager::ProcessEventFn> HookManager::originalByVTable_;

    HookManager &HookManager::Get()
    {
        static HookManager instance;
        return instance;
    }

    HookManager::HookManager()
        : initialized_(false), processEventHooked_(false), unlimitedAmmoEnabled_(false), durabilityBypassEnabled_(false),
          hungerDisabled_(false), fatigueDisabled_(false), autoMagEnabled_(false)
    {
    }

    HookManager::~HookManager()
    {
        Shutdown();
    }

    ScopedProcessEventGuard::ScopedProcessEventGuard()
    {
        previous = HookManager::GetReentryGuard();
        HookManager::SetReentryGuard(true);
    }

    ScopedProcessEventGuard::~ScopedProcessEventGuard()
    {
        HookManager::SetReentryGuard(previous);
    }

    void HookManager::SetReentryGuard(bool enabled)
    {
        gProcessEventReentryGuard = enabled;
    }

    bool HookManager::GetReentryGuard()
    {
        return gProcessEventReentryGuard;
    }

    // ---------------------------------------------------------------------
    // Diagnostics: ProcessEvent trace control API
    // ---------------------------------------------------------------------
    void HookManager::Trace_SetEnabled(bool enabled)
    {
        gTraceEnabled.store(enabled, std::memory_order_relaxed);

        std::string fnFilter;
        std::string objFilter;
        {
            std::lock_guard<std::mutex> lock(gTraceMutex);
            fnFilter = gTraceFilterLower;
            objFilter = gTraceObjectFilterLower;
        }

        gTraceFile.OnEnabledChanged(enabled, fnFilter, objFilter);

        LOG_INFO("[Trace] TRACE " << (enabled ? "ENABLED" : "DISABLED")
                                  << " path=" << gTraceFile.GetPathCopy()
                                  << " fnFilter='" << fnFilter << "'"
                                  << " objFilter='" << objFilter << "'");
    }

    bool HookManager::Trace_IsEnabled()
    {
        return gTraceEnabled.load(std::memory_order_relaxed);
    }

    void HookManager::Trace_SetFilter(const std::string& filterSubstringLower)
    {
        std::lock_guard<std::mutex> lock(gTraceMutex);
        gTraceFilterLower = filterSubstringLower;
        LOG_INFO("[Trace] Filter='" << gTraceFilterLower << "'");

        // Marker for later correlation.
        {
            std::lock_guard<std::mutex> tlock(gTraceFile.Mutex);
            std::ostringstream details;
            details << "\"fnFilter\":\"" << TraceFileWriter::JsonEscape(gTraceFilterLower) << "\"";
            gTraceFile.WriteMarkerLocked("TRACE_FILTER", details.str());
        }
    }

    void HookManager::Trace_SetObjectFilter(const std::string& filterSubstringLower)
    {
        std::lock_guard<std::mutex> lock(gTraceMutex);
        gTraceObjectFilterLower = filterSubstringLower;
        LOG_INFO("[Trace] ObjectFilter='" << gTraceObjectFilterLower << "'");

        {
            std::lock_guard<std::mutex> tlock(gTraceFile.Mutex);
            std::ostringstream details;
            details << "\"objFilter\":\"" << TraceFileWriter::JsonEscape(gTraceObjectFilterLower) << "\"";
            gTraceFile.WriteMarkerLocked("TRACE_OBJECT_FILTER", details.str());
        }
    }

    void HookManager::Trace_Reset()
    {
        TraceReset();
        LOG_INFO("[Trace] Reset");

        gTraceFile.ResetFile();
    }

    std::string HookManager::Trace_Dump(int topN, int lastN)
    {
        return TraceDumpSummary(topN, lastN);
    }

    void HookManager::Trace_Flush()
    {
        gTraceFile.Flush();
        LOG_INFO("[Trace] Flush requested");
    }

    std::string HookManager::Trace_GetFilePath()
    {
        return gTraceFile.GetPathCopy();
    }

    std::string HookManager::TabletDiag_GetLastHolsteredSummary()
    {
        std::lock_guard<std::mutex> lock(TabletMutexRef());
        const std::string& actor = LastHolsteredActorRef();
        const std::string& cls = LastHolsteredActorClassRef();
        if (actor.empty() && cls.empty())
            return "tablet_last: <none>";
        return std::string("tablet_last: actor=") + actor + " class=" + cls;
    }

    void HookManager::TabletDiag_SetEnabled(bool enabled)
    {
        gTabletInteractionDiagEnabled.store(enabled, std::memory_order_relaxed);
        LOG_INFO("[HookManager][TabletDiag] Interaction diag " << (enabled ? "ENABLED" : "DISABLED"));
    }

    bool HookManager::TabletDiag_IsEnabled()
    {
        return gTabletInteractionDiagEnabled.load(std::memory_order_relaxed);
    }

    std::string HookManager::TabletDiag_GetLastInteractionSummary()
    {
        std::lock_guard<std::mutex> lock(gTabletInteractionMutex);
        if (gLastTabletInteraction.empty())
            return "tablet_diag_last: <none>";
        return std::string("tablet_diag_last: ") + gLastTabletInteraction;
    }

    void HookManager::NotifBridge_SetEnabled(bool enabled, bool playSound)
    {
        gNotifBridgeEnabled.store(enabled, std::memory_order_relaxed);
        gNotifBridgePlaySound.store(playSound, std::memory_order_relaxed);
        LOG_INFO("[HookManager][NotifBridge] " << (enabled ? "ENABLED" : "DISABLED") << " playSound=" << (playSound ? "true" : "false"));
    }

    bool HookManager::NotifBridge_IsEnabled()
    {
        return gNotifBridgeEnabled.load(std::memory_order_relaxed);
    }

    bool HookManager::NotifBridge_IsPlaySoundEnabled()
    {
        return gNotifBridgePlaySound.load(std::memory_order_relaxed);
    }

    bool HookManager::Initialize()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (initialized_)
        {
            LOG_INFO("[HookManager] Already initialized");
            return true;
        }

        LOG_INFO("[HookManager] Initializing hook manager...");

        // For POC: We'll use manual VTable hooking for ProcessEvent
        // In future: Can integrate MinHook for more robust hooking

        RegisterDefaultHooks();

        initialized_ = true;
        LOG_INFO("[HookManager] Hook manager initialized successfully");
        return true;
    }

    void HookManager::RegisterNamedHook(const std::string &functionName, NamedHookFn hookFn)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        namedHooks_[functionName] = hookFn;
        namedHooksSnapshot_ = namedHooks_;
        // Clear cache so it rebuilds with new name
        cachedNamedHooks_.clear();
        LOG_INFO("[HookManager] Registered named hook: " << functionName << " (snapshot updated)");
    }

    void HookManager::RegisterDefaultHooks()
    {
        // Unlimited ammo core hooks
        namedHooks_["TryExtractNextItem"] = &Hook_TryExtractNextItem;
        namedHooks_["ServerShootProjectile"] = &Hook_ServerShootProjectile;

        // Durability bypass (weapon degradation)
        namedHooks_["DamageDurabilityFromShot"] = &Hook_DurabilityNoOp;
        namedHooks_["ChangeDurability"] = &Hook_DurabilityNoOp;

        // Arena behavior
        //namedHooks_["RequestAttackRole"] = &Hook_RequestAttackRole;

        // Player statsx
        namedHooks_["ChangeHungerAndNotifyAll"] = &Hook_ChangeHunger;
        namedHooks_["ChangeStaminaAndNotifyAll"] = &Hook_ChangeStamina;

        // Tablet discovery (diagnostic only)
        namedHooks_["SetHolsteredActor"] = &Hook_SetHolsteredActor;

        // Tablet interaction diagnostics (toggleable via TCP)
        namedHooks_["OnGrip"] = &Hook_Tablet_OnGrip;
        namedHooks_["OnGripRelease"] = &Hook_Tablet_OnGripRelease;
        namedHooks_["OnButtonPressedEvent__DelegateSignature"] = &Hook_Tablet_UiDelegate;
        namedHooks_["OnClick_ZoomPlus__DelegateSignature"] = &Hook_Tablet_UiDelegate;
        namedHooks_["OnClick_ZoomMinus__DelegateSignature"] = &Hook_Tablet_UiDelegate;

        // Notification discovery (trace-gated; helps identify the "safety is on" path)
        namedHooks_["ShowSubtitles"] = &Hook_LogShowSubtitles;
        namedHooks_["ShowMessage"] = &Hook_LogShowMessage;

        // AutoMag: SetHolsteredActor (holster components) + PutItemToContainer (body pouches)
        namedHooks_["PutItemToContainer"] = &Hook_PutItemToContainer;

        // VR Menu input hooks
        // Left Y/B button: toggle menu open/close (two variants: _17=Started, _18=Completed)
        namedHooks_["InpActEvt_IA_Button2_Left_K2Node_EnhancedInputActionEvent_17"] = &Hook_VRMenu_ToggleButton;
        namedHooks_["InpActEvt_IA_Button2_Left_K2Node_EnhancedInputActionEvent_18"] = &Hook_VRMenu_ToggleButton;
        // Left thumbstick: navigate menu up/down when open (_0=Started, _1=Triggered/continuous)
        namedHooks_["InpActEvt_IA_Movement_K2Node_EnhancedInputActionEvent_0"] = &Hook_VRMenu_Movement;
        namedHooks_["InpActEvt_IA_Movement_K2Node_EnhancedInputActionEvent_1"] = &Hook_VRMenu_Movement;
        // Left trigger: select menu item when open
        namedHooks_["InpActEvt_IA_Trigger_Left_K2Node_EnhancedInputActionEvent_24"] = &Hook_VRMenu_TriggerLeft;

        LOG_INFO("[HookManager] VR Menu input hooks registered (5 events)");

        // Build snapshot for lock-free ProcessEvent dispatch
        namedHooksSnapshot_ = namedHooks_;
        cachedNamedHooks_.clear();
        LOG_INFO("[HookManager] Built hook snapshot from " << namedHooks_.size() << " registered hooks");
    }

    void HookManager::Shutdown()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        if (!initialized_)
            return;

        LOG_INFO("[HookManager] Shutting down hook manager...");

        if (processEventHooked_)
        {
            RemoveProcessEventHookInternal();
        }

        initialized_ = false;
        LOG_INFO("[HookManager] Hook manager shut down");
    }

    bool HookManager::InstallProcessEventHook()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        return InstallProcessEventHookInternal();
    }

    bool HookManager::InstallProcessEventHookInternal()
    {

        if (!initialized_)
        {
            LOG_ERROR("[HookManager] Cannot install hook: manager not initialized");
            return false;
        }

        if (processEventHooked_)
        {
            LOG_INFO("[HookManager] ProcessEvent hook already installed");
            return true;
        }

        LOG_INFO("[HookManager] Installing ProcessEvent hook...");

        // Safety: check GObjects is actually there and has enough items to be useful
        if (SDK::UObject::GObjects->Num() < 100) 
        {
            LOG_ERROR("[HookManager] GObjects looks empty (" << SDK::UObject::GObjects->Num() << " objects), skipping hook for now");
            return false;
        }

        auto *objectArray = SDK::UObject::GObjects.GetTypedPtr();
        if (!objectArray)
        {
            LOG_ERROR("[HookManager] GObjects is null");
            return false;
        }

        const UINT32 objectCount = objectArray->Num();
        if (objectCount <= 0)
        {
            LOG_ERROR("[HookManager] GObjects has no objects yet");
            return false;
        }

        std::unordered_set<void **> seenVTables;
        int patchedVTables = 0;

        for (UINT32 index = 0; index < objectCount; ++index)
        {
            SDK::UObject *obj = objectArray->GetByIndex(index);
            if (!obj || !obj->VTable)
                continue;

            void **vtable = reinterpret_cast<void **>(obj->VTable);
            if (!vtable)
                continue;

            if (!seenVTables.insert(vtable).second)
                continue;

            void **slot = &vtable[SDK::Offsets::ProcessEventIdx];
            if (!slot || !*slot)
                continue;

            auto original = reinterpret_cast<ProcessEventFn>(*slot);
            if (original == reinterpret_cast<ProcessEventFn>(&HookManager::Hook_ProcessEvent))
            {
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                continue;
            }

            if (!originalProcessEvent_)
            {
                originalProcessEvent_ = original;
            }

            originalByVTable_[vtable] = original;
            *slot = reinterpret_cast<void *>(&HookManager::Hook_ProcessEvent);

            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), oldProtect, &ignored);

            patchedVTables++;
        }

        if (patchedVTables <= 0)
        {
            LOG_ERROR("[HookManager] Failed to patch any ProcessEvent vtables");
            return false;
        }

        processEventHooked_ = true;
        LOG_INFO("[HookManager] ProcessEvent hook installed. Patched vtables: " << patchedVTables);
        return true;
    }

    bool HookManager::RemoveProcessEventHook()
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);

        return RemoveProcessEventHookInternal();
    }

    bool HookManager::RemoveProcessEventHookInternal()
    {

        if (!processEventHooked_)
        {
            LOG_INFO("[HookManager] ProcessEvent hook not installed");
            return true;
        }

        LOG_INFO("[HookManager] Removing ProcessEvent hook...");

        int restoredVTables = 0;
        for (auto &entry : originalByVTable_)
        {
            void **vtable = entry.first;
            ProcessEventFn original = entry.second;
            if (!vtable || !original)
                continue;

            void **slot = &vtable[SDK::Offsets::ProcessEventIdx];
            if (!slot)
                continue;

            DWORD oldProtect = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect))
            {
                continue;
            }

            *slot = reinterpret_cast<void *>(original);

            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), oldProtect, &ignored);
            restoredVTables++;
        }

        originalByVTable_.clear();

        processEventHooked_ = false;
        LOG_INFO("[HookManager] ProcessEvent hook removed. Restored vtables: " << restoredVTables);
        return true;
    }

    void HookManager::SetUnlimitedAmmoEnabled(bool enabled)
    {
        unlimitedAmmoEnabled_ = enabled;

        if (enabled && initialized_ && !processEventHooked_)
        {
            InstallProcessEventHook();
        }

        Mod::ModFeedback::ShowMessage(enabled ? L"Unlimited Ammo ENABLED" : L"Unlimited Ammo DISABLED", 3.0f, {0.0f, 1.0f, 0.0f, 1.0f});
        LOG_INFO("[HookManager] Unlimited ammo " << (enabled ? "enabled" : "disabled"));
    }

    void HookManager::SetDurabilityBypassEnabled(bool enabled)
    {
        durabilityBypassEnabled_ = enabled;

        if (enabled && initialized_ && !processEventHooked_)
        {
            InstallProcessEventHook();
        }

        Mod::ModFeedback::ShowMessage(enabled ? L"Durability Bypass ENABLED" : L"Durability Bypass DISABLED", 3.0f, {0.0f, 1.0f, 0.0f, 1.0f});
        LOG_INFO("[HookManager] Durability bypass " << (enabled ? "enabled" : "disabled"));
    }

    void HookManager::SetHungerDisabled(bool disabled)
    {
        hungerDisabled_ = disabled;

        if (disabled && initialized_ && !processEventHooked_)
        {
            InstallProcessEventHook();
        }

        Mod::ModFeedback::ShowMessage(disabled ? L"Hunger Bypass ENABLED" : L"Hunger Bypass DISABLED", 3.0f, {0.0f, 1.0f, 0.0f, 1.0f});
        LOG_INFO("[HookManager] Hunger disabled " << (disabled ? "enabled" : "disabled"));
    }

    void HookManager::SetFatigueDisabled(bool disabled)
    {
        fatigueDisabled_ = disabled;

        if (disabled && initialized_ && !processEventHooked_)
        {
            InstallProcessEventHook();
        }

        Mod::ModFeedback::ShowMessage(disabled ? L"Fatigue Bypass ENABLED" : L"Fatigue Bypass DISABLED", 3.0f, {0.0f, 1.0f, 0.0f, 1.0f});
        LOG_INFO("[HookManager] Fatigue disabled " << (disabled ? "enabled" : "disabled"));
    }

    void HookManager::SetAutoMagEnabled(bool enabled)
    {
        autoMagEnabled_ = enabled;

        if (enabled && initialized_ && !processEventHooked_)
        {
            InstallProcessEventHook();
        }

        LOG_INFO("[HookManager] AutoMag " << (enabled ? "enabled" : "disabled"));
    }

    void HookManager::Hook_ProcessEvent(SDK::UObject *pThis, SDK::UFunction *function, void *parms)
    {
        ProcessEventFn originalFn = nullptr;
        if (pThis && pThis->VTable)
        {
            void **vtable = reinterpret_cast<void **>(pThis->VTable);
            auto it = originalByVTable_.find(vtable);
            if (it != originalByVTable_.end())
            {
                originalFn = it->second;
            }
        }

        if (!originalFn)
        {
            originalFn = originalProcessEvent_;
        }

        if (!originalFn)
        {
            return;
        }

        if (!function)
        {
            originalFn(pThis, function, parms);
            return;
        }

        // Diagnostics trace (data-collection mode). Keep it extremely cheap when disabled.
        // Skip tracing during reentry-guarded calls to reduce self-noise.
        if (!gProcessEventReentryGuard)
        {
            TraceOnProcessEvent(pThis, function);
            // AutoMag diagnostic monitor: log ALL magazine/container/holster related ProcessEvents
            //AutoMag_DiagnosticMonitor(pThis, function);
        }

        if (gProcessEventReentryGuard)
        {
            originalFn(pThis, function, parms);
            return;
        }

        HookManager &mgr = HookManager::Get();

        // Run mod tick on game thread (throttle to ~100Hz)
        {
            static std::atomic<long long> lastTickTime{0};
            auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            // 10ms = 10,000,000 ns if using high_res, but steady_clock usually in ns or ticks.
            // Let's use milliseconds directly for clarity
            auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            long long expected = lastTickTime.load();
            if (nowMs - expected >= 10)
            {
                if (lastTickTime.compare_exchange_strong(expected, nowMs))
                {
					SDK::UWorld *world = SDK::UWorld::GetWorld();
					// Allow OnTick to run even if world is null (main menu / map loading)
					// so TCP commands can still receive responses like `help`.
					ModMain::OnTick(world);
                }
            }
        }

        const bool hasGameplayHooks = mgr.IsUnlimitedAmmoEnabled() || mgr.IsDurabilityBypassEnabled();
        
        // Use shared_lock for the fast path
        std::shared_lock<std::shared_mutex> lock(mgr.mutex_);
        const bool hasNamedHooks = !mgr.namedHooksSnapshot_.empty();

        const bool traceEnabled = gTraceEnabled.load(std::memory_order_relaxed);

        // Fast-path: if no gameplay hooks, no named hooks, and trace is off, skip all string work.
        if (!hasGameplayHooks && !hasNamedHooks && !traceEnabled)
        {
            lock.unlock(); // Release lock before calling original
            originalFn(pThis, function, parms);
            return;
        }

        // Tablet UI interaction discovery: we cannot rely on knowing every exact delegate name up-front.
        // When enabled, log any *tablet-scoped* delegate signature events as a catch-all.
        if (gTabletInteractionDiagEnabled.load(std::memory_order_relaxed) && pThis)
        {
            const std::string fnName = function->GetName();
            if (fnName.find("DelegateSignature") != std::string::npos)
            {
                const std::string objFull = pThis->GetFullName();
                if (objFull.find("Tablet") != std::string::npos)
                {
                    static uint64_t lastMs = 0;
                    const uint64_t nowMs = TraceFileWriter::NowTickMs();
                    if (lastMs == 0 || nowMs - lastMs >= 150)
                    {
                        lastMs = nowMs;
                        const std::string fnFull = function->GetFullName();
                        const std::string outer = (pThis->Outer ? pThis->Outer->GetFullName() : std::string("<null>"));
                        const std::string s = std::string("[TabletDiag] UiDelegate obj=") + objFull + " outer=" + outer + " fn=" + fnFull;
                        LOG_INFO(s);
                        TabletInteraction_Record(s);
                    }
                }
            }
        }

        // Check cache first (Lock-protected or shared_locked)
        auto cacheIt = mgr.cachedNamedHooks_.find(function);
        if (cacheIt != mgr.cachedNamedHooks_.end())
        {
            NamedHookFn hook = cacheIt->second;
            lock.unlock(); // Unlock before calling hook/original
            if (hook)
            {
                if (hook(pThis, function, parms, originalFn))
                    return;
            }
            originalFn(pThis, function, parms);
            return;
        }

        // Cache miss: need to check by name and update cache
        const std::string functionName = function->GetName();
        auto it = mgr.namedHooks_.find(functionName);
        NamedHookFn foundHook = (it != mgr.namedHooks_.end()) ? it->second : nullptr;

        // Upgrading to unique_lock to update cache is expensive, but only happens once per function.
        // However, we can't easily upgrade shared_lock to unique_lock in C++17 shared_mutex.
        // Instead, we'll release and re-acquire, or just use naming.
        lock.unlock();
        
        {
            std::unique_lock<std::shared_mutex> uniqueLock(mgr.mutex_);
            mgr.cachedNamedHooks_[function] = foundHook;
        }

        if (foundHook)
        {
            if (foundHook(pThis, function, parms, originalFn))
                return;
        }

        originalFn(pThis, function, parms);
    }
}
