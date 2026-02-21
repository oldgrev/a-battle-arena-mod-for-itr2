// AI SHOULD:
// IF YOU HAVE ACCESS TO MCP server my-visual-studio, USE IT TO INITIATE A TEST BUILD AFTER MAKING CHANGES, SO YOU CAN SEE THE EFFECT OF YOUR CHANGES FASTER. THIS IS ESPECIALLY IMPORTANT FOR CODE THAT AFFECTS CHEATS, AS TESTING CHEATS INVOLVES A LOT OF STEPS (BUILD, COPY, LAUNCH GAME, ENABLE CHEATS IN-GAME, TEST CHEAT EFFECTS).
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
//


#include "pch.h"

#include "HookManager.hpp"
#include "Logging.hpp"

#include <Windows.h>
#include <unordered_set>

namespace Mod
{
    namespace
    {
        constexpr int kUnlimitedMagazineTarget = 30;
        SDK::FGameplayTag gLastObservedAmmoTag{};
        bool gHasLastObservedAmmoTag = false;

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

        void ForceResyncFirearm(SDK::URadiusFirearmComponent *component)
        {
            if (!component)
                return;

            // POC: emulate “mag reinsert” style resync
            component->InitializeMagazine();
            component->DeliverAmmoFromMagToChamber();
            component->SetWeaponCocked(true);
            component->SetSliderLock(false, true);
        }
    }

    // Static member initialization
    HookManager::ProcessEventFn HookManager::originalProcessEvent_ = nullptr;
    std::unordered_map<void **, HookManager::ProcessEventFn> HookManager::originalByVTable_;

    HookManager &HookManager::Get()
    {
        static HookManager instance;
        return instance;
    }

    HookManager::HookManager()
        : initialized_(false), processEventHooked_(false), unlimitedAmmoEnabled_(false)
    {
    }

    HookManager::~HookManager()
    {
        Shutdown();
    }

    bool HookManager::Initialize()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (initialized_)
        {
            LOG_INFO("[HookManager] Already initialized");
            return true;
        }

        LOG_INFO("[HookManager] Initializing hook manager...");

        // For POC: We'll use manual VTable hooking for ProcessEvent
        // In future: Can integrate MinHook for more robust hooking

        initialized_ = true;
        LOG_INFO("[HookManager] Hook manager initialized successfully");
        return true;
    }

    void HookManager::Shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

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
        std::lock_guard<std::mutex> lock(mutex_);

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

        LOG_INFO("[HookManager] Unlimited ammo " << (enabled ? "enabled" : "disabled"));
    }

    void HookManager::Hook_ProcessEvent(SDK::UObject *pThis, SDK::UFunction *function, void *parms)
    {
        // Prevent recursion when we call SDK functions from inside the hook.
        static thread_local bool reentryGuard = false;

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

        if (reentryGuard)
        {
            originalFn(pThis, function, parms);
            return;
        }

        const bool unlimitedAmmoEnabled = HookManager::Get().IsUnlimitedAmmoEnabled();
        if (!unlimitedAmmoEnabled)
        {
            originalFn(pThis, function, parms);
            return;
        }

        const std::string functionName = function->GetName();

        const bool isFirearmComponent = pThis && pThis->IsA(SDK::URadiusFirearmComponent::StaticClass());
        const bool isFirearmActor = pThis && pThis->IsA(SDK::ARadiusFirearmBase::StaticClass());
        const std::string objectClassName = (pThis && pThis->Class) ? pThis->Class->GetName() : "";
        const bool isFirearmRelated = isFirearmComponent ||
                                      isFirearmActor ||
                                      objectClassName.find("Shutter") != std::string::npos ||
                                      objectClassName.find("ItemStack") != std::string::npos ||
                                      objectClassName.find("BulletInternal") != std::string::npos ||
                                      objectClassName.find("RadiusFirearm") != std::string::npos;

        //LOG_INFO("[HookManager] Intercepted ProcessEvent: " << functionName << " on object of class " << objectClassName);

        // =========================
        // PROOF MODE: hard short-circuits
        // =========================

        // // Keep the internal bullet state from ever being cleared.
        // if (functionName == "SetBulletInvisible" && objectClassName.find("RadiusBulletInternal") != std::string::npos)
        // {
        //     static int setBulletInvisibleBypassLogs = 0;
        //     if (setBulletInvisibleBypassLogs < 20)
        //     {
        //         setBulletInvisibleBypassLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] Bypass SetBulletInvisible (" << objectClassName << ")");
        //     }
        //     return;
        // }


        // if (functionName == "UpdateBulletAmmo" && objectClassName.find("RadiusBulletInternal") != std::string::npos && parms)
        // {
        //     auto *params = reinterpret_cast<UpdateBulletAmmoParams *>(parms);
        //     if (gHasLastObservedAmmoTag)
        //     {
        //         params->AmmoTypeID = gLastObservedAmmoTag;
        //         params->bIsShell_0 = false;
        //     }

        //     static int updateBulletAmmoLogs = 0;
        //     if (updateBulletAmmoLogs < 20)
        //     {
        //         updateBulletAmmoLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] Force UpdateBulletAmmo tag="
        //                  << (gHasLastObservedAmmoTag ? gLastObservedAmmoTag.TagName.ToString() : std::string("<none>"))
        //                  << " shell=false (" << objectClassName << ")");
        //     }

        //     originalFn(pThis, function, parms);
        //     return;
        // }


        // Force muzzle shot path to always use a valid ammo tag and succeed.
        // if (functionName == "ShootProjectile" && objectClassName.find("Muzzle") != std::string::npos && parms)
        // {
        //     auto *params = reinterpret_cast<MuzzleShootProjectileParams *>(parms);
        //     if (gHasLastObservedAmmoTag)
        //     {
        //         params->GameplayTag = gLastObservedAmmoTag;
        //     }

        //     static int muzzleShootLogs = 0;
        //     if (muzzleShootLogs < 20)
        //     {
        //         muzzleShootLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] Force ShootProjectile tag="
        //                  << (gHasLastObservedAmmoTag ? gLastObservedAmmoTag.TagName.ToString() : std::string("<none>"))
        //                  << " (" << objectClassName << ")");
        //     }

        //     originalFn(pThis, function, parms);
        //     params->ReturnValue = true;
        //     return;
        // }
        // if (functionName == "IsDenyItemUse" && parms)
        // {
        //     auto *returnValue = reinterpret_cast<BoolReturnParam *>(parms);
        //     returnValue->ReturnValue = false;
        //     return;
        // }

        if (functionName == "TryExtractNextItem" && parms)
        {
            auto *extractParams = reinterpret_cast<TryExtractNextItemParams *>(parms);
            extractParams->ReturnValue = true;
            if (gHasLastObservedAmmoTag)
            {
                extractParams->ExtractedItemTag = gLastObservedAmmoTag;
            }
            return;
        }

        // if (functionName == "GetCurrentAmmoNumber" && parms)
        // {
        //     auto *ammoParams = reinterpret_cast<GetCurrentAmmoNumberParams *>(parms);
        //     ammoParams->Number = kUnlimitedMagazineTarget;
        //     ammoParams->ReturnValue = true;
        //     return;
        // }

        // if (functionName == "CanShoot" && parms)
        // {
        //     auto *returnValue = reinterpret_cast<BoolReturnParam *>(parms);
        //     returnValue->ReturnValue = true;
        //     return;
        // }

        // if (functionName == "IsOutOfAmmo" && parms)
        // {
        //     auto *returnValue = reinterpret_cast<BoolReturnParam *>(parms);
        //     returnValue->ReturnValue = false;
        //     return;
        // }

        // if (functionName == "TryNextAutomaticFire" && parms)
        // {
        //     auto *returnValue = reinterpret_cast<BoolReturnParam *>(parms);
        //     returnValue->ReturnValue = true;
        //     return;
        // }

        // if (isFirearmRelated &&
        //     (functionName == "SliderLocked" ||
        //      functionName == "OnSliderLock" ||
        //      functionName == "OnChangeShutterState" ||
        //      functionName == "OnOpenStateChange_Event" ||
        //      functionName == "OnDysfunction"))
        // {
        //     return;
        // }

        // if (functionName == "SetSliderLock" && parms)
        // {
        //     auto *params = reinterpret_cast<SetSliderLockParams *>(parms);
        //     params->bSet = false;
        //     params->bForce = true;
        //     params->ReturnValue = false;
        //     return;
        // }

        // if (isFirearmComponent && parms)
        // {
        //     if (functionName == "ServerUpdateWithAmmo")
        //     {
        //         auto *params = reinterpret_cast<ServerUpdateWithAmmoParams *>(parms);
        //         params->AmmoRep.bMagazineAttached = true;
        //         params->AmmoRep.Frame++;
        //     }
        //     else if (functionName == "ServerShootProjectile")
        //     {
        //         auto *params = reinterpret_cast<ServerShootProjectileParams *>(parms);

        //         gLastObservedAmmoTag = params->Rep.AmmoTag;
        //         gHasLastObservedAmmoTag = true;

        //         if (params->Rep.MagazineAmmoLeft < kUnlimitedMagazineTarget)
        //         {
        //             params->Rep.MagazineAmmoLeft = kUnlimitedMagazineTarget;
        //         }

        //         params->Rep.AmmoStateFrame++;
        //         params->Rep.Frame++;
        //     }
        //     else if (functionName == "ServerAmmoInserted" || functionName == "Server_InsertAmmoInMag")
        //     {
        //         auto *params = reinterpret_cast<ServerInsertAmmoParams *>(parms);
        //         params->Rep.bMagazineAttached = true;
        //         params->Rep.Frame++;
        //     }
        // }

        // // Force a resync whenever the firearm thinks its stack changed.
        // if (functionName == "OnItemStackChanged" && isFirearmComponent)
        // {
        //     originalFn(pThis, function, parms);
        //     reentryGuard = true;
        //     ForceResyncFirearm(static_cast<SDK::URadiusFirearmComponent *>(pThis));
        //     reentryGuard = false;

        //     static int stackResyncLogs = 0;
        //     if (stackResyncLogs < 20)
        //     {
        //         stackResyncLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] ForceResyncFirearm after OnItemStackChanged");
        //     }
        //     return;
        // }

        // // Force a resync on weapon OnUsed (covers the “gun drained but mag full” desync).
        // if (functionName == "OnUsed" && isFirearmActor)
        // {
        //     originalFn(pThis, function, parms);
        //     auto *weapon = static_cast<SDK::ARadiusFirearmBase *>(pThis);
        //     reentryGuard = true;
        //     ForceResyncFirearm(weapon ? weapon->FirearmComponent : nullptr);
        //     reentryGuard = false;

        //     static int usedResyncLogs = 0;
        //     if (usedResyncLogs < 20)
        //     {
        //         usedResyncLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] ForceResyncFirearm after OnUsed");
        //     }
        //     return;
        // }

        originalFn(pThis, function, parms);

        // After the server-shot RPC returns, immediately resync locally to avoid needing a mag reinsertion.
        if (functionName == "ServerShootProjectile" && isFirearmComponent)
        {
            reentryGuard = true;
            ForceResyncFirearm(static_cast<SDK::URadiusFirearmComponent *>(pThis));
            reentryGuard = false;

        }

        // if (isFirearmComponent)
        // {
        //     static int hookHitLogs = 0;
        //     static int shotRepLogs = 0;
        //     if (hookHitLogs < 40 &&
        //         (functionName == "ServerUpdateWithAmmo" ||
        //          functionName == "ServerShootProjectile" ||
        //          functionName == "TryExtractNextItem" ||
        //          functionName == "SetSliderLock" ||
        //          functionName == "SliderLocked" ||
        //          functionName == "GetCurrentAmmoNumber" ||
        //          functionName == "CanShoot" ||
        //          functionName == "TryReload" ||
        //          functionName == "IsOutOfAmmo"))
        //     {
        //         hookHitLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] Hit " << functionName);
        //     }

        //     if (functionName == "ServerShootProjectile" && parms && shotRepLogs < 20)
        //     {
        //         auto *params = reinterpret_cast<ServerShootProjectileParams *>(parms);
        //         shotRepLogs++;
        //         LOG_INFO("[HookManager][AmmoHook] Patched shot rep: MagazineAmmoLeft=" << params->Rep.MagazineAmmoLeft);
        //     }
        // }
    }
}
