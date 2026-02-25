#pragma once

#include <string>
#include <atomic>
#include <unordered_map>
#include <chrono>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    class Cheats
    {
    public:
        Cheats();
        ~Cheats() = default;

        // Toggle god mode on/off
        void ToggleGodMode();

        // Check if god mode is active
        bool IsGodModeActive() const;

        // Toggle unlimited ammo on/off
        void ToggleUnlimitedAmmo();

        // Check if unlimited ammo is active
        bool IsUnlimitedAmmoActive() const;

        // Toggle durability bypass on/off
        void ToggleDurabilityBypass();

        // Check if durability bypass is active
        bool IsDurabilityBypassActive() const;

        // Toggle hunger disabled on/off
        void ToggleHungerDisabled();

        // Check if hunger disabled is active
        bool IsHungerDisabledActive() const;

        // Toggle fatigue disabled on/off
        void ToggleFatigueDisabled();

        // Check if fatigue disabled is active
        bool IsFatigueDisabledActive() const;

        // Toggle bullet time on/off
        void ToggleBulletTime();

        // Set bullet time scale (e.g. 0.2 for 5x slow mo)
        void SetBulletTimeScale(float scale);

        // Toggle no-clip using the game's cheat subsystem
        void ToggleNoClip();
        bool IsNoClipActive() const;

        // Toggle whether the player is allowed to jump
        void ToggleJumpAllowed();
        bool IsJumpAllowedActive() const;

        // Add money instantly (delta may be negative)
        void AddMoney(int amount);

        // Change the access level (0-3 interpreted by game)
        void SetAccessLevel(int level);

        // Toggle internal debug mode for extra log messages
        void ToggleDebugMode();
        bool IsDebugModeActive() const;

        // Scale intensity of portable player lights (flashlight/headlamp/etc that use BPC_LightComp)
        // 1.0 = default game brightness
        void SetPortableLightIntensityScale(float scale);
        float GetPortableLightIntensityScale() const;

        // Get cheat status as string
        std::string GetStatus() const;

        // Update cheats - called every frame
        void Update(SDK::UWorld *world);

        // Deactivate all cheats (e.g. on level change)
        void DeactivateAll();

        // Explicit setters (used by command handler / subsystems)
        void SetGodMode(bool enabled);
        void SetUnlimitedAmmo(bool enabled);
        void SetHungerDisabled(bool enabled);
        void SetFatigueDisabled(bool enabled);

    private:
        std::atomic<bool> godModeActive_{false};
        std::atomic<bool> unlimitedAmmoActive_{false};
        std::atomic<bool> durabilityBypassActive_{false};
        std::atomic<bool> hungerDisabled_{false};
        std::atomic<bool> fatigueDisabled_{false};
        std::atomic<bool> bulletTimeActive_{false};
        std::atomic<float> bulletTimeScale_{0.2f};
        std::atomic<bool> bulletTimeDirty_{true};

        // additional cheat state
        std::atomic<bool> noClipActive_{false};
        std::atomic<bool> jumpAllowedActive_{false};
        std::atomic<bool> debugModeActive_{false};

        // Counter for periodic durability check (every 600 cycles)
        uint32_t durabilityCheckCounter_{0};

        // Cached player character for performance
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C *cachedPlayer_{nullptr};

        // Helper to find the player character
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C *FindPlayer(SDK::UWorld *world);

        // Apply god mode logic
        void ApplyGodMode(SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player);

        // Apply stats cheats (hunger/fatigue) logic
        void ApplyStatsCheats(SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player);

        // Periodic durability restoration for held items
        void ApplyDurabilityFix(SDK::UWorld *world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player);

        // Ensure held items move at normal speed during bullet time
        void UpdateHeldItemsDilation(SDK::UWorld *world, float targetDilation, SDK::ABP_RadiusPlayerCharacter_Gameplay_C *player);

        // Apply flashlight/headlamp brightness scaling
        void ApplyPortableLightBrightness(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player);

        // Track original light intensities so scaling is stable (no compounding each tick)
        std::unordered_map<SDK::ULightComponentBase*, float> portableLightOriginalIntensity_;
        std::atomic<float> portableLightIntensityScale_{1.0f};
        std::atomic<bool> portableLightScaleDirty_{true};
        SDK::UWorld* lastPortableLightWorld_{nullptr};

        SDK::UWorld* lastBulletTimeWorld_{nullptr};
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* lastBulletTimePlayer_{nullptr};
        uint32_t portableLightLogCounter_{0};
    };
}
