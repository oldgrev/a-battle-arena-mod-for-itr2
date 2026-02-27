#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    class HookManager;

    struct ScopedProcessEventGuard
    {
        bool previous;
        ScopedProcessEventGuard();
        ~ScopedProcessEventGuard();
    };

    /**
     * @brief Central manager for function hooking
     *
     * This class provides a centralized system for installing and managing
     * function hooks in the game. It uses manual VTable patching of the 
     * ProcessEvent slot across all active engine objects.
     *
     * Targeting ProcessEvent allows the mod to intercept engine-reflected 
     * calls (Blueprints, RPCs, etc.) without needing direct assembly offsets
     * for every individual function.
     */
    class HookManager
    {
    public:
        using ProcessEventFn = void (*)(const SDK::UObject *, SDK::UFunction *, void *);

        // Named ProcessEvent hook. Return true if handled (original should NOT be called by dispatcher).
        using NamedHookFn = bool (*)(SDK::UObject *object, SDK::UFunction *function, void *parms, ProcessEventFn originalFn);

        /**
         * @brief Get the singleton instance
         */
        static HookManager &Get();

        /**
         * @brief Initialize the hook manager and MinHook library
         * @return true if initialization succeeded
         */
        bool Initialize();

        /**
         * @brief Shutdown the hook manager and remove all hooks
         */
        void Shutdown();

        /**
         * @brief Check if the hook manager is initialized
         */
        bool IsInitialized() const { return initialized_; }

        /**
         * @brief Check if ProcessEvent is hooked
         */
        bool IsProcessEventHooked() const { return processEventHooked_; }

        /**
         * @brief Install ProcessEvent hook for intercepting blueprint function calls
         * @return true if hook was installed successfully
         */
        bool InstallProcessEventHook();

        /**
         * @brief Remove ProcessEvent hook
         * @return true if hook was removed successfully
         */
        bool RemoveProcessEventHook();

        // Register a named ProcessEvent hook. HookManager will call it when Function->GetName() matches.
        // Intended for POC-level fast iteration: hard short-circuit / replace behavior.
        void RegisterNamedHook(const std::string &functionName, NamedHookFn hookFn);

        /**
         * @brief Set whether unlimited ammo is enabled
         * This is checked by the ProcessEvent hook
         */
        void SetUnlimitedAmmoEnabled(bool enabled);

        /**
         * @brief Set whether durability bypass is enabled
         */
        void SetDurabilityBypassEnabled(bool enabled);

        /**
         * @brief Get whether durability bypass is enabled
         */
        bool IsDurabilityBypassEnabled() const { return durabilityBypassEnabled_; }

        /**
         * @brief Get whether unlimited ammo is enabled
         */
        bool IsUnlimitedAmmoEnabled() const { return unlimitedAmmoEnabled_; }

        void SetHungerDisabled(bool disabled);
        bool IsHungerDisabled() const { return hungerDisabled_; }

        void SetFatigueDisabled(bool disabled);
        bool IsFatigueDisabled() const { return fatigueDisabled_; }

        /**
         * @brief Set whether automag is enabled
         * When active, magazines placed in mag pouches auto-refill to capacity.
         */
        void SetAutoMagEnabled(bool enabled);
        bool IsAutoMagEnabled() const { return autoMagEnabled_; }

        // Reentry guard for when the mod calls SDK functions
        static void SetReentryGuard(bool enabled);
        static bool GetReentryGuard();

        // -----------------------------------------------------------------
        // Diagnostics: ProcessEvent trace (data collection)
        // -----------------------------------------------------------------
        static void Trace_SetEnabled(bool enabled);
        static bool Trace_IsEnabled();
        static void Trace_SetFilter(const std::string& filterSubstringLower);
        static void Trace_SetObjectFilter(const std::string& filterSubstringLower);
        static void Trace_Reset();
        static std::string Trace_Dump(int topN = 30, int lastN = 50);
        static void Trace_Flush();
        static std::string Trace_GetFilePath();

        // -----------------------------------------------------------------
        // Diagnostics: Tablet discovery
        // -----------------------------------------------------------------
        static std::string TabletDiag_GetLastHolsteredSummary();

        // Tablet interaction diagnostics (grip + UI delegates). Toggleable to avoid noise.
        static void TabletDiag_SetEnabled(bool enabled);
        static bool TabletDiag_IsEnabled();
        static std::string TabletDiag_GetLastInteractionSummary();

        // -----------------------------------------------------------------
        // Diagnostics: Notification bridge
        // -----------------------------------------------------------------
        // Mirrors game-generated subtitles into VR-visible popup + optional sound.
        static void NotifBridge_SetEnabled(bool enabled, bool playSound);
        static bool NotifBridge_IsEnabled();
        static bool NotifBridge_IsPlaySoundEnabled();

    private:
        HookManager();
        ~HookManager();

        // Disable copy and assignment
        HookManager(const HookManager &) = delete;
        HookManager &operator=(const HookManager &) = delete;

        bool initialized_;
        bool processEventHooked_;
        std::atomic<bool> unlimitedAmmoEnabled_;
        std::atomic<bool> durabilityBypassEnabled_;
        std::atomic<bool> hungerDisabled_;
        std::atomic<bool> fatigueDisabled_;
        std::atomic<bool> autoMagEnabled_;

        // One original ProcessEvent pointer for fast fallback/logging
        static ProcessEventFn originalProcessEvent_;

        // Per-vtable original ProcessEvent pointers
        static std::unordered_map<void **, ProcessEventFn> originalByVTable_;

        // ProcessEvent hook function
        static void Hook_ProcessEvent(SDK::UObject *pThis, SDK::UFunction *function, void *parms);

        bool InstallProcessEventHookInternal();
        bool RemoveProcessEventHookInternal();

        void RegisterDefaultHooks();

        // Thread safety
        mutable std::shared_mutex mutex_;

        // Named hooks keyed by UFunction::GetName()
        std::unordered_map<std::string, NamedHookFn> namedHooks_;

        // Cached function pointer hooks to avoid string lookups in every ProcessEvent
        std::unordered_map<SDK::UFunction *, NamedHookFn> cachedNamedHooks_;
        
        // Thread-safe snapshot for ProcessEvent dispatch
        std::unordered_map<std::string, NamedHookFn> namedHooksSnapshot_;
    };
}
