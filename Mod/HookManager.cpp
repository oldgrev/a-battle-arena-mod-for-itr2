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
//





#include "HookManager.hpp"
#include "Cheats.hpp"
#include "ModMain.hpp"
#include "CommandQueue.hpp"
#include "Logging.hpp"
#include "ArenaSubsystem.hpp"

#include "ModFeedback.hpp"

#include <Windows.h>
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

            auto *extractParams = reinterpret_cast<TryExtractNextItemParams *>(parms);
            
         /*   {
                static int logCount = 0;
                if (logCount < 10)
                {
                    logCount++;
                    LOG_INFO("[HookManager][TryExtractNextItem] Called. extractParams=" << extractParams 
                             << ", CurrentReturnValue=" << (int)extractParams->ReturnValue);
                }
            }*/
            
            // Only interfere when:
            // - we are in a held-weapon context (prevents shop/world weapons being mutated)
            // - we have a known ammo tag (prevents breaking fresh weapons when enabling cheat before first shot)
            // if (!IsHeldContext(object))
            // {
            //     static int notHeldLogs = 0;
            //     if (notHeldLogs < 10)
            //     {
            //         notHeldLogs++;
            //         LOG_INFO("[HookManager][TryExtractNextItem] Skip (not held context)");
            //     }
            //     return false;
            // }

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
          hungerDisabled_(false), fatigueDisabled_(false)
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
        // namedHooks_["RequestAttackRole"] = &Hook_RequestAttackRole;

        // Player statsx
        namedHooks_["ChangeHungerAndNotifyAll"] = &Hook_ChangeHunger;
        namedHooks_["ChangeStaminaAndNotifyAll"] = &Hook_ChangeStamina;

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
                    if (world)
                    {
                        ModMain::OnTick(world);
                    }
                }
            }
        }

        const bool hasGameplayHooks = mgr.IsUnlimitedAmmoEnabled() || mgr.IsDurabilityBypassEnabled();
        
        // Use shared_lock for the fast path
        std::shared_lock<std::shared_mutex> lock(mgr.mutex_);
        const bool hasNamedHooks = !mgr.namedHooksSnapshot_.empty();

        // Fast-path: if no gameplay hooks and no named hooks are active, skip all string work.
        if (!hasGameplayHooks && !hasNamedHooks)
        {
            lock.unlock(); // Release lock before calling original
            originalFn(pThis, function, parms);
            return;
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
