#pragma once

/*
AILEARNINGS
- NVG subsystem: Simplified to single camera-PP approach with game NVG blendable discovery.
- Approach A (UMobilePostProcessSubsystem): REMOVED. Did not work on PC at all.
- Approach C (SetNV ProcessEvent): REMOVED as standalone. Its mechanism is now studied
  via ProbeGameNVG() to discover blendable materials used by the game's own NVG system.
- The game uses SwitchPPElement(On, Index) which adds/removes FWeightedBlendable entries
  to the camera's PostProcessSettings.WeightedBlendables array. SetNV calls this internally.
- ABP_PlayerPostProcessMesh_C (TidePPMesh) is an AStaticMeshActor spawned in front of the
  camera for post-process effects. It may cause right-eye-only issues in VR stereo.
- Fullscreen mode: camera PP only (subtle green tint + shadow lift + moderate exposure boost).
- Lens modes: probe game NVG to discover blendable material, then apply it directly to camera
  PP WeightedBlendables. Fall back to camera PP with diagnostic logging if discovery fails.
- PP effect tuning: uses shadow-specific color grading (ColorGammaShadows, ColorGainShadows)
  and tone mapping (FilmToe) to lift dark areas without blowing out highlights. No massive EV
  bias. Moderate auto-exposure boost (+1 to +3 EV).
- Bitfield members (bOverride_*) must be set individually via assignment, not memset.
*/

#include <atomic>
#include <string>
#include <vector>
#include "..\CppSDK\SDK.hpp"

namespace Mod
{
    // NVG display mode
    enum class NVGMode : int
    {
        Fullscreen = 0,      // NVG covers entire VR view via camera PP (both eyes)
        LensBlackout = 1,    // Camera PP (both eyes) + MI_PP_NightVision weight (may be one eye)
        LensOverlay = 2,     // Mesh-based lens: NVG inside circular lens, normal outside (BOTH eyes)
        LensMeshBlackout = 3,// Mesh-based lens: NVG inside lens, blacked out outside (BOTH eyes)
        GameNVGOnly = 4,     // Pure SetNV delegation — no camera PP, isolated stereo test
        COUNT = 5,
    };

    class NVGSubsystem
    {
    public:
        static NVGSubsystem& Get();

        // Toggle NVG on/off
        void Toggle();

        // Turn NVG explicitly on or off
        void SetEnabled(bool enabled);
        bool IsEnabled() const { return enabled_.load(std::memory_order_relaxed); }

        // Set the NVG display mode
        void SetMode(NVGMode mode);
        NVGMode GetMode() const { return mode_.load(std::memory_order_relaxed); }

        // Intensity: overall NVG brightness multiplier (0.1 - 10.0, default 1.0)
        void SetIntensity(float intensity);
        float GetIntensity() const { return intensity_.load(std::memory_order_relaxed); }

        // Grain: film grain / noise amount (0.0 - 5.0, default 0.3)
        void SetGrain(float grain);
        float GetGrain() const { return grain_.load(std::memory_order_relaxed); }

        // Bloom: bloom intensity for light sources (0.0 - 50.0, default 2.0)
        void SetBloom(float bloom);
        float GetBloom() const { return bloom_.load(std::memory_order_relaxed); }

        // Chromatic aberration / edge distortion for lens modes (0.0 - 10.0, default 1.0)
        void SetAberration(float aberration);
        float GetAberration() const { return aberration_.load(std::memory_order_relaxed); }

        // Called every frame from Cheats::Update or ModMain tick
        void Update(SDK::UWorld* world);

        // Called on level change or deactivation to fully clean up
        void Shutdown();

        // --- Diagnostic / discovery tools ---

        // Probe: call SetNV and read blendables to discover game's NVG material
        std::string ProbeGameNVG(SDK::UWorld* world);

        // Directly call SwitchPPElement on the BPC component for experimentation
        std::string ProbePPElement(SDK::UWorld* world, int index, bool on);

        // Comprehensive diagnostics
        std::string RunDiagnostics(SDK::UWorld* world);

        // Dump all WeightedBlendables on camera PP
        std::string DumpBlendables(SDK::UWorld* world);

        // Get a human-readable status string
        std::string GetStatus() const;

        // Get mode name
        static const char* ModeToString(NVGMode mode);

        // Probe status accessors (for UI)
        bool IsProbeComplete() const { return probeComplete_; }
        bool IsProbeFound() const { return probeFoundMaterial_; }

        // Direct game NVG control (wraps SetNV, bypasses camera PP)
        // Useful for isolated stereo testing from command line
        std::string EnableGameNVGDirect(SDK::UWorld* world);
        std::string DisableGameNVGDirect(SDK::UWorld* world);

        // --- Material investigation ---

        // Dump all scalar/vector/texture parameters of MI_PP_NightVision.
        // Reveals lens mask parameters and any stereo-control parameters.
        // Run AFTER nvg_probe so discoveredNVGMaterial_ is populated.
        std::string DumpMaterialParams(SDK::UWorld* world);

        // Dump MobilePostProcessSubsystem NV-relevant fields (current values)
        std::string DumpMobileSubsystem();

        // Create a UMaterialInstanceDynamic from MI_PP_NightVision and add to camera blendables.
        // Attempt: MID may have different stereo behaviour to the static MaterialInstanceConstant.
        // Requires probe to have run first to populate discoveredNVGMaterial_.
        std::string CreateAndApplyNVGMid(SDK::UWorld* world);

        // Remove the MID from camera blendables and clear stored pointer
        std::string RemoveNVGMid(SDK::UWorld* world);

        // Set a scalar parameter on the stored NVG MID (requires CreateAndApplyNVGMid first)
        // paramName: the exact material parameter name (from DumpMaterialParams)
        std::string SetMIDScalarParam(const std::string& paramName, float value);

        // Set a vector parameter on the stored NVG MID
        std::string SetMIDVectorParam(const std::string& paramName, float r, float g, float b, float a);

        // --- Lens overlay experiments (2026-03-09 batch 2) ---

        // Dump scalar/vector/texture params for ALL camera blendable materials
        std::string DumpAllBlendableParams(SDK::UWorld* world);

        // Write a float field on UMobilePostProcessSubsystem by name
        std::string WriteMobileSubsystem(const std::string& field, float value);

        // Create MID and REPLACE an existing blendable slot (fixes TArray::Add failure)
        // Default: replaces the MI_PP_NightVision slot (index 4).
        std::string MIDReplaceSlot(SDK::UWorld* world, int index);

        // Restore the original object in the MID-replaced blendable slot
        std::string MIDRestoreSlot(SDK::UWorld* world);

        // Set camera PP field directly by name (for rapid VR iteration)
        std::string SetCameraPPField(SDK::UWorld* world, const std::string& field, float value);

        // Dump current camera PP field values (NVG-relevant fields)
        std::string DumpCameraPP(SDK::UWorld* world);

        // Set weight on any blendable by index (activate/deactivate)
        std::string ActivateBlendable(SDK::UWorld* world, int index, float weight);

        // --- Mesh-based NVG Lens System ---

        // Set up the lens: spawn scene capture actor, create render target, spawn mesh, attach to camera
        std::string SetupLens(SDK::UWorld* world);

        // Tear down the lens: destroy actors, clean up render target and MID
        std::string TeardownLens(SDK::UWorld* world);

        // Update lens capture PP settings (NVG look: green tint, bloom, grain)
        void UpdateLensCaptureSettings();

        // Set lens mesh distance from camera
        void SetLensDistance(float dist);
        float GetLensDistance() const { return lensDistance_; }

        // Set lens mesh scale  
        void SetLensScale(float scale);
        float GetLensScale() const { return lensScale_; }

        // Set lens capture FOV
        void SetLensFOV(float fov);
        float GetLensFOV() const { return lensFOV_; }

        // Set lens capture render target resolution
        void SetLensResolution(int res);
        int GetLensResolution() const { return lensResolution_; }

        // Set a lens material parameter (on the lens MID)
        std::string SetLensMaterialParam(const std::string& paramName, float value);

        // Set lens mesh rotation (Pitch, Yaw, Roll in degrees)
        void SetLensRotation(float pitch, float yaw, float roll);

        // Individual rotation axis getters (degrees; normalised to [0, 360))
        float GetLensRotPitch() const { return lensRotPitch_; }
        float GetLensRotYaw()   const { return lensRotYaw_;   }
        float GetLensRotRoll()  const { return lensRotRoll_;  }

        // Rotate a single axis by +/-90 degrees (convenience helpers for mod menu buttons)
        // Each call normalises the result to [0, 360) and applies immediately if lens is active.
        void StepLensRotPitch(float deltaDeg);
        void StepLensRotYaw(float deltaDeg);
        void StepLensRotRoll(float deltaDeg);

        // Set lens material type (0=M_Lens, 1=M_Particle, 2=MI_Lens_Magnifer)
        void SetLensMatType(int type);
        int GetLensMatType() const { return lensMatType_; }

        // Set lens mesh type (0=Plane, 1=Cylinder disc)
        void SetLensMeshType(int type);
        int GetLensMeshType() const { return lensMeshType_; }

        // Set lens offset (Y=left/right, Z=up/down) relative to camera forward
        void SetLensOffset(float y, float z);
        float GetLensOffsetY() const { return lensOffsetY_; }
        float GetLensOffsetZ() const { return lensOffsetZ_; }

        // Thumbstick lens adjust mode: when active, left stick moves lens offset
        void SetLensAdjustMode(bool enabled);
        bool IsLensAdjustMode() const { return lensAdjustMode_.load(std::memory_order_relaxed); }

        // Called from HookManager when thumbstick input arrives during lens adjust mode
        void ApplyLensAdjust(float stickX, float stickY);

        // Capture camera offset (shift the NVG camera relative to the VR camera)
        void SetCaptureOffset(float x, float y, float z);
        float GetCaptureOffsetX() const { return captureOffsetX_; }
        float GetCaptureOffsetY() const { return captureOffsetY_; }
        float GetCaptureOffsetZ() const { return captureOffsetZ_; }

        // --- Render Target / Material Projection Controls ---
        // Controls how the NVG "painting" (render target) maps onto the lens mesh.
        // ImageScale: how big the painting is relative to the lens (bigger = never see edges)
        // RimScale:   radius of circular darkening border (bigger = border pushed further out)
        // RimDepth:   sharpness of circular border (0 = no border at all)
        // ImageDepth: opacity of the render target layer (very negative = fully opaque)
        // AutoFOV:    when true, capture FOV is auto-matched to lens visual angle
        void SetRTImageScale(float scale);   // 0 = auto-compute from lens geometry
        void SetRTRimScale(float scale);
        void SetRTRimDepth(float depth);
        void SetRTImageDepth(float depth);
        void SetAutoFOV(bool enabled);
        float GetRTImageScale() const { return rtImageScale_; }
        float GetRTRimScale() const { return rtRimScale_; }
        float GetRTRimDepth() const { return rtRimDepth_; }
        float GetRTImageDepth() const { return rtImageDepth_; }
        bool  GetAutoFOV() const { return autoFOV_; }
        float ComputeAutoImageScale() const;
        float ComputeAutoFOV() const;

        // Update lens material params dynamically (called each tick when lens active)
        void UpdateLensMaterialParams();

        // Check if lens system is active
        bool IsLensActive() const { return lensActive_; }

    private:
        NVGSubsystem() = default;
        ~NVGSubsystem() = default;
        NVGSubsystem(const NVGSubsystem&) = delete;
        NVGSubsystem& operator=(const NVGSubsystem&) = delete;

        // --- State ---
        std::atomic<bool> enabled_{false};
        std::atomic<NVGMode> mode_{NVGMode::Fullscreen};
        std::atomic<float> intensity_{1.1f};
        std::atomic<float> grain_{2.9f};
        std::atomic<float> bloom_{2.0f};
        std::atomic<float> aberration_{1.0f};
        std::atomic<bool> dirty_{true}; // Settings changed, need reapply

        // Track whether we've applied NVG so we can restore on disable
        bool wasApplied_{false};
        SDK::UWorld* lastWorld_{nullptr};

        // Cached original camera PP values for restoration
        struct OriginalCameraPP
        {
            bool captured{false};
            float postProcessBlendWeight{0.0f};

            // Color grading (global)
            SDK::FVector4 colorSaturation{};
            SDK::FVector4 colorGain{};
            SDK::FVector4 colorGamma{};
            SDK::FVector4 colorOffset{};
            SDK::FLinearColor sceneColorTint{};

            // Color grading (shadows)
            SDK::FVector4 colorGammaShadows{};
            SDK::FVector4 colorGainShadows{};

            // Tone mapping
            float filmSlope{0.0f};
            float filmToe{0.0f};
            float filmShoulder{0.0f};

            // Bloom
            float bloomIntensity{0.0f};
            float bloomThreshold{0.0f};

            // Vignette  
            float vignetteIntensity{0.0f};

            // Film grain
            float filmGrainIntensity{0.0f};
            float filmGrainIntensityShadows{0.0f};
            float filmGrainIntensityMidtones{0.0f};
            float filmGrainIntensityHighlights{0.0f};
            float filmGrainTexelSize{0.0f};

            // Chromatic aberration
            float sceneFringeIntensity{0.0f};
            float chromaticAberrationStartOffset{0.0f};

            // Auto exposure
            float autoExposureBias{0.0f};
            float autoExposureMinBrightness{0.0f};
            float autoExposureMaxBrightness{0.0f};
            float autoExposureSpeedUp{0.0f};
            float autoExposureSpeedDown{0.0f};
            uint8_t autoExposureMethod{0};

            // AO
            float ambientOcclusionIntensity{0.0f};

            // Motion blur
            float motionBlurAmount{0.0f};

            // Sharpening
            float sharpen{0.0f};
        };
        OriginalCameraPP originalCameraPP_{};

        // --- Game NVG discovery ---
        bool probeComplete_{false};
        bool probeFoundMaterial_{false};
        SDK::UObject* discoveredNVGMaterial_{nullptr};  // The blendable material from game's NVG
        int discoveredPPElementIndex_{-1};              // SwitchPPElement index for NVG
        bool gameNVGActive_{false};                     // Whether we've called SetNV(true)

        // --- MID investigation state ---
        // A UMaterialInstanceDynamic created from MI_PP_NightVision for parameter experiments
        SDK::UMaterialInstanceDynamic* nvgMid_{nullptr};
        bool midApplied_{false};  // Whether the MID is currently in the camera blendables

        // MID slot-replacement tracking (for MIDReplaceSlot/MIDRestoreSlot)
        SDK::UObject* replacedSlotOriginalObj_{nullptr};
        int replacedSlotIndex_{-1};

        // --- Mesh-based NVG Lens System ---
        SDK::AActor* lensCaptureActor_{nullptr};           // ASceneCapture2D actor
        SDK::AActor* lensMeshActor_{nullptr};              // AStaticMeshActor for the lens mesh (left eye)
        SDK::AActor* lensMeshActor2_{nullptr};             // AStaticMeshActor for the second lens mesh (right eye, mirrored Y)
        SDK::UTextureRenderTarget2D* lensRenderTarget_{nullptr};
        SDK::UMaterialInstanceDynamic* lensMID_{nullptr};  // Dynamic material for lens mesh
        bool lensActive_{false};
        bool lensSetupFailed_{false};  // Prevents infinite retry when SetupLens fails
        bool inUpdate_{false};         // Re-entrancy guard for Update()
        float lensFOV_{53.0f};       // Capture FOV (degrees) (user-tested: 53 is best)
        float lensScale_{0.05f};     // Mesh scale factor (user-tested: 0.05 is best)
        float lensDistance_{5.0f};    // Mesh distance from camera (user-tested: 5.0 is best)
        int lensResolution_{1024};    // Render target resolution (square)

        // Mesh rotation (user-tested working rotation: P270/Y180/R270 for cylinder, verified 2026-03-11)
        float lensRotPitch_{270.0f};
        float lensRotYaw_{180.0f};
        float lensRotRoll_{270.0f};
        int lensDiagCounter_{0};  // Throttle diagnostic logging

        // Lens material type: 0=M_Lens (circular scope material), 1=M_Particle (simple translucent), 2=MI_Lens_Magnifer
        int lensMatType_{2};  // Default to MI_Magnifer (user-tested: best material)

        // Lens mesh type: 0=Plane (square), 1=Cylinder (disc/circle)
        int lensMeshType_{1};  // Default to cylinder for circular shape

        // Lens offset from camera forward axis (Y=right, Z=up) in cm
        // Left lens at lensOffsetY_, right lens at -lensOffsetY_
        float lensOffsetY_{-3.0f};  // User-tested: -3.0 for left eye, +3.0 (mirrored) for right
        float lensOffsetZ_{0.0f};

        // Thumbstick lens positioning mode
        std::atomic<bool> lensAdjustMode_{false};
        float lensAdjustSpeed_{0.5f};  // cm per tick at full stick deflection

        // Capture camera offset (shift NVG camera position relative to VR camera)
        float captureOffsetX_{0.0f};  // Forward/backward
        float captureOffsetY_{0.0f};  // Left/right
        float captureOffsetZ_{0.0f};  // Up/down

        // NVG PP settings applied to capture component (reduced from initial to fix washed out)
        float lensNvgGreenGain_{1.5f};
        float lensNvgBloomIntensity_{0.8f};
        float lensNvgGrainIntensity_{0.1f};
        float lensNvgExposureBias_{1.0f};

        // --- Render target / material projection params ---
        // These control how the "painting" (render target) maps onto the lens mesh.
        // The dark edge encroachment was caused by the M_Lens material's circular rim mask
        // and/or ImageScale being insufficient for the NVG lens geometry+stereo parallax.
        float rtImageScale_{25.3f};   // MID "ImageScale": 0 = auto-compute; user-tested: 25.3
        float rtRimScale_{50.0f};     // MID "Rim Scale": very large to push rim beyond visible area
        float rtRimDepth_{0.0f};      // MID "RimDepth": 0 = no rim mask (ExponentialDensity(0)=0)
        float rtImageDepth_{-500.0f}; // MID "ImageDepth": very negative = fully opaque everywhere
        bool  autoFOV_{false};        // When true, capture FOV auto-matches lens visual angle

        // Components hidden from scene capture (to restore on teardown)
        std::vector<uintptr_t> hiddenFromCaptureComps_;

        // --- Helpers ---
        SDK::UBPC_PlayerPostProcess_C* FindPlayerPostProcessComponent(SDK::UWorld* world);
        SDK::UReplicatedVRCameraComponent* FindPlayerCamera(SDK::UWorld* world);

        // Apply/remove camera PP effect (fullscreen green tint + shadow lift)
        void ApplyCameraPP(SDK::UWorld* world);
        void RemoveCameraPP(SDK::UWorld* world);

        // Apply/remove lens masking (game's NVG blendable or fallback)
        void ApplyLensMask(SDK::UWorld* world);
        void RemoveLensMask(SDK::UWorld* world);

        // Apply/remove the game's own SetNV (used by GameNVGOnly mode and direct commands)
        void ApplyGameNVG(SDK::UWorld* world);
        void RemoveGameNVG(SDK::UWorld* world);

        // Read WeightedBlendables from camera PP and return as string
        std::string ReadBlendables(SDK::UReplicatedVRCameraComponent* camera);

        int logCounter_{0};
    };
}
