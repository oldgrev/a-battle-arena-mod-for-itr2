#pragma once

/*
AILEARNINGS:
- ABPA_Scope_C has SceneCaptureComponent2D at 0x0840, LensMesh at 0x0878, LensMatInst at 0x0858
- The scope's SceneCaptureComponent2D has PostProcessSettings at 0x0340 and PostProcessBlendWeight at 0x0A60
- bCaptureEveryFrame is at USceneCaptureComponent offset 0x0232 bit 0
- Scope NVG works by injecting green-tint PP into the scope's EXISTING capture component
- No new lens/mesh/camera creation needed — just PP modification on the scope's own capture
- GetAllPlayerItems() returns all ARadiusItemBase* on the player, which can be IsA-checked for ABPA_Scope_C
- ARadiusContainerSubsystem is a world subsystem retrieved via GetWorldSubsystem()
*/

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>

#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    /**
     * Information about a discovered scope on the player.
     */
    struct ScopeInfo
    {
        SDK::ABPA_Scope_C* actor{nullptr};       // The scope actor
        std::string name;                          // Short display name (e.g., "Magnifier")
        std::string className;                     // Full class name for logging
        bool nvgEnabled{false};                    // Whether NVG PP is active on this scope

        // Backup of original state (so we can restore cleanly)
        bool hadBackup{false};
        float origPostProcessBlendWeight{0.0f};
        float origExposureBias{0.0f};
        float origBrightnessMult{1.0f};            // M_Lens BrightnessMult backup
        bool origCaptureEveryFrame{false};          // bCaptureEveryFrame backup
        // Track whether we flipped a PostProcessing ShowFlag from false→true
        bool flippedPPShowFlag{false};
        
        // Pointer validity check — store the address of the capture component
        // so we can detect if it got recycled or destroyed
        uintptr_t captureCompAddr{0};
    };

    /**
     * Manages NVG post-processing injection into weapon scope capture cameras.
     *
     * Unlike the full NVG lens system, this does NOT create new cameras, meshes, or render
     * targets. It modifies the scope's own SceneCaptureComponent2D PostProcessSettings to
     * apply a green-tinted NVG look through the existing scope pipeline.
     *
     * Usage:
     *   1. ScanScopes(world) — discovers all scopes on the player
     *   2. ToggleScopeNVG(index) — enable/disable NVG on a specific scope
     *   3. Update(world) — called every tick to keep PP settings applied
     */
    class ScopeNVGSubsystem
    {
    public:
        static ScopeNVGSubsystem& Get();

        // --- Core API ---
        
        /// Scan player's inventory for scopes. Returns a human-readable report.
        std::string ScanScopes(SDK::UWorld* world);

        /// Enable NVG post-processing on a specific scope (by index from last scan).
        std::string EnableScopeNVG(int scopeIndex);

        /// Disable NVG post-processing on a specific scope, restoring original PP.
        std::string DisableScopeNVG(int scopeIndex);

        /// Toggle NVG on a specific scope.
        std::string ToggleScopeNVG(int scopeIndex);

        /// Enable NVG on ALL discovered scopes.
        std::string EnableAll();

        /// Disable NVG on ALL discovered scopes.
        std::string DisableAll();

        /// Called every tick. Keeps NVG PP applied on enabled scopes and validates pointers.
        void Update(SDK::UWorld* world);

        // --- Getters for menu ---
        
        /// Get the list of discovered scopes (from last scan).
        const std::vector<ScopeInfo>& GetScopes() const { return scopes_; }

        /// Number of discovered scopes.
        int GetScopeCount() const { return static_cast<int>(scopes_.size()); }

        /// Whether any scope has NVG enabled.
        bool AnyNVGEnabled() const;

        /// Get a diagnostic/status report.
        std::string GetStatusReport() const;

        // --- NVG PP tuning params ---
        float GetGreenGain() const { return greenGain_; }
        void SetGreenGain(float v) { greenGain_ = v; }
        float GetExposureBias() const { return exposureBias_; }
        void SetExposureBias(float v) { exposureBias_ = v; }
        float GetGrainIntensity() const { return grainIntensity_; }
        void SetGrainIntensity(float v) { grainIntensity_ = v; }
        float GetBloomIntensity() const { return bloomIntensity_; }
        void SetBloomIntensity(float v) { bloomIntensity_ = v; }

        /// Whether auto-scan is enabled (re-scans on each Update tick).
        bool IsAutoScanEnabled() const { return autoScan_.load(std::memory_order_relaxed); }
        void SetAutoScan(bool v) { autoScan_.store(v, std::memory_order_relaxed); }

        /// Trigger a scan (called from menu page on open).
        void RequestScanOnNextUpdate() { scanRequested_.store(true, std::memory_order_relaxed); }

    private:
        ScopeNVGSubsystem() = default;

        // --- Approach A: PP + ShowFlags + forced CaptureScene ---
        void ApplyApproachA_PostProcess(ScopeInfo& scope);
        void RestoreApproachA_PostProcess(ScopeInfo& scope);

        // --- Approach B: LensMatInst MID param tweaks (BrightnessMult, GridColor) ---
        void ApplyApproachB_MaterialParams(ScopeInfo& scope);
        void RestoreApproachB_MaterialParams(ScopeInfo& scope);

        // --- Approach C: bCaptureEveryFrame + force CaptureScene ---
        void ApplyApproachC_ForcedCapture(ScopeInfo& scope);
        void RestoreApproachC_ForcedCapture(ScopeInfo& scope);

        // Combined apply/restore (runs all approaches)
        void ApplyAllApproaches(ScopeInfo& scope);
        void RestoreAllApproaches(ScopeInfo& scope);

        /// Backup original state from a scope's capture component and material.
        void BackupScopeState(ScopeInfo& scope);

        /// Validate a scope actor pointer is still alive.
        bool IsScopeValid(const ScopeInfo& scope) const;

        /// Get the scope's SceneCaptureComponent2D (convenience).
        SDK::USceneCaptureComponent2D* GetCaptureComponent(SDK::ABPA_Scope_C* scope) const;

        /// Log before/after values of a capture component for diagnostics.
        void LogCaptureState(const std::string& prefix, SDK::USceneCaptureComponent2D* cc, const ScopeInfo& scope);

        // --- State ---
        std::vector<ScopeInfo> scopes_;
        mutable std::mutex mutex_;

        // NVG PP tuning
        float greenGain_{1.5f};
        float exposureBias_{1.5f};
        float grainIntensity_{0.3f};
        float bloomIntensity_{0.8f};

        // Auto-scan: off by default, triggered on menu page open
        std::atomic<bool> autoScan_{false};
        std::atomic<bool> scanRequested_{false};
        int tickCounter_{0};
        static constexpr int kScanInterval = 120;  // Re-scan interval when autoScan is on
    };

} // namespace Mod
