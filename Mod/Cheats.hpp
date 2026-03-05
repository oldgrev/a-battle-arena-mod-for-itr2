#pragma once

#include <string>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <vector>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    class Cheats
    {
    public:
        enum class EnvironmentPruneCategory
        {
            All,
            PlantsBushes,
            Trees,
        };

        enum class EnvironmentPruneSource
        {
            All,
            Foliage,
            Grass,
            InstancedStaticMesh,
        };

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
        bool IsBulletTimeActive() const { return bulletTimeActive_.load(std::memory_order_relaxed); }

        // Set bullet time scale (e.g. 0.2 for 5x slow mo)
        void SetBulletTimeScale(float scale);

        // Toggle no-clip using the game's cheat subsystem
        void ToggleNoClip();
        bool IsNoClipActive() const;


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

        // Scale light-function fade distance for portable lights
        // 1.0 = default game fade distance
        void SetPortableLightFadeDistanceScale(float scale);
        float GetPortableLightFadeDistanceScale() const;

        // Toggle anomaly spawning disabled -- destroys existing anomalies and stops new ones
        void ToggleAnomaliesDisabled();
        bool IsAnomaliesDisabledActive() const;
        // Apply anomaly suppression each tick (destroys new anomalies if cheat is on)
        void ApplyAnomalySuppression(SDK::UWorld* world);

        // Toggle automag (magazines placed in mag pouches auto-refill)
        void ToggleAutoMag();
        bool IsAutoMagActive() const;
        void SetAutoMag(bool enabled);

        // Spawn a QuickHeal injector item near the player
        std::string SpawnHealItem(SDK::UWorld* world);

        // Environment debug cheats
        std::string RemovePlantsBushesOnce(SDK::UWorld* world, float radius);
        std::string RemoveTreesOnce(SDK::UWorld* world, float radius);
        std::string TogglePlantsBushesPersistent(float radius, float intervalSeconds);
        std::string ToggleTreesPersistent(float radius, float intervalSeconds);

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

        static constexpr float kDefaultEnvironmentPruneRadius = 4000.0f;
        static constexpr float kDefaultEnvironmentPruneIntervalSeconds = 15.0f;

    private:
        struct PruneInstancesTarget
        {
            SDK::UInstancedStaticMeshComponent* component{nullptr};
            std::vector<int32_t> instanceIndices;
            std::string meshName;
            std::string ownerName;
        };

        struct PendingPruneBatch
        {
            EnvironmentPruneCategory category{EnvironmentPruneCategory::PlantsBushes};
            EnvironmentPruneSource source{EnvironmentPruneSource::All};
            bool persistent{false};
            float radius{0.0f};
            std::chrono::steady_clock::time_point executeAt{};
            std::vector<PruneInstancesTarget> instanceTargets;
        };

        struct EnvironmentPruneRule
        {
            bool enabled{false};
            EnvironmentPruneCategory category{EnvironmentPruneCategory::All};
            EnvironmentPruneSource source{EnvironmentPruneSource::All};
            float radius{kDefaultEnvironmentPruneRadius};
            float intervalSeconds{kDefaultEnvironmentPruneIntervalSeconds};
            std::chrono::steady_clock::time_point nextCollectAt{};
        };

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
        std::atomic<bool> debugModeActive_{false};
        std::atomic<bool> anomaliesDisabled_{false};
        std::atomic<bool> autoMagActive_{false};

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
        std::unordered_map<SDK::ULightComponentBase*, float> portableLightOriginalFadeDistance_;
        std::atomic<float> portableLightIntensityScale_{1.0f};
        std::atomic<float> portableLightFadeDistanceScale_{1.0f};
        std::atomic<bool> portableLightScaleDirty_{true};
        SDK::UWorld* lastPortableLightWorld_{nullptr};

        SDK::UWorld* lastBulletTimeWorld_{nullptr};
        SDK::ABP_RadiusPlayerCharacter_Gameplay_C* lastBulletTimePlayer_{nullptr};
        std::chrono::steady_clock::time_point nextAnomalySuppressionAt_{};
        uint32_t portableLightLogCounter_{0};

        // Environment pruning state
        EnvironmentPruneRule plantsBushesRule_{};
        EnvironmentPruneRule treesRule_{};
        std::vector<PendingPruneBatch> pendingEnvironmentPruneBatches_{};
        SDK::UWorld* lastEnvironmentPruneWorld_{nullptr};

        const char* CategoryToLabel(EnvironmentPruneCategory category) const;
        const char* SourceToLabel(EnvironmentPruneSource source) const;
        bool MatchesEnvironmentCategory(EnvironmentPruneCategory category, const std::string& name) const;
        bool MatchesEnvironmentSource(EnvironmentPruneSource source, const SDK::UInstancedStaticMeshComponent* component) const;
        bool IsWithinRadius(const SDK::FVector& a, const SDK::FVector& b, float radius) const;

        std::string ToggleEnvironmentPersistent(EnvironmentPruneRule& rule, float radius, float intervalSeconds);
        std::string QueueEnvironmentPruneNow(
            SDK::UWorld* world,
            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player,
            EnvironmentPruneCategory category,
            EnvironmentPruneSource source,
            float radius,
            bool persistent);

        void CollectEnvironmentPruneTargets(
            SDK::UWorld* world,
            SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player,
            EnvironmentPruneCategory category,
            EnvironmentPruneSource source,
            float radius,
            PendingPruneBatch& batch);

        void UpdateEnvironmentPrune(SDK::UWorld* world, SDK::ABP_RadiusPlayerCharacter_Gameplay_C* player);
        void ExecuteDueEnvironmentPruneBatches();
    };
}
