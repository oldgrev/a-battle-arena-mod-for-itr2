/*
AILEARNINGS
- NVG subsystem: Now supports 5 modes including mesh-based NVG lens rendering.
- STEREO BUG ROOT CAUSE (CONFIRMED 2026-03-10): MI_PP_NightVision material uses BitMask (x2)
  and DynamicBranch (x2) in the compiled shader to check StereoPassIndex. This is BAKED INTO
  the shader and cannot be overridden via any material parameter. The NVG post-process effect
  only renders on the RIGHT eye. This is why all MID/blendable approaches failed for stereo.

- MESH-BASED LENS SOLUTION (2026-03-10): Modeled after the game's scope system (BPA_Scope).
  Architecture: ASceneCapture2D → TextureRenderTarget2D → MID(M_Lens) → AStaticMeshActor
  attached to VR camera. Because the mesh is a world-space object (not a post-process), UE
  renders it for BOTH eyes. The NVG look is applied via PostProcessSettings on the capture
  component (green tint, bloom, grain, exposure), NOT via the broken MI_PP_NightVision shader.

- KEY GAME MATERIAL ANALYSIS (from unpacked pak JSON exports):
  * M_PP_NightVision: 7 scalar params (NVNoise, NVMult, MaskScale, Param, blur_base, Blur_size,
    LaserBrightness). Uses BitMask+DynamicBranch = stereo check. UNFIXABLE for both-eye.
  * M_Lens: Unlit mesh material (NOT post-process). Texture param RT_Scope defaults to RT_NVG.
    Uses RadialGradientExponential for circular rim effect. Params: Rim Scale, RimDepth,
    GridScale, ImageScale, BrightnessMult. bHasVertexInterpolator=true.
  * RT_NVG: 512x512 TextureRenderTarget2D, non-SRGB, Clamp addressing.
  * BPA_Scope setup: SceneCaptureComponent2D → CreateRenderTarget2D → CreateDynamicMaterialInstance
    from MI_Lens_PSO → set RT_Scope param → apply to LensMesh. FOV 2.0 (narrow for scope).

- ASSET PATHS:
  * M_Lens: /Game/ITR1/ART/Weapons/PSO/M_Lens
  * MI_Lens_PSO: parent is M_Lens at /Game/ITR1/ART/Weapons/PSO/M_Lens
  * RT_NVG: /Game/ITR1/ART/Weapons/PSO/RT_NVG
  * SM_PVS14_Tube: /Game/ITR2/Art/Items/Equipment/PVS14/SM_PVS14_Tube
  * 1M_Cube: /Game/_VRE/Misc/Meshes/1M_Cube

- NVG MODES:
  * 0 Fullscreen: Camera PP only (green tint, bloom, grain via PostProcessSettings) — both eyes
  * 1 LensBlackout: Camera PP + MI_PP_NightVision blendable weight (right eye only — legacy)
  * 2 LensOverlay: Mesh-based lens with NVG capture, normal view outside — BOTH eyes
  * 3 LensMeshBlackout: Same as LensOverlay + dark camera PP to black out outside lens
  * 4 GameNVGOnly: Pure SetNV delegation, no camera PP — for diagnosis/comparison

- Approach A (UMobilePostProcessSubsystem): REMOVED. Did not work on PC.
  The game's BPC_PlayerPostProcess checks IsMobilePlatform() before writing to the subsystem.
- Approach C (SetNV ProcessEvent): Mechanism studied via ProbeGameNVG(). Sets weight on
  MI_PP_NightVision in camera blendables. But stereo check in shader makes it right-eye-only.

- FPostProcessSettings KEY OFFSETS:
  * ColorSaturation at 0x0040, ColorGain at 0x00A0, SceneColorTint at 0x02EC
  * BloomIntensity at 0x0304, AutoExposureBias at 0x049C, VignetteIntensity at 0x05C8
  * UCameraComponent: PostProcessSettings at 0x0300, PostProcessBlendWeight at 0x02D0

- KEY SDK TYPES FOR LENS:
  * ASceneCapture2D: CaptureComponent2D at 0x02B8 (USceneCaptureComponent2D*)
  * USceneCaptureComponent: bCaptureEveryFrame at 0x0232 bit 0, bCaptureOnMovement bit 1
  * USceneCaptureComponent2D: FOVAngle at 0x030C, TextureTarget at 0x0328,
    PostProcessSettings at 0x0340, PostProcessBlendWeight at 0x0A60
  * AStaticMeshActor: StaticMeshComponent at 0x02A8
  * UKismetRenderingLibrary::CreateRenderTarget2D / ReleaseRenderTarget2D
  * UKismetMaterialLibrary::CreateDynamicMaterialInstance
  * UMaterialInstanceDynamic::SetTextureParameterValue / SetScalarParameterValue
  * UPrimitiveComponent::SetMaterial / SetCollisionEnabled
  * AActor::K2_AttachToComponent / K2_SetActorRelativeLocation / K2_DestroyActor

- PP v3: shadow-specific grading + FilmToe + moderate EV. No massive bias.
- Bitfield members (bOverride_*) must be set individually via assignment, not memset.
- TArray::Add on camera blendables silently fails (fixed-size array). Use in-place replacement.
*/

#include "NVGSubsystem.hpp"
#include "HookManager.hpp"
#include "GameContext.hpp"
#include "Logging.hpp"
#include "ModFeedback.hpp"

#include <sstream>
#include <cmath>
#include <iomanip>

namespace Mod
{
    NVGSubsystem& NVGSubsystem::Get()
    {
        static NVGSubsystem instance;
        return instance;
    }

    const char* NVGSubsystem::ModeToString(NVGMode mode)
    {
        switch (mode)
        {
        case NVGMode::Fullscreen:       return "Fullscreen";
        case NVGMode::LensBlackout:     return "LensBlackout";
        case NVGMode::LensOverlay:      return "LensOverlay";
        case NVGMode::LensMeshBlackout: return "LensMeshBlackout";
        case NVGMode::GameNVGOnly:      return "GameNVGOnly";
        default:                        return "Unknown";
        }
    }

    void NVGSubsystem::Toggle()
    {
        bool current = enabled_.load(std::memory_order_relaxed);
        bool next = !current;
        enabled_.store(next, std::memory_order_relaxed);
        dirty_.store(true, std::memory_order_release);

        LOG_INFO("[NVG] Toggle -> " << (next ? "ON" : "OFF")
            << " mode=" << ModeToString(mode_.load())
            << " intensity=" << intensity_.load());
    }

    void NVGSubsystem::SetEnabled(bool enabled)
    {
        if (enabled_.load(std::memory_order_relaxed) != enabled)
        {
            enabled_.store(enabled, std::memory_order_relaxed);
            dirty_.store(true, std::memory_order_release);
            LOG_INFO("[NVG] SetEnabled -> " << (enabled ? "ON" : "OFF"));
        }
    }

    void NVGSubsystem::SetMode(NVGMode mode)
    {
        if (static_cast<int>(mode) < 0 || static_cast<int>(mode) >= static_cast<int>(NVGMode::COUNT))
            mode = NVGMode::Fullscreen;
        mode_.store(mode, std::memory_order_relaxed);
        lensSetupFailed_ = false;  // Allow retry on mode change
        dirty_.store(true, std::memory_order_release);
        LOG_INFO("[NVG] SetMode -> " << ModeToString(mode));
    }

    void NVGSubsystem::SetIntensity(float intensity)
    {
        if (!std::isfinite(intensity)) intensity = 1.0f;
        if (intensity < 0.001f) intensity = 0.001f;
        if (intensity > 20.0f) intensity = 20.0f;
        intensity_.store(intensity, std::memory_order_relaxed);
        // NOT setting dirty_ — UpdateLensCaptureSettings applies this each tick without rebuild
        LOG_INFO("[NVG] SetIntensity -> " << intensity);
    }

    void NVGSubsystem::SetGrain(float grain)
    {
        if (!std::isfinite(grain)) grain = 0.3f;
        if (grain < 0.0f) grain = 0.0f;
        if (grain > 5.0f) grain = 5.0f;
        grain_.store(grain, std::memory_order_relaxed);
        // NOT setting dirty_ — UpdateLensCaptureSettings applies this each tick without rebuild
        LOG_INFO("[NVG] SetGrain -> " << grain);
    }

    void NVGSubsystem::SetBloom(float bloom)
    {
        if (!std::isfinite(bloom)) bloom = 2.0f;
        if (bloom < 0.0f) bloom = 0.0f;
        if (bloom > 50.0f) bloom = 50.0f;
        bloom_.store(bloom, std::memory_order_relaxed);
        // NOT setting dirty_ — UpdateLensCaptureSettings applies this each tick without rebuild
        LOG_INFO("[NVG] SetBloom -> " << bloom);
    }

    void NVGSubsystem::SetAberration(float aberration)
    {
        if (!std::isfinite(aberration)) aberration = 1.0f;
        if (aberration < 0.0f) aberration = 0.0f;
        if (aberration > 10.0f) aberration = 10.0f;
        aberration_.store(aberration, std::memory_order_relaxed);
        // NOT setting dirty_ — UpdateLensCaptureSettings applies this each tick without rebuild
        LOG_INFO("[NVG] SetAberration -> " << aberration);
    }

    std::string NVGSubsystem::GetStatus() const
    {
        std::ostringstream oss;
        oss << "NVG: " << (enabled_.load() ? "ON" : "OFF")
            << " | mode=" << ModeToString(mode_.load())
            << " | intensity=" << intensity_.load()
            << " | grain=" << grain_.load()
            << " | bloom=" << bloom_.load()
            << " | aberration=" << aberration_.load()
            << " | probe=" << (probeComplete_ ? (probeFoundMaterial_ ? "FOUND" : "NOT_FOUND") : "PENDING")
            << " | gameNVG=" << (gameNVGActive_ ? "ACTIVE" : "OFF")
            << " | lens=" << (lensActive_ ? "ACTIVE" : "OFF");
        if (lensActive_)
        {
            float effectiveImgScale = rtImageScale_ > 0.01f ? rtImageScale_ : ComputeAutoImageScale();
            oss << " fov=" << lensFOV_
                << " scale=" << lensScale_
                << " dist=" << lensDistance_
                << " rot=P" << lensRotPitch_ << "/Y" << lensRotYaw_ << "/R" << lensRotRoll_
                << " res=" << lensResolution_
                << " imgScale=" << effectiveImgScale << (rtImageScale_ <= 0.01f ? "(auto)" : "")
                << " rimScale=" << rtRimScale_
                << " rimDepth=" << rtRimDepth_
                << " imgDepth=" << rtImageDepth_
                << " autoFOV=" << (autoFOV_ ? "ON" : "OFF");
        }
        if (lensSetupFailed_)
        {
            oss << " | LENS_SETUP_FAILED";
        }
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // Helper: find the UBPC_PlayerPostProcess_C on the player
    // -----------------------------------------------------------------------
    SDK::UBPC_PlayerPostProcess_C* NVGSubsystem::FindPlayerPostProcessComponent(SDK::UWorld* world)
    {
        if (!world) return nullptr;

        auto* player = GameContext::GetPlayerCharacter();
        if (!player || !SDK::UKismetSystemLibrary::IsValid(player))
        {
            if (logCounter_++ % 300 == 0)
                LOG_WARN("[NVG] FindPlayerPostProcessComponent: player not found");
            return nullptr;
        }

        auto* ppComp = player->BPC_PlayerPostProcess;
        if (ppComp && SDK::UKismetSystemLibrary::IsValid(ppComp))
            return ppComp;

        // Fallback: GetComponentByClass
        LOG_INFO("[NVG] BPC_PlayerPostProcess direct member is null, trying GetComponentByClass");
        {
            Mod::ScopedProcessEventGuard guard;
            auto* comp = player->GetComponentByClass(SDK::UBPC_PlayerPostProcess_C::StaticClass());
            if (comp && SDK::UKismetSystemLibrary::IsValid(comp))
            {
                LOG_INFO("[NVG] Found BPC_PlayerPostProcess_C via GetComponentByClass");
                return static_cast<SDK::UBPC_PlayerPostProcess_C*>(comp);
            }
        }

        if (logCounter_++ % 300 == 0)
            LOG_WARN("[NVG] Failed to find BPC_PlayerPostProcess_C on player");
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Helper: find the VR camera component
    // -----------------------------------------------------------------------
    SDK::UReplicatedVRCameraComponent* NVGSubsystem::FindPlayerCamera(SDK::UWorld* world)
    {
        if (!world) return nullptr;

        auto* player = GameContext::GetPlayerCharacter();
        if (!player || !SDK::UKismetSystemLibrary::IsValid(player))
        {
            if (logCounter_++ % 300 == 0)
                LOG_WARN("[NVG] FindPlayerCamera: player not found");
            return nullptr;
        }

        auto* vrCam = player->VRReplicatedCamera;
        if (vrCam && SDK::UKismetSystemLibrary::IsValid(vrCam))
        {
            if (logCounter_ < 5)
            {
                LOG_INFO("[NVG] Found VRReplicatedCamera: " << vrCam->GetName());
                logCounter_++;
            }
            return vrCam;
        }

        // Fallback
        LOG_INFO("[NVG] VRReplicatedCamera direct member is null, trying GetComponentByClass");
        {
            Mod::ScopedProcessEventGuard guard;
            auto* comp = player->GetComponentByClass(SDK::UReplicatedVRCameraComponent::StaticClass());
            if (comp && SDK::UKismetSystemLibrary::IsValid(comp))
            {
                LOG_INFO("[NVG] Found VR camera via GetComponentByClass: " << comp->GetName());
                return static_cast<SDK::UReplicatedVRCameraComponent*>(comp);
            }
        }

        if (logCounter_++ % 300 == 0)
            LOG_WARN("[NVG] Failed to find VR camera on player");
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Read WeightedBlendables from camera PP
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::ReadBlendables(SDK::UReplicatedVRCameraComponent* camera)
    {
        if (!camera) return "(null camera)";

        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        std::ostringstream oss;
        int count = wb.Array.Num();
        oss << "WeightedBlendables[" << count << "]: ";
        for (int i = 0; i < count; i++)
        {
            auto& entry = wb.Array[i];
            oss << "\n  [" << i << "] weight=" << entry.Weight
                << " obj=" << (entry.Object ? "0x" : "null");
            if (entry.Object)
            {
                // Log the pointer address and try to get the class name
                oss << std::hex << reinterpret_cast<uintptr_t>(entry.Object) << std::dec;
                try
                {
                    if (SDK::UKismetSystemLibrary::IsValid(entry.Object))
                    {
                        oss << " class=" << entry.Object->Class->GetName()
                            << " name=" << entry.Object->GetName();
                    }
                    else
                    {
                        oss << " (INVALID)";
                    }
                }
                catch (...)
                {
                    oss << " (exception reading class)";
                }
            }
        }
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // Camera PP: Apply subtle NVG post-process effect
    // -----------------------------------------------------------------------
    void NVGSubsystem::ApplyCameraPP(SDK::UWorld* world)
    {
        auto* camera = FindPlayerCamera(world);
        if (!camera)
        {
            if (logCounter_++ % 300 == 0)
                LOG_WARN("[NVG] Cannot apply camera PP: VR camera not found");
            return;
        }

        auto& pp = camera->PostProcessSettings;
        const float intensity = intensity_.load(std::memory_order_relaxed);
        const float grain = grain_.load(std::memory_order_relaxed);
        const float bloom = bloom_.load(std::memory_order_relaxed);
        const float aberration = aberration_.load(std::memory_order_relaxed);
        const NVGMode mode = mode_.load(std::memory_order_relaxed);

        // Capture originals on first apply
        if (!originalCameraPP_.captured)
        {
            originalCameraPP_.postProcessBlendWeight = camera->PostProcessBlendWeight;

            originalCameraPP_.colorSaturation = pp.ColorSaturation;
            originalCameraPP_.colorGain = pp.ColorGain;
            originalCameraPP_.colorGamma = pp.ColorGamma;
            originalCameraPP_.colorOffset = pp.ColorOffset;
            originalCameraPP_.sceneColorTint = pp.SceneColorTint;
            originalCameraPP_.colorGammaShadows = pp.ColorGammaShadows;
            originalCameraPP_.colorGainShadows = pp.ColorGainShadows;

            originalCameraPP_.filmSlope = pp.FilmSlope;
            originalCameraPP_.filmToe = pp.FilmToe;
            originalCameraPP_.filmShoulder = pp.FilmShoulder;

            originalCameraPP_.bloomIntensity = pp.BloomIntensity;
            originalCameraPP_.bloomThreshold = pp.BloomThreshold;
            originalCameraPP_.vignetteIntensity = pp.VignetteIntensity;

            originalCameraPP_.filmGrainIntensity = pp.FilmGrainIntensity;
            originalCameraPP_.filmGrainIntensityShadows = pp.FilmGrainIntensityShadows;
            originalCameraPP_.filmGrainIntensityMidtones = pp.FilmGrainIntensityMidtones;
            originalCameraPP_.filmGrainIntensityHighlights = pp.FilmGrainIntensityHighlights;
            originalCameraPP_.filmGrainTexelSize = pp.FilmGrainTexelSize;

            originalCameraPP_.sceneFringeIntensity = pp.SceneFringeIntensity;
            originalCameraPP_.chromaticAberrationStartOffset = pp.ChromaticAberrationStartOffset;

            originalCameraPP_.autoExposureBias = pp.AutoExposureBias;
            originalCameraPP_.autoExposureMinBrightness = pp.AutoExposureMinBrightness;
            originalCameraPP_.autoExposureMaxBrightness = pp.AutoExposureMaxBrightness;
            originalCameraPP_.autoExposureSpeedUp = pp.AutoExposureSpeedUp;
            originalCameraPP_.autoExposureSpeedDown = pp.AutoExposureSpeedDown;
            originalCameraPP_.autoExposureMethod = static_cast<uint8_t>(pp.AutoExposureMethod);

            originalCameraPP_.ambientOcclusionIntensity = pp.AmbientOcclusionIntensity;
            originalCameraPP_.motionBlurAmount = pp.MotionBlurAmount;
            originalCameraPP_.sharpen = pp.Sharpen;

            originalCameraPP_.captured = true;
            LOG_INFO("[NVG] Captured original camera PP values");
        }

        // Enable the camera's post-process blend
        camera->PostProcessBlendWeight = 1.0f;

        // =====================================================================
        // Enable override flags
        // =====================================================================
        pp.bOverride_ColorSaturation = 1;
        pp.bOverride_ColorGain = 1;
        pp.bOverride_ColorGamma = 1;
        pp.bOverride_ColorOffset = 1;
        pp.bOverride_SceneColorTint = 1;
        pp.bOverride_ColorGammaShadows = 1;
        pp.bOverride_ColorGainShadows = 1;
        pp.bOverride_FilmSlope = 1;
        pp.bOverride_FilmToe = 1;
        pp.bOverride_FilmShoulder = 1;
        pp.bOverride_BloomIntensity = 1;
        pp.bOverride_BloomThreshold = 1;
        pp.bOverride_VignetteIntensity = 1;
        pp.bOverride_FilmGrainIntensity = 1;
        pp.bOverride_FilmGrainIntensityShadows = 1;
        pp.bOverride_FilmGrainIntensityMidtones = 1;
        pp.bOverride_FilmGrainIntensityHighlights = 1;
        pp.bOverride_FilmGrainTexelSize = 1;
        pp.bOverride_SceneFringeIntensity = 1;
        pp.bOverride_ChromaticAberrationStartOffset = 1;
        pp.bOverride_AutoExposureBias = 1;
        pp.bOverride_AutoExposureMinBrightness = 1;
        pp.bOverride_AutoExposureMaxBrightness = 1;
        pp.bOverride_AutoExposureMethod = 1;
        pp.bOverride_AutoExposureSpeedUp = 1;
        pp.bOverride_AutoExposureSpeedDown = 1;
        pp.bOverride_AmbientOcclusionIntensity = 1;
        pp.bOverride_MotionBlurAmount = 1;
        pp.bOverride_Sharpen = 1;

        // =====================================================================
        // TONE MAPPING: lift dark areas without blowing out highlights
        // FilmToe controls the "toe" of the curve (how darks are mapped).
        // Higher toe = brighter darks. Default is ~0.55.
        // =====================================================================
        float toeLift = 0.6f + (intensity * 0.15f);   // 0.75 at default, up to 1.35
        if (toeLift > 1.5f) toeLift = 1.5f;
        pp.FilmToe = toeLift;
        pp.FilmSlope = 0.88f;         // Keep default slope
        pp.FilmShoulder = 0.18f;      // Slightly lower shoulder = less highlight rolloff

        // =====================================================================
        // AUTO-EXPOSURE: moderate boost to brighten the scene
        // NOT massive (+10 EV). Just enough to lightly brighten darks.
        // =====================================================================
        pp.AutoExposureMethod = SDK::EAutoExposureMethod::AEM_Manual;
        float exposureBias = 1.0f + (intensity * 1.5f);  // +2.5 at default, up to +8.5
        pp.AutoExposureBias = exposureBias;
        pp.AutoExposureMinBrightness = 0.01f;
        pp.AutoExposureMaxBrightness = 50.0f;
        pp.AutoExposureSpeedUp = 50.0f;    // Fast adaptation
        pp.AutoExposureSpeedDown = 50.0f;

        // =====================================================================
        // SHADOW-SPECIFIC COLOR GRADING: boost green in shadows
        // This lifts dark areas with a green tint while leaving bright areas
        // relatively untouched. Much better than global ColorGain.
        // =====================================================================
        float shadowGreenGamma = 1.0f + (intensity * 0.15f);  // 1.15 default
        pp.ColorGammaShadows = SDK::FVector4{1.0, shadowGreenGamma, 1.0, 1.0};

        float shadowGreenGain = 0.6f + (intensity * 0.4f);  // 1.0 at default
        pp.ColorGainShadows = SDK::FVector4{0.4, shadowGreenGain, 0.35, 1.0};

        // =====================================================================
        // GLOBAL COLOR GRADING: subtle green tint (NOT overwhelming)
        // Partial desaturation + mild green boost
        // =====================================================================
        pp.ColorSaturation = SDK::FVector4{0.35, 0.6, 0.35, 1.0};
        float greenGain = 0.9f + (intensity * 0.2f);   // 1.1 at default
        pp.ColorGain = SDK::FVector4{0.6, greenGain, 0.55, 1.0};
        pp.ColorGamma = SDK::FVector4{0.95, 1.0, 0.95, 1.0};  // Very subtle green gamma

        // Small green floor (phosphor glow in total darkness)
        float greenFloor = 0.01f + (intensity * 0.01f);
        pp.ColorOffset = SDK::FVector4{0.0, greenFloor, 0.0, 0.0};

        // Scene color tint: very light green
        pp.SceneColorTint = SDK::FLinearColor{0.75f, 1.0f, 0.75f, 1.0f};

        // =====================================================================
        // BLOOM: light sources should bloom (NVGs bloom around bright things)
        // =====================================================================
        pp.BloomIntensity = bloom * 0.8f;
        pp.BloomThreshold = 0.3f;  // Moderate threshold

        // =====================================================================
        // FILM GRAIN: NVG static/noise
        // =====================================================================
        pp.FilmGrainIntensity = grain * 0.5f;
        pp.FilmGrainIntensityShadows = grain * 0.8f;
        pp.FilmGrainIntensityMidtones = grain * 0.4f;
        pp.FilmGrainIntensityHighlights = grain * 0.1f;
        pp.FilmGrainTexelSize = 1.5f;

        // =====================================================================
        // VIGNETTE: mode-dependent edge darkening
        // LensOverlay mode uses HEAVY vignette to simulate binocular tube view
        // Other modes use light vignette for subtle immersion
        // =====================================================================
        if (mode == NVGMode::LensOverlay)
        {
            // Heavy vignette: creates "looking through NVG tubes" effect
            // Gradually darkens edges, center remains bright = pseudo-lens circle
            float vignetteScale = 2.0f + (intensity * 1.5f);  // 3.5 at default, up to 9.5
            if (vignetteScale > 10.0f) vignetteScale = 10.0f;
            pp.VignetteIntensity = vignetteScale;
        }
        else
        {
            pp.VignetteIntensity = 0.4f;  // Very light vignette for other modes
        }

        // =====================================================================
        // CHROMATIC ABERRATION
        // =====================================================================
        pp.SceneFringeIntensity = aberration * 0.3f;
        pp.ChromaticAberrationStartOffset = 0.2f;

        // =====================================================================
        // DISABLE AO + MOTION BLUR
        // =====================================================================
        pp.AmbientOcclusionIntensity = 0.0f;
        pp.MotionBlurAmount = 0.0f;
        pp.Sharpen = 0.0f;

        if (logCounter_++ % 120 == 0)
        {
            LOG_INFO("[NVG] Applied camera PP:"
                << " exposureBias=" << exposureBias
                << " filmToe=" << toeLift
                << " greenGain=" << greenGain
                << " shadowGreenGain=" << shadowGreenGain
                << " bloom=" << pp.BloomIntensity
                << " grain=" << pp.FilmGrainIntensity
                << " vignette=" << pp.VignetteIntensity
                << " mode=" << ModeToString(mode));
        }
    }

    // -----------------------------------------------------------------------
    // Camera PP: Remove / restore
    // -----------------------------------------------------------------------
    void NVGSubsystem::RemoveCameraPP(SDK::UWorld* world)
    {
        auto* camera = FindPlayerCamera(world);
        if (!camera)
        {
            LOG_WARN("[NVG] Cannot remove camera PP: VR camera not found");
            originalCameraPP_.captured = false;
            return;
        }

        if (originalCameraPP_.captured)
        {
            camera->PostProcessBlendWeight = originalCameraPP_.postProcessBlendWeight;
            auto& pp = camera->PostProcessSettings;

            pp.ColorSaturation = originalCameraPP_.colorSaturation;
            pp.ColorGain = originalCameraPP_.colorGain;
            pp.ColorGamma = originalCameraPP_.colorGamma;
            pp.ColorOffset = originalCameraPP_.colorOffset;
            pp.SceneColorTint = originalCameraPP_.sceneColorTint;
            pp.ColorGammaShadows = originalCameraPP_.colorGammaShadows;
            pp.ColorGainShadows = originalCameraPP_.colorGainShadows;

            pp.FilmSlope = originalCameraPP_.filmSlope;
            pp.FilmToe = originalCameraPP_.filmToe;
            pp.FilmShoulder = originalCameraPP_.filmShoulder;

            pp.BloomIntensity = originalCameraPP_.bloomIntensity;
            pp.BloomThreshold = originalCameraPP_.bloomThreshold;
            pp.VignetteIntensity = originalCameraPP_.vignetteIntensity;

            pp.FilmGrainIntensity = originalCameraPP_.filmGrainIntensity;
            pp.FilmGrainIntensityShadows = originalCameraPP_.filmGrainIntensityShadows;
            pp.FilmGrainIntensityMidtones = originalCameraPP_.filmGrainIntensityMidtones;
            pp.FilmGrainIntensityHighlights = originalCameraPP_.filmGrainIntensityHighlights;
            pp.FilmGrainTexelSize = originalCameraPP_.filmGrainTexelSize;

            pp.SceneFringeIntensity = originalCameraPP_.sceneFringeIntensity;
            pp.ChromaticAberrationStartOffset = originalCameraPP_.chromaticAberrationStartOffset;

            pp.AutoExposureBias = originalCameraPP_.autoExposureBias;
            pp.AutoExposureMinBrightness = originalCameraPP_.autoExposureMinBrightness;
            pp.AutoExposureMaxBrightness = originalCameraPP_.autoExposureMaxBrightness;
            pp.AutoExposureSpeedUp = originalCameraPP_.autoExposureSpeedUp;
            pp.AutoExposureSpeedDown = originalCameraPP_.autoExposureSpeedDown;
            pp.AutoExposureMethod = static_cast<SDK::EAutoExposureMethod>(originalCameraPP_.autoExposureMethod);

            pp.AmbientOcclusionIntensity = originalCameraPP_.ambientOcclusionIntensity;
            pp.MotionBlurAmount = originalCameraPP_.motionBlurAmount;
            pp.Sharpen = originalCameraPP_.sharpen;

            // CRITICAL: Clear ALL bOverride flags so the game's normal PP pipeline
            // takes effect again. Without this, the camera continues to override
            // the game's PP volumes with our (now-restored) values, which may look
            // different from the game's intended rendering.
            pp.bOverride_ColorSaturation = 0;
            pp.bOverride_ColorGain = 0;
            pp.bOverride_ColorGamma = 0;
            pp.bOverride_ColorOffset = 0;
            pp.bOverride_SceneColorTint = 0;
            pp.bOverride_ColorGammaShadows = 0;
            pp.bOverride_ColorGainShadows = 0;
            pp.bOverride_FilmSlope = 0;
            pp.bOverride_FilmToe = 0;
            pp.bOverride_FilmShoulder = 0;
            pp.bOverride_BloomIntensity = 0;
            pp.bOverride_BloomThreshold = 0;
            pp.bOverride_VignetteIntensity = 0;
            pp.bOverride_FilmGrainIntensity = 0;
            pp.bOverride_FilmGrainIntensityShadows = 0;
            pp.bOverride_FilmGrainIntensityMidtones = 0;
            pp.bOverride_FilmGrainIntensityHighlights = 0;
            pp.bOverride_FilmGrainTexelSize = 0;
            pp.bOverride_SceneFringeIntensity = 0;
            pp.bOverride_ChromaticAberrationStartOffset = 0;
            pp.bOverride_AutoExposureBias = 0;
            pp.bOverride_AutoExposureMinBrightness = 0;
            pp.bOverride_AutoExposureMaxBrightness = 0;
            pp.bOverride_AutoExposureMethod = 0;
            pp.bOverride_AutoExposureSpeedUp = 0;
            pp.bOverride_AutoExposureSpeedDown = 0;
            pp.bOverride_AmbientOcclusionIntensity = 0;
            pp.bOverride_MotionBlurAmount = 0;
            pp.bOverride_Sharpen = 0;

            LOG_INFO("[NVG] Restored original camera PP values and cleared override flags");
        }
        else
        {
            LOG_INFO("[NVG] No original values captured; skipping restore");
        }

        originalCameraPP_.captured = false;
    }

    // -----------------------------------------------------------------------
    // Lens Mask: Apply game's NVG blendable if discovered, or fallback
    // -----------------------------------------------------------------------
    void NVGSubsystem::ApplyLensMask(SDK::UWorld* world)
    {
        NVGMode mode = mode_.load(std::memory_order_relaxed);
        if (mode == NVGMode::Fullscreen || mode == NVGMode::GameNVGOnly || mode == NVGMode::LensOverlay)
            return;  // Fullscreen: no lens. GameNVGOnly: handled elsewhere. LensOverlay: uses vignette only.

        // If we haven't probed yet, log a reminder
        if (!probeComplete_)
        {
            if (logCounter_++ % 300 == 0)
                LOG_INFO("[NVG] Lens mode active but probe not yet run. Run nvg_probe to discover MI_PP_NightVision.");
        }

        auto* camera = FindPlayerCamera(world);
        if (!camera)
        {
            if (logCounter_++ % 300 == 0)
                LOG_WARN("[NVG] ApplyLensMask: camera not found");
            return;
        }

        auto& wb = camera->PostProcessSettings.WeightedBlendables;

        // If we discovered the NVG material via probe, set its weight to 1
        if (probeFoundMaterial_ && discoveredNVGMaterial_)
        {
            bool found = false;
            for (int i = 0; i < wb.Array.Num(); i++)
            {
                if (wb.Array[i].Object == discoveredNVGMaterial_)
                {
                    if (wb.Array[i].Weight != 1.0f)
                    {
                        wb.Array[i].Weight = 1.0f;
                        LOG_INFO("[NVG] ApplyLensMask: set MI_PP_NightVision weight=1 at index " << i
                            << " (mode=" << ModeToString(mode) << ")"
                            << " NOTE: this material may render right-eye-only in stereo VR");
                    }
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                // Material not in array anymore (level reload?) — add it
                SDK::FWeightedBlendable entry;
                entry.Weight = 1.0f;
                entry.Object = discoveredNVGMaterial_;
                wb.Array.Add(entry);
                LOG_INFO("[NVG] ApplyLensMask: re-added MI_PP_NightVision to blendables (was missing)");
            }

            // Log full blendables state every 600 ticks for diagnosis
            if (logCounter_++ % 600 == 0)
            {
                LOG_INFO("[NVG] LensMask active blendables: " << ReadBlendables(camera));
            }
        }
        else if (discoveredPPElementIndex_ >= 0)
        {
            // We know the index but probe flagging missed it — use index directly
            if (discoveredPPElementIndex_ < wb.Array.Num())
            {
                auto& entry = wb.Array[discoveredPPElementIndex_];
                if (entry.Weight != 1.0f)
                {
                    entry.Weight = 1.0f;
                    LOG_INFO("[NVG] ApplyLensMask: set blendable index " << discoveredPPElementIndex_
                        << " weight=1 via saved index (object ptr may differ)");
                }
            }
        }
        else
        {
            // No discovered material or index — scan array for MI_PP_NightVision by name as last resort
            bool found = false;
            for (int i = 0; i < wb.Array.Num(); i++)
            {
                auto* obj = wb.Array[i].Object;
                if (!obj) continue;
                if (SDK::UKismetSystemLibrary::IsValid(obj))
                {
                    std::string name = obj->GetName();
                    if (name.find("NightVision") != std::string::npos ||
                        name.find("nightvision") != std::string::npos)
                    {
                        wb.Array[i].Weight = 1.0f;
                        discoveredNVGMaterial_ = obj;
                        discoveredPPElementIndex_ = i;
                        probeFoundMaterial_ = true;
                        probeComplete_ = true;
                        found = true;
                        LOG_INFO("[NVG] ApplyLensMask: found MI_PP_NightVision by name scan at index " << i
                            << " name=" << name);
                        break;
                    }
                }
            }
            if (!found && logCounter_++ % 300 == 0)
            {
                LOG_WARN("[NVG] ApplyLensMask: no MI_PP_NightVision discovered. Run nvg_probe.");
            }
        }
    }

    void NVGSubsystem::RemoveLensMask(SDK::UWorld* world)
    {
        // Set weight=0 on the discovered NVG blendable
        if (discoveredNVGMaterial_ || discoveredPPElementIndex_ >= 0)
        {
            auto* camera = FindPlayerCamera(world);
            if (camera)
            {
                auto& wb = camera->PostProcessSettings.WeightedBlendables;

                // Try by pointer first
                if (discoveredNVGMaterial_)
                {
                    for (int i = 0; i < wb.Array.Num(); i++)
                    {
                        if (wb.Array[i].Object == discoveredNVGMaterial_)
                        {
                            if (wb.Array[i].Weight != 0.0f)
                            {
                                wb.Array[i].Weight = 0.0f;
                                LOG_INFO("[NVG] RemoveLensMask: set MI_PP_NightVision weight=0 at index " << i);
                            }
                            break;
                        }
                    }
                }
                // Also try by saved index as fallback
                else if (discoveredPPElementIndex_ >= 0 && discoveredPPElementIndex_ < wb.Array.Num())
                {
                    if (wb.Array[discoveredPPElementIndex_].Weight != 0.0f)
                    {
                        wb.Array[discoveredPPElementIndex_].Weight = 0.0f;
                        LOG_INFO("[NVG] RemoveLensMask: set index " << discoveredPPElementIndex_ << " weight=0 via saved index");
                    }
                }
            }
        }

        // Clean up direct game SetNV if it was activated
        RemoveGameNVG(world);
    }

    // -----------------------------------------------------------------------
    // ApplyGameNVG / RemoveGameNVG: wrap SetNV for GameNVGOnly mode
    // -----------------------------------------------------------------------
    void NVGSubsystem::ApplyGameNVG(SDK::UWorld* world)
    {
        if (gameNVGActive_) return;  // Already on

        auto* ppComp = FindPlayerPostProcessComponent(world);
        if (!ppComp)
        {
            LOG_WARN("[NVG] ApplyGameNVG: BPC_PlayerPostProcess not found");
            return;
        }

        LOG_INFO("[NVG] ApplyGameNVG: calling SetNV(true) via ProcessEvent");
        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(true);
        }
        gameNVGActive_ = true;

        // Log resulting blendable state for stereo diagnosis
        auto* camera = FindPlayerCamera(world);
        if (camera)
        {
            LOG_INFO("[NVG] ApplyGameNVG post-SetNV blendables: " << ReadBlendables(camera));
        }
    }

    void NVGSubsystem::RemoveGameNVG(SDK::UWorld* world)
    {
        if (!gameNVGActive_) return;  // Already off

        auto* ppComp = FindPlayerPostProcessComponent(world);
        if (!ppComp)
        {
            LOG_WARN("[NVG] RemoveGameNVG: BPC_PlayerPostProcess not found");
            gameNVGActive_ = false;
            return;
        }

        LOG_INFO("[NVG] RemoveGameNVG: calling SetNV(false)");
        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(false);
        }
        gameNVGActive_ = false;
    }

    // -----------------------------------------------------------------------
    // EnableGameNVGDirect / DisableGameNVGDirect: public wrappers for commands
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::EnableGameNVGDirect(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "EnableGameNVGDirect: ";
        auto* ppComp = FindPlayerPostProcessComponent(world);
        if (!ppComp) { oss << "ERROR: BPC_PlayerPostProcess not found\n"; return oss.str(); }

        auto* camera = FindPlayerCamera(world);
        if (camera) oss << "Before: " << ReadBlendables(camera) << "\n";

        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(true);
        }
        gameNVGActive_ = true;
        oss << "SetNV(true) called\n";

        if (camera) oss << "After: " << ReadBlendables(camera) << "\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    std::string NVGSubsystem::DisableGameNVGDirect(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "DisableGameNVGDirect: ";
        auto* ppComp = FindPlayerPostProcessComponent(world);
        if (!ppComp) { oss << "ERROR: BPC_PlayerPostProcess not found\n"; return oss.str(); }

        auto* camera = FindPlayerCamera(world);
        if (camera) oss << "Before: " << ReadBlendables(camera) << "\n";

        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(false);
        }
        gameNVGActive_ = false;
        oss << "SetNV(false) called\n";

        if (camera) oss << "After: " << ReadBlendables(camera) << "\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // ProbeGameNVG: Discover the game's NVG blendable material
    // Strategy: read blendables -> call SetNV(true) -> read again -> compare
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::ProbeGameNVG(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== NVG PROBE ===\n";

        if (!world)
        {
            oss << "ERROR: world is null\n";
            return oss.str();
        }

        auto* camera = FindPlayerCamera(world);
        auto* ppComp = FindPlayerPostProcessComponent(world);

        oss << "Camera: " << (camera ? "found" : "NULL") << "\n";
        oss << "BPC_PlayerPostProcess: " << (ppComp ? "found" : "NULL") << "\n";

        if (!camera || !ppComp)
        {
            oss << "ERROR: Cannot probe without camera and BPC component\n";
            LOG_INFO("[NVG] Probe failed: " << oss.str());
            return oss.str();
        }

        // Step 1: Read current blendables
        oss << "\n--- BEFORE SetNV(true) ---\n";
        std::string beforeBlendables = ReadBlendables(camera);
        oss << beforeBlendables << "\n";
        LOG_INFO("[NVG] Probe BEFORE: " << beforeBlendables);

        // Capture before-state: object pointers AND weights (SetNV toggles weight, not array entries)
        struct BlendableSnapshot { SDK::UObject* obj; float weight; int arrayIndex; };
        std::vector<BlendableSnapshot> beforeStates;
        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        for (int i = 0; i < wb.Array.Num(); i++)
        {
            BlendableSnapshot snap;
            snap.obj = wb.Array[i].Object;
            snap.weight = wb.Array[i].Weight;
            snap.arrayIndex = i;
            beforeStates.push_back(snap);
        }

        // Also log the TidePPMesh
        auto* tideMesh = ppComp->TidePPMesh;
        oss << "TidePPMesh: " << (tideMesh ? "found" : "NULL") << "\n";
        if (tideMesh && SDK::UKismetSystemLibrary::IsValid(tideMesh))
        {
            Mod::ScopedProcessEventGuard guard;
            auto loc = tideMesh->K2_GetActorLocation();
            oss << "  TidePPMesh location: (" << loc.X << ", " << loc.Y << ", " << loc.Z << ")\n";
            oss << "  TidePPMesh name: " << tideMesh->GetName() << "\n";
        }

        // Also log PlayerCamera from the BPC component
        auto* bpcCam = ppComp->PlayerCamera;
        oss << "BPC->PlayerCamera: " << (bpcCam ? "found" : "NULL") << "\n";
        if (bpcCam && camera)
        {
            oss << "  Same as VRReplicatedCamera? " << (bpcCam == camera ? "YES" : "NO") << "\n";
        }

        // Step 2: Call SetNV(true)
        oss << "\n--- Calling SetNV(true) ---\n";
        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(true);
        }
        gameNVGActive_ = true;
        oss << "SetNV(true) called\n";

        // Step 3: Read blendables after
        oss << "\n--- AFTER SetNV(true) ---\n";
        std::string afterBlendables = ReadBlendables(camera);
        oss << afterBlendables << "\n";
        LOG_INFO("[NVG] Probe AFTER: " << afterBlendables);

        // Step 4: Find NVG entry — check for NEW entries AND for weight-activated existing entries
        // KEY INSIGHT: SetNV does NOT add new blendable entries; it changes weight of an existing one
        // (MI_PP_NightVision is always at index 4 with weight=0, SetNV sets it to weight=1)
        bool foundEntry = false;
        for (int i = 0; i < wb.Array.Num(); i++)
        {
            SDK::UObject* obj = wb.Array[i].Object;
            float newWeight = wb.Array[i].Weight;
            if (!obj) continue;

            // Find what the state was before
            bool wasPresent = false;
            float oldWeight = 0.0f;
            for (auto& snap : beforeStates)
            {
                if (snap.obj == obj)
                {
                    wasPresent = true;
                    oldWeight = snap.weight;
                    break;
                }
            }

            // Activated = either newly added with weight>0, OR existing entry whose weight went 0->positive
            bool activatedBySetNV = (!wasPresent && newWeight > 0.0f) ||
                                    (wasPresent && oldWeight <= 0.0f && newWeight > 0.0f);
            if (activatedBySetNV)
            {
                oss << "\n*** DISCOVERED NVG BLENDABLE ***\n";
                oss << "  Array Index: " << i << "\n";
                oss << "  Weight change: " << oldWeight << " -> " << newWeight << "\n";
                oss << "  " << (wasPresent ? "(existing entry, weight activated)" : "(new entry)") << "\n";
                oss << "  Object: 0x" << std::hex << reinterpret_cast<uintptr_t>(obj) << std::dec << "\n";
                if (SDK::UKismetSystemLibrary::IsValid(obj))
                {
                    oss << "  Class: " << obj->Class->GetName() << "\n";
                    oss << "  Name: " << obj->GetName() << "\n";
                }

                discoveredNVGMaterial_ = obj;
                discoveredPPElementIndex_ = i;
                probeFoundMaterial_ = true;
                foundEntry = true;
                LOG_INFO("[NVG] PROBE FOUND NVG material: " << obj->GetName()
                    << " class=" << obj->Class->GetName()
                    << " blendable index=" << i
                    << " weight: " << oldWeight << " -> " << newWeight);
            }
        }

        if (!foundEntry)
        {
            oss << "\nNo blendable entry was activated by SetNV.\n";
            oss << "Dumping full before/after comparison:\n";
            for (int i = 0; i < wb.Array.Num(); i++)
            {
                float beforeW = 0.0f;
                for (auto& snap : beforeStates)
                    if (snap.arrayIndex == i) { beforeW = snap.weight; break; }
                oss << "  [" << i << "] before=" << beforeW << " after=" << wb.Array[i].Weight;
                if (wb.Array[i].Object && SDK::UKismetSystemLibrary::IsValid(wb.Array[i].Object))
                    oss << " name=" << wb.Array[i].Object->GetName();
                oss << "\n";
            }
        }

        // Step 5: Also check MobilePostProcessSubsystem state
        {
            Mod::ScopedProcessEventGuard guard;
            auto* cls = SDK::UMobilePostProcessSubsystem::StaticClass();
            auto* sub = cls ? static_cast<SDK::UMobilePostProcessSubsystem*>(
                SDK::USubsystemBlueprintLibrary::GetEngineSubsystem(cls)) : nullptr;
            if (sub)
            {
                oss << "\nMobilePostProcessSubsystem after SetNV:\n"
                    << "  NVFactor=" << sub->NightVisionFactor
                    << " VignetteR=" << sub->VignetteRadius
                    << " BlinkR=" << sub->BlinkRadius
                    << " Sat=" << sub->Saturation
                    << " Contrast=" << sub->Contrast << "\n";
            }
        }

        // Step 6: Call SetNV(false) to clean up
        oss << "\n--- Calling SetNV(false) ---\n";
        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SetNV(false);
        }
        gameNVGActive_ = false;

        // Final blendables state
        oss << "\n--- AFTER SetNV(false) ---\n";
        std::string cleanBlendables = ReadBlendables(camera);
        oss << cleanBlendables << "\n";

        probeComplete_ = true;
        oss << "\n=== PROBE COMPLETE ===\n";
        oss << "Material found: " << (probeFoundMaterial_ ? "YES" : "NO") << "\n";

        std::string result = oss.str();
        LOG_INFO("[NVG] Probe result:\n" << result);
        return result;
    }

    // -----------------------------------------------------------------------
    // ProbePPElement: Directly call SwitchPPElement for experimentation
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::ProbePPElement(SDK::UWorld* world, int index, bool on)
    {
        std::ostringstream oss;
        oss << "SwitchPPElement(On=" << (on ? "true" : "false") << ", Index=" << index << ")\n";

        auto* ppComp = FindPlayerPostProcessComponent(world);
        if (!ppComp)
        {
            oss << "ERROR: BPC_PlayerPostProcess not found\n";
            return oss.str();
        }

        auto* camera = FindPlayerCamera(world);
        if (camera)
        {
            oss << "BEFORE: " << ReadBlendables(camera) << "\n";
        }

        // Call SwitchPPElement
        {
            Mod::ScopedProcessEventGuard guard;
            ppComp->SwitchPPElement(on, index);
        }
        oss << "SwitchPPElement called\n";

        if (camera)
        {
            oss << "AFTER: " << ReadBlendables(camera) << "\n";
        }

        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // DumpBlendables: Quick dump of current camera blendables
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::DumpBlendables(SDK::UWorld* world)
    {
        auto* camera = FindPlayerCamera(world);
        if (!camera) return "Camera not found";
        return ReadBlendables(camera);
    }

    // -----------------------------------------------------------------------
    // RunDiagnostics: Comprehensive NVG state dump
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::RunDiagnostics(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== NVG DIAGNOSTICS ===\n";
        oss << GetStatus() << "\n";

        auto* player = GameContext::GetPlayerCharacter();
        oss << "Player: " << (player ? "found" : "NULL") << "\n";

        if (player)
        {
            auto* ppComp = player->BPC_PlayerPostProcess;
            oss << "BPC_PlayerPostProcess: " << (ppComp ? "found" : "NULL") << "\n";
            if (ppComp)
            {
                oss << "  TidePPMesh: " << (ppComp->TidePPMesh ? "found" : "NULL") << "\n";
                oss << "  PlayerCamera: " << (ppComp->PlayerCamera ? "found" : "NULL") << "\n";
                oss << "  SpectatorComp: " << (ppComp->SpectatorComp ? "found" : "NULL") << "\n";
            }

            auto* vrCam = player->VRReplicatedCamera;
            oss << "VRReplicatedCamera: " << (vrCam ? "found" : "NULL") << "\n";
            if (vrCam)
            {
                oss << "  PostProcessBlendWeight: " << vrCam->PostProcessBlendWeight << "\n";
                oss << "  Blendables: " << ReadBlendables(vrCam) << "\n";
                oss << "  AutoExposureBias: " << vrCam->PostProcessSettings.AutoExposureBias << "\n";
                oss << "  AutoExposureMethod: " << static_cast<int>(vrCam->PostProcessSettings.AutoExposureMethod) << "\n";
                oss << "  FilmToe: " << vrCam->PostProcessSettings.FilmToe << "\n";
                oss << "  VignetteIntensity: " << vrCam->PostProcessSettings.VignetteIntensity << "\n";
            }
        }

        // MobilePostProcessSubsystem
        {
            Mod::ScopedProcessEventGuard guard;
            auto* cls = SDK::UMobilePostProcessSubsystem::StaticClass();
            auto* sub = cls ? static_cast<SDK::UMobilePostProcessSubsystem*>(
                SDK::USubsystemBlueprintLibrary::GetEngineSubsystem(cls)) : nullptr;
            oss << "MobilePostProcessSubsystem: " << (sub ? "found" : "NULL") << "\n";
            if (sub)
            {
                oss << "  NVFactor=" << sub->NightVisionFactor
                    << " VignetteR=" << sub->VignetteRadius
                    << " BlinkR=" << sub->BlinkRadius
                    << " Sat=" << sub->Saturation
                    << " Contrast=" << sub->Contrast << "\n";
            }
        }

        oss << "Probe: " << (probeComplete_ ? "done" : "pending")
            << " | Found material: " << (probeFoundMaterial_ ? "YES" : "NO") << "\n";
        if (discoveredNVGMaterial_ && SDK::UKismetSystemLibrary::IsValid(discoveredNVGMaterial_))
        {
            oss << "  Material: " << discoveredNVGMaterial_->GetName()
                << " class=" << discoveredNVGMaterial_->Class->GetName() << "\n";
        }

        std::string result = oss.str();
        LOG_INFO("[NVG] Diagnostics:\n" << result);
        return result;
    }

    // =======================================================================
    // MESH-BASED NVG LENS SYSTEM
    //
    // Architecture:
    //   1. Spawn ASceneCapture2D actor (has built-in SceneCaptureComponent2D)
    //   2. Configure capture component with NVG-like PostProcessSettings
    //      (green tint, bloom, grain via PostProcessSettings on the capture)
    //   3. Create UTextureRenderTarget2D as the capture's TextureTarget
    //   4. Load M_Lens material (the game's scope lens material — Unlit, mesh-based)
    //   5. Create MID from M_Lens, set RT_Scope texture param = our render target
    //   6. Spawn AStaticMeshActor with a simple mesh (1M_Cube, flattened)
    //   7. Apply our MID to the mesh
    //   8. Attach both actors to the VR camera
    //   9. Enable bCaptureEveryFrame on the scene capture
    //
    // Why this works for VR stereo:
    //   The lens mesh is a world-space object, so UE renders it for BOTH eyes.
    //   Unlike post-process blendables (which the game's NVG shader limits to
    //   one eye via StereoPassIndex checks), a mesh with an Unlit material
    //   renders identically in both stereo views.
    // =======================================================================

    std::string NVGSubsystem::SetupLens(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== SetupLens ===\n";

        if (lensActive_)
        {
            oss << "Lens already active. Call TeardownLens first.\n";
            return oss.str();
        }

        if (!world)
        {
            oss << "ERROR: world is null\n";
            return oss.str();
        }

        auto* camera = FindPlayerCamera(world);
        if (!camera)
        {
            oss << "ERROR: VR camera not found\n";
            return oss.str();
        }

        // --- Step 1: Create render target ---
        oss << "Creating render target " << lensResolution_ << "x" << lensResolution_ << "...\n";
        {
            Mod::ScopedProcessEventGuard guard;
            lensRenderTarget_ = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(
                world,
                lensResolution_,
                lensResolution_,
                SDK::ETextureRenderTargetFormat::RTF_RGBA16f,
                SDK::FLinearColor{0.0f, 0.0f, 0.0f, 1.0f},
                false,  // no auto mips
                false   // no UAV
            );
        }

        if (!lensRenderTarget_)
        {
            oss << "ERROR: CreateRenderTarget2D returned null\n";
            LOG_WARN("[NVG] SetupLens: CreateRenderTarget2D failed");
            return oss.str();
        }
        oss << "Render target created: " << lensRenderTarget_->GetName() << "\n";

        // --- Step 2: Spawn ASceneCapture2D actor ---
        oss << "Spawning ASceneCapture2D...\n";
        SDK::AActor* captureActor = nullptr;
        {
            SDK::FTransform transform{};
            transform.Translation = SDK::FVector{0.0, 0.0, 0.0};
            transform.Rotation = SDK::FQuat{0.0, 0.0, 0.0, 1.0};
            transform.Scale3D = SDK::FVector{1.0, 1.0, 1.0};

            Mod::ScopedProcessEventGuard guard;
            captureActor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                SDK::ASceneCapture2D::StaticClass(),
                transform,
                SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                nullptr,
                SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime
            );

            if (captureActor)
            {
                SDK::UGameplayStatics::FinishSpawningActor(captureActor, transform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);
            }
        }

        if (!captureActor)
        {
            oss << "ERROR: Failed to spawn ASceneCapture2D\n";
            LOG_WARN("[NVG] SetupLens: failed to spawn capture actor");
            return oss.str();
        }
        lensCaptureActor_ = captureActor;
        oss << "Capture actor spawned: " << captureActor->GetName() << "\n";

        // Configure the capture component
        auto* captureComp = static_cast<SDK::ASceneCapture2D*>(captureActor)->CaptureComponent2D;
        if (!captureComp)
        {
            oss << "ERROR: CaptureComponent2D is null on spawned actor\n";
            LOG_WARN("[NVG] SetupLens: CaptureComponent2D is null");
            TeardownLens(world);
            return oss.str();
        }
        oss << "CaptureComponent2D: " << captureComp->GetName() << "\n";

        // Set render target
        captureComp->TextureTarget = lensRenderTarget_;

        // Set FOV to match player view
        captureComp->FOVAngle = lensFOV_;

        // Capture source: SceneColorHDRNoAlpha for best quality
        captureComp->CaptureSource = SDK::ESceneCaptureSource::SCS_FinalToneCurveHDR;

        // Enable continuous capture
        // bCaptureEveryFrame is a bitfield at 0x0232 bit 0
        uint8_t& captureBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(captureComp) + 0x0232);
        captureBits |= 0x01;  // bCaptureEveryFrame = true
        captureBits |= 0x02;  // bCaptureOnMovement = true

        // Set PostProcessBlendWeight to 1.0 so our PP settings take effect
        captureComp->PostProcessBlendWeight = 1.0f;

        // Apply NVG look to the capture's PostProcessSettings
        UpdateLensCaptureSettings();

        oss << "Capture component configured: FOV=" << lensFOV_
            << " bCaptureEveryFrame=true captureSource=FinalToneCurveHDR\n";

        // --- Step 3: Attach capture actor to VR camera ---
        oss << "Attaching capture actor to VR camera...\n";
        {
            Mod::ScopedProcessEventGuard guard;
            captureActor->K2_AttachToComponent(
                camera,
                SDK::FName{},  // no socket
                SDK::EAttachmentRule::SnapToTarget,
                SDK::EAttachmentRule::SnapToTarget,
                SDK::EAttachmentRule::KeepRelative,
                false  // no weld
            );
        }
        oss << "Capture actor attached to camera\n";

        // Apply capture camera offset if set (allows shifting NVG camera position)
        if (captureOffsetX_ != 0.0f || captureOffsetY_ != 0.0f || captureOffsetZ_ != 0.0f)
        {
            SDK::FVector captureOff{captureOffsetX_, captureOffsetY_, captureOffsetZ_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            captureActor->K2_SetActorRelativeLocation(captureOff, false, &hit, true);
            oss << "Applied capture offset: X=" << captureOffsetX_ << " Y=" << captureOffsetY_
                << " Z=" << captureOffsetZ_ << "\n";
            LOG_INFO("[NVG] SetupLens: Applied capture offset X=" << captureOffsetX_
                << " Y=" << captureOffsetY_ << " Z=" << captureOffsetZ_);
        }

        // --- Step 4: Load lens material and create MID ---
        // Material type 0 = M_Lens (circular scope material with rim mask)
        // Material type 1 = M_Particle (simple translucent - no circle, no scope artifacts)
        oss << "Loading lens material (type=" << lensMatType_ << ")...\n";
        LOG_INFO("[NVG] SetupLens: loading material type " << lensMatType_);
        SDK::UMaterialInterface* lensMat = nullptr;

        if (lensMatType_ == 1)
        {
            // --- M_Particle: simple Unlit Translucent material ---
            // Has DIFF texture + Intensity scalar. No scope artifacts.
            lensMat = SDK::UObject::FindObject<SDK::UMaterialInterface>(
                "Material /Game/ITR2/Art/VFX/Environment/Dust/M_Particle.M_Particle");
            if (!lensMat)
            {
                const SDK::FString pathF(L"/Game/ITR2/Art/VFX/Environment/Dust/M_Particle.M_Particle");
                const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                Mod::ScopedProcessEventGuard guard;
                SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                if (loaded && loaded->IsA(SDK::UMaterialInterface::StaticClass()))
                    lensMat = static_cast<SDK::UMaterialInterface*>(loaded);
            }
            if (!lensMat)
            {
                oss << "WARNING: M_Particle not found, falling back to M_Lens\n";
                LOG_WARN("[NVG] M_Particle not found, falling back to M_Lens");
                lensMatType_ = 0;  // Fall back
            }
        }

        if (lensMatType_ == 2)
        {
            // --- MI_Lens_Magnifer: scope magnifier material, child of M_Lens ---
            // Pre-tuned with ImageScale=6.36, ImageDepth=-122.2, etc.
            lensMat = SDK::UObject::FindObject<SDK::UMaterialInterface>(
                "MaterialInstanceConstant /Game/ITR1/ART/Weapons/PSO/MI_Lens_Magnifer.MI_Lens_Magnifer");
            if (!lensMat)
            {
                const SDK::FString pathF(L"/Game/ITR1/ART/Weapons/PSO/MI_Lens_Magnifer.MI_Lens_Magnifer");
                const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                Mod::ScopedProcessEventGuard guard;
                SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                if (loaded && loaded->IsA(SDK::UMaterialInterface::StaticClass()))
                    lensMat = static_cast<SDK::UMaterialInterface*>(loaded);
            }
            if (!lensMat)
            {
                oss << "WARNING: MI_Lens_Magnifer not found, falling back to M_Lens\n";
                LOG_WARN("[NVG] MI_Lens_Magnifer not found, falling back to M_Lens");
                lensMatType_ = 0;  // Fall back
            }
        }

        if (lensMatType_ == 0)
        {
            // --- M_Lens: scope material with circular rim mask ---
            lensMat = SDK::UObject::FindObject<SDK::UMaterialInterface>(
                "Material /Game/ITR1/ART/Weapons/PSO/M_Lens.M_Lens");

            if (!lensMat)
            {
                const SDK::FString pathF(L"/Game/ITR1/ART/Weapons/PSO/M_Lens.M_Lens");
                const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                Mod::ScopedProcessEventGuard guard;
                SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                if (loaded && loaded->IsA(SDK::UMaterialInterface::StaticClass()))
                    lensMat = static_cast<SDK::UMaterialInterface*>(loaded);
            }

            if (!lensMat)
            {
                oss << "WARNING: M_Lens not found. Trying MI_Lens_PSO...\n";
                lensMat = SDK::UObject::FindObject<SDK::UMaterialInterface>(
                    "MaterialInstanceConstant /Game/ITR1/ART/Weapons/PSO/MI_Lens_PSO.MI_Lens_PSO");
                if (!lensMat)
                {
                    const SDK::FString pathF(L"/Game/ITR1/ART/Weapons/PSO/MI_Lens_PSO.MI_Lens_PSO");
                    const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                    const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                    Mod::ScopedProcessEventGuard guard;
                    SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                    if (loaded && loaded->IsA(SDK::UMaterialInterface::StaticClass()))
                        lensMat = static_cast<SDK::UMaterialInterface*>(loaded);
                }
            }
        }

        if (!lensMat)
        {
            oss << "ERROR: Could not load any lens material\n";
            LOG_WARN("[NVG] SetupLens: Failed to load lens material");
            TeardownLens(world);
            return oss.str();
        }
        oss << "Lens material loaded: " << lensMat->GetName() << " class=" << lensMat->Class->GetName() << "\n";

        // Create MID from lens material
        {
            Mod::ScopedProcessEventGuard guard;
            lensMID_ = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
                world,
                lensMat,
                SDK::FName{},
                SDK::EMIDCreationFlags::Transient
            );
        }

        if (!lensMID_)
        {
            oss << "ERROR: CreateDynamicMaterialInstance for lens material returned null\n";
            LOG_WARN("[NVG] SetupLens: lens MID creation failed");
            TeardownLens(world);
            return oss.str();
        }
        oss << "Lens MID created: " << lensMID_->GetName() << "\n";

        // Configure material parameters based on material type
        if (lensMatType_ == 1)
        {
            // --- M_Particle material params ---
            // Simple: DIFF (texture) + Intensity (scalar)
            Mod::ScopedProcessEventGuard guard;

            // Set render target as DIFF texture
            std::wstring diffParamW = L"DIFF";
            SDK::FName diffFname = SDK::BasicFilesImpleUtils::StringToName(diffParamW.c_str());
            lensMID_->SetTextureParameterValue(diffFname, static_cast<SDK::UTexture*>(lensRenderTarget_));
            LOG_INFO("[NVG] M_Particle: Set DIFF = render target");

            // Set intensity
            std::wstring intParamW = L"Intensity";
            SDK::FName intFname = SDK::BasicFilesImpleUtils::StringToName(intParamW.c_str());
            lensMID_->SetScalarParameterValue(intFname, 1.0f);
            LOG_INFO("[NVG] M_Particle: Set Intensity = 1.0");

            oss << "M_Particle: Set DIFF=RT, Intensity=1.0 (simple translucent, no circle mask)\n";
        }
        else if (lensMatType_ == 2)
        {
            // --- MI_Lens_Magnifer material params ---
            // Child of M_Lens. Has pre-tuned scope params that we OVERRIDE for NVG use.
            // Grid is eliminated. ImageScale/Rim/ImageDepth are managed by UpdateLensMaterialParams().
            {
                Mod::ScopedProcessEventGuard guard;

                // Set render target
                std::wstring paramW = L"RT_Scope";
                SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(paramW.c_str());
                lensMID_->SetTextureParameterValue(fname, static_cast<SDK::UTexture*>(lensRenderTarget_));
                oss << "MI_Lens_Magnifer: Set RT_Scope = render target\n";
                LOG_INFO("[NVG] MI_Lens_Magnifer: Set RT_Scope = render target");

                // Eliminate grid overlay
                auto setScalar = [&](const wchar_t* name, float val) {
                    SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(name);
                    lensMID_->SetScalarParameterValue(fn, val);
                    LOG_INFO("[NVG] MI_Lens_Magnifer scalar: " << std::string(name, name + wcslen(name)) << " = " << val);
                };
                setScalar(L"GridDepth", 0.0f);      // Transparent grid
                setScalar(L"GridScale", 100.0f);     // Zoom out grid
                setScalar(L"BrightnessMult", 1.0f);  // Normal brightness
                setScalar(L"FocusDistanceOffset", 0.0f);
                setScalar(L"OuterRimScale", 2.0f);

                // Set GridColor to fully transparent
                std::wstring gcParamW = L"GridColor";
                SDK::FName gcFname = SDK::BasicFilesImpleUtils::StringToName(gcParamW.c_str());
                SDK::FLinearColor transparentBlack{0.0f, 0.0f, 0.0f, 0.0f};
                lensMID_->SetVectorParameterValue(gcFname, transparentBlack);
                LOG_INFO("[NVG] MI_Lens_Magnifer: Set GridColor = transparent");

                // Override the Grid texture with a white texture (insurance)
                SDK::UTexture* whiteTex = SDK::UObject::FindObject<SDK::UTexture>(
                    "Texture2D /Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");
                if (!whiteTex)
                    whiteTex = SDK::UObject::FindObject<SDK::UTexture>(
                        "Texture2D /Engine/EngineResources/White.White");
                if (whiteTex)
                {
                    SDK::FName gridTexFname = SDK::BasicFilesImpleUtils::StringToName(L"Grid");
                    lensMID_->SetTextureParameterValue(gridTexFname, whiteTex);
                    LOG_INFO("[NVG] MI_Lens_Magnifer: Grid texture overridden with white");
                }
            }
            oss << "MI_Lens_Magnifer: Set RT_Scope + eliminated grid. ImageScale/Rim managed dynamically.\n";
        }
        else
        {
            // --- M_Lens material params ---
            // Set the RT_Scope texture parameter to our render target.
            // ImageScale, ImageDepth, Rim Scale, RimDepth are managed by UpdateLensMaterialParams()
            // which runs every tick and adapts to current lens geometry.
            {
                std::wstring paramW = L"RT_Scope";
                SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(paramW.c_str());
                Mod::ScopedProcessEventGuard guard;
                lensMID_->SetTextureParameterValue(fname, static_cast<SDK::UTexture*>(lensRenderTarget_));
            }
            oss << "Set RT_Scope param on lens MID\n";

            // Grid elimination + static params (non-image/rim)
            {
                Mod::ScopedProcessEventGuard guard;
                auto setScalar = [&](const wchar_t* name, float value) {
                    std::wstring nameW = name;
                    SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(nameW.c_str());
                    lensMID_->SetScalarParameterValue(fname, value);
                    LOG_INFO("[NVG] MID scalar: " << std::string(nameW.begin(), nameW.end()) << " = " << value);
                };

                auto setVector = [&](const wchar_t* name, float r, float g, float b, float a) {
                    std::wstring nameW = name;
                    SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(nameW.c_str());
                    SDK::FLinearColor color{r, g, b, a};
                    lensMID_->SetVectorParameterValue(fname, color);
                    LOG_INFO("[NVG] MID vector: " << std::string(nameW.begin(), nameW.end())
                             << " = (" << r << ", " << g << ", " << b << ", " << a << ")");
                };

                // *** GRID OVERLAY REMOVAL ***
                setScalar(L"GridDepth", 0.0f);
                setScalar(L"GridScale", 100.0f);
                setVector(L"GridColor", 0.0f, 0.0f, 0.0f, 0.0f);

                // Static params (not managed by tick update)
                setScalar(L"OuterRimScale", 2.0f);
                setScalar(L"FocusDistanceOffset", 0.0f);
                setScalar(L"BrightnessMult", 1.0f);

                // *** Override Grid TEXTURE to white ***
                SDK::UTexture* whiteTex = nullptr;
                const char* whiteTexPaths[] = {
                    "Texture2D /Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture",
                    "Texture2D /Engine/EngineResources/White.White",
                    "Texture2D /Engine/EngineResources/DefaultTexture.DefaultTexture",
                    "Texture2D /Engine/EngineMaterials/DefaultWhiteGrid.DefaultWhiteGrid",
                    nullptr
                };
                for (int i = 0; whiteTexPaths[i] && !whiteTex; ++i)
                {
                    whiteTex = SDK::UObject::FindObject<SDK::UTexture>(whiteTexPaths[i]);
                    if (whiteTex) {
                        LOG_INFO("[NVG] Found white texture: " << whiteTexPaths[i]);
                    }
                }

                if (whiteTex)
                {
                    std::wstring gridTexParamW = L"Grid";
                    SDK::FName gridTexFname = SDK::BasicFilesImpleUtils::StringToName(gridTexParamW.c_str());
                    lensMID_->SetTextureParameterValue(gridTexFname, whiteTex);
                    LOG_INFO("[NVG] Overrode Grid texture with white texture: " << whiteTex->GetName());
                    oss << "Grid texture overridden with: " << whiteTex->GetName() << "\n";
                }
                else
                {
                    LOG_WARN("[NVG] No white texture found for Grid override — relying on GridDepth=0");
                    oss << "WARNING: Could not find white texture for Grid override\n";
                }
            }
            oss << "M_Lens: grid eliminated, ImageScale/Rim/ImageDepth managed dynamically\n";
        }  // end else (M_Lens material type)

        // --- Step 5: Spawn mesh actor for the lens ---
        oss << "Spawning lens mesh actor...\n";
        SDK::AActor* meshActor = nullptr;
        {
            SDK::FTransform transform{};
            transform.Translation = SDK::FVector{0.0, 0.0, 0.0};
            transform.Rotation = SDK::FQuat{0.0, 0.0, 0.0, 1.0};
            transform.Scale3D = SDK::FVector{1.0, 1.0, 1.0};

            Mod::ScopedProcessEventGuard guard;
            meshActor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                world,
                SDK::AStaticMeshActor::StaticClass(),
                transform,
                SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                nullptr,
                SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime
            );

            if (meshActor)
            {
                SDK::UGameplayStatics::FinishSpawningActor(meshActor, transform, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);
            }
        }

        if (!meshActor)
        {
            oss << "ERROR: Failed to spawn AStaticMeshActor\n";
            LOG_WARN("[NVG] SetupLens: failed to spawn mesh actor");
            TeardownLens(world);
            return oss.str();
        }
        lensMeshActor_ = meshActor;
        oss << "Mesh actor spawned: " << meshActor->GetName() << "\n";

        auto* meshComp = static_cast<SDK::AStaticMeshActor*>(meshActor)->StaticMeshComponent;
        if (!meshComp)
        {
            oss << "ERROR: StaticMeshComponent is null\n";
            TeardownLens(world);
            return oss.str();
        }

        // *** CRITICAL: Set mobility to Movable FIRST ***
        // AStaticMeshActor defaults to Static mobility. ALL transform calls
        // (K2_SetActorRelativeLocation, K2_AttachToComponent, SetStaticMesh etc.)
        // SILENTLY FAIL on Static actors. This was the root cause of the mesh
        // being stuck at world origin (0,0,0) with HasMesh: NO.
        {
            Mod::ScopedProcessEventGuard guard;
            static_cast<SDK::AStaticMeshActor*>(meshActor)->SetMobility(SDK::EComponentMobility::Movable);
            meshComp->SetMobility(SDK::EComponentMobility::Movable);
        }
        oss << "Mobility set to Movable on actor AND component\n";
        LOG_INFO("[NVG] SetupLens: Mobility set to Movable");

        // Load mesh based on mesh type
        SDK::UStaticMesh* mesh = nullptr;
        bool usingCylinder = false;
        oss << "Loading mesh type " << lensMeshType_ << " (" << (lensMeshType_ == 1 ? "Cylinder" : "Plane") << ")...\n";

        if (lensMeshType_ == 1)
        {
            // Cylinder mesh: /Engine/EngineMeshes/Cylinder (confirmed from exports)
            mesh = SDK::UObject::FindObject<SDK::UStaticMesh>(
                "StaticMesh /Engine/EngineMeshes/Cylinder.Cylinder");
            if (!mesh)
            {
                const SDK::FString pathF(L"/Engine/EngineMeshes/Cylinder.Cylinder");
                const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                Mod::ScopedProcessEventGuard guard;
                SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                if (loaded && loaded->IsA(SDK::UStaticMesh::StaticClass()))
                    mesh = static_cast<SDK::UStaticMesh*>(loaded);
            }
            if (mesh)
            {
                usingCylinder = true;
                oss << "Cylinder mesh loaded: " << mesh->GetName() << "\n";
                LOG_INFO("[NVG] SetupLens: Using Cylinder mesh");
            }
            else
            {
                oss << "WARNING: Cylinder mesh not found, falling back to Plane\n";
                LOG_WARN("[NVG] Cylinder mesh not found, falling back to Plane");
            }
        }

        // Fallback: Plane mesh
        if (!mesh)
        {
            mesh = SDK::UObject::FindObject<SDK::UStaticMesh>(
                "StaticMesh /Engine/BasicShapes/Plane.Plane");
            if (!mesh)
            {
                const SDK::FString pathF(L"/Engine/BasicShapes/Plane.Plane");
                const SDK::FSoftObjectPath softPath = SDK::UKismetSystemLibrary::MakeSoftObjectPath(pathF);
                const SDK::TSoftObjectPtr<SDK::UObject> softRef = SDK::UKismetSystemLibrary::Conv_SoftObjPathToSoftObjRef(softPath);
                Mod::ScopedProcessEventGuard guard;
                SDK::UObject* loaded = SDK::UKismetSystemLibrary::LoadAsset_Blocking(softRef);
                if (loaded && loaded->IsA(SDK::UStaticMesh::StaticClass()))
                    mesh = static_cast<SDK::UStaticMesh*>(loaded);
            }
        }

        if (!mesh)
        {
            oss << "ERROR: Could not load any mesh for lens\n";
            LOG_WARN("[NVG] SetupLens: Failed to load lens mesh");
            TeardownLens(world);
            return oss.str();
        }
        oss << "Mesh loaded: " << mesh->GetName() << "\n";

        // Set the mesh (AFTER mobility is Movable!)
        bool setMeshResult = false;
        {
            Mod::ScopedProcessEventGuard guard;
            setMeshResult = meshComp->SetStaticMesh(mesh);
        }
        oss << "SetStaticMesh returned: " << (setMeshResult ? "TRUE" : "FALSE") << "\n";
        LOG_INFO("[NVG] SetupLens: SetStaticMesh returned " << (setMeshResult ? "TRUE" : "FALSE"));

        // Verify mesh was actually set
        if (!meshComp->StaticMesh)
        {
            oss << "WARNING: StaticMesh property is null after SetStaticMesh! Trying direct assignment...\n";
            LOG_WARN("[NVG] SetupLens: StaticMesh null after SetStaticMesh, trying direct property write");
            meshComp->StaticMesh = mesh;
            oss << "Direct assignment result: " << (meshComp->StaticMesh ? "SET" : "STILL NULL") << "\n";
        }

        // Apply lens MID to mesh
        {
            Mod::ScopedProcessEventGuard guard;
            meshComp->SetMaterial(0, static_cast<SDK::UMaterialInterface*>(lensMID_));
        }
        oss << "Applied lens MID to mesh\n";

        // Disable collision so lens mesh doesn't block anything
        {
            Mod::ScopedProcessEventGuard guard;
            meshComp->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
        }

        // Set scale based on mesh type.
        // IMPORTANT: For the cylinder with rotation P270/Y180/R270, the local Z axis (cylinder
        // height) is NOT pointing toward the camera — it lies in the visible plane. Using a
        // hardcoded 0.001 for Z was making the cylinder appear as a thin line at small scales.
        // Uniform scaling (X=Y=Z=lensScale_) ensures the disc looks circular regardless of which
        // local axis happens to face the camera after user-chosen rotation.
        SDK::FVector meshScale;
        if (usingCylinder)
        {
            // Cylinder: uniform scaling so the circular face is always a proper circle,
            // regardless of which local axis points toward the camera after rotation.
            meshScale = SDK::FVector{lensScale_, lensScale_, lensScale_};
            oss << "Using Cylinder uniform scale: " << lensScale_ << "\n";
            LOG_INFO("[NVG] SetupLens: Cylinder scale=" << lensScale_ << " (uniform XYZ)");
        }
        else
        {
            // Plane: X/Y control size, Z=1.0 (plane mesh has zero Z extent by nature)
            meshScale = SDK::FVector{lensScale_, lensScale_, 1.0};
            oss << "Using Plane scale: " << lensScale_ << "\n";
            LOG_INFO("[NVG] SetupLens: Plane scale=" << lensScale_ << " XY, Z=1.0");
        }
        {
            Mod::ScopedProcessEventGuard guard;
            meshActor->SetActorRelativeScale3D(meshScale);
        }
        oss << "Mesh scale set: " << meshScale.X << ", " << meshScale.Y << ", " << meshScale.Z << "\n";

        // --- Step 6: Place mesh using camera world pose, then attach ---
        // This mirrors the Loadout subsystem approach: compute world transform from
        // an anchor and apply it explicitly BEFORE attachment. This prevents actors
        // from getting stuck at world origin if attachment alone doesn't move them.
        SDK::FVector cameraWorldLoc{};
        SDK::FRotator cameraWorldRot{};
        SDK::FVector cameraForward{};
        {
            Mod::ScopedProcessEventGuard guard;
            cameraWorldLoc = camera->K2_GetComponentLocation();
            cameraWorldRot = camera->K2_GetComponentRotation();
            cameraForward = camera->GetForwardVector();
        }
        oss << "Camera world: pos=(" << cameraWorldLoc.X << ", " << cameraWorldLoc.Y << ", " << cameraWorldLoc.Z
            << ") rot=P" << cameraWorldRot.Pitch << " Y" << cameraWorldRot.Yaw << " R" << cameraWorldRot.Roll
            << " fwd=(" << cameraForward.X << ", " << cameraForward.Y << ", " << cameraForward.Z << ")\n";

        // Compute mesh world transform: camera pos + forward * distance
        SDK::FVector meshWorldLoc{
            cameraWorldLoc.X + (cameraForward.X * lensDistance_),
            cameraWorldLoc.Y + (cameraForward.Y * lensDistance_),
            cameraWorldLoc.Z + (cameraForward.Z * lensDistance_)
        };

        SDK::FRotator meshWorldRot{
                cameraWorldRot.Pitch + lensRotPitch_,
                cameraWorldRot.Yaw + lensRotYaw_,
                cameraWorldRot.Roll + lensRotRoll_
            };
        
        // First: set world-space position/rotation explicitly (works even without attachment)
        {
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            meshActor->K2_SetActorLocationAndRotation(meshWorldLoc, meshWorldRot, false, &hit, true);
        }
        oss << "Mesh world-preplaced at (" << meshWorldLoc.X << ", " << meshWorldLoc.Y << ", " << meshWorldLoc.Z
            << ") rot(P=" << meshWorldRot.Pitch << " Y=" << meshWorldRot.Yaw << " R=" << meshWorldRot.Roll << ")\n";

        // Verify the world placement actually took effect
        {
            Mod::ScopedProcessEventGuard guard;
            auto verifyLoc = meshActor->K2_GetActorLocation();
            auto verifyRot = meshActor->K2_GetActorRotation();
            oss << "Verify after world-place: pos=(" << verifyLoc.X << ", " << verifyLoc.Y << ", " << verifyLoc.Z
                << ") rot=P" << verifyRot.Pitch << " Y" << verifyRot.Yaw << " R" << verifyRot.Roll << "\n";
            const bool nearOriginPre = (std::abs(verifyLoc.X) < 1.0 && std::abs(verifyLoc.Y) < 1.0 && std::abs(verifyLoc.Z) < 1.0);
            if (nearOriginPre)
            {
                oss << "*** WARNING: Mesh STILL at origin after K2_SetActorLocationAndRotation! ***\n";
                LOG_WARN("[NVG] SetupLens: Mesh still at origin after explicit world placement!");
            }
        }

        // Attach to camera using KeepWorld — preserves the world position we just set
        {
            Mod::ScopedProcessEventGuard guard;
            meshActor->K2_AttachToComponent(
                camera,
                SDK::FName{},  // no socket
                SDK::EAttachmentRule::KeepWorld,
                SDK::EAttachmentRule::KeepWorld,
                SDK::EAttachmentRule::KeepWorld,
                false
            );
        }
        oss << "Attached to camera with KeepWorld rules\n";

        // Now also set the relative transform (for ongoing camera-following behavior)
        {
            SDK::FVector offset{lensDistance_, lensOffsetY_, lensOffsetZ_};  // X=forward, Y=right, Z=up
            SDK::FRotator relRot{lensRotPitch_, lensRotYaw_, lensRotRoll_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            meshActor->K2_SetActorRelativeLocation(offset, false, &hit, true);
            meshActor->K2_SetActorRelativeRotation(relRot, false, &hit, true);
        }
        oss << "Set relative offset (" << lensDistance_ << ", " << lensOffsetY_ << ", " << lensOffsetZ_ << ") rot(P=" << lensRotPitch_ << " Y=" << lensRotYaw_ << " R=" << lensRotRoll_ << ")\n";

        // Verify again — if mesh ended up near origin, force world placement as fallback
        {
            Mod::ScopedProcessEventGuard guard;
            auto verifyLoc = meshActor->K2_GetActorLocation();
            const bool nearOrigin = (std::abs(verifyLoc.X) < 1.0 && std::abs(verifyLoc.Y) < 1.0 && std::abs(verifyLoc.Z) < 1.0);
            if (nearOrigin)
            {
                oss << "*** FALLBACK: Mesh at origin after attach. Forcing world placement again ***\n";
                LOG_WARN("[NVG] SetupLens: mesh at origin after attach; forcing world placement fallback");
                SDK::FHitResult hit{};
                meshActor->K2_SetActorLocationAndRotation(meshWorldLoc, meshWorldRot, false, &hit, true);

                auto verifyLoc2 = meshActor->K2_GetActorLocation();
                oss << "After fallback: pos=(" << verifyLoc2.X << ", " << verifyLoc2.Y << ", " << verifyLoc2.Z << ")\n";
            }
            else
            {
                oss << "Post-attach verify: pos=(" << verifyLoc.X << ", " << verifyLoc.Y << ", " << verifyLoc.Z << ") OK\n";
            }
        }

        // Explicitly ensure mesh is visible
        {
            Mod::ScopedProcessEventGuard guard;
            meshActor->SetActorHiddenInGame(false);
        }

        // --- CRITICAL: Hide lens mesh from scene capture ---
        // The scene capture camera captures the ENTIRE scene, including the lens mesh itself.
        // This creates a recursive black shape (the mesh is opaque and blocks what's behind it).
        // Setting bHiddenInSceneCapture on the mesh component makes the capture camera skip it.
        // bHiddenInSceneCapture is at offset 0x0262 bit 0 on UPrimitiveComponent.
        {
            uint8_t& hideBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(meshComp) + 0x0262);
            hideBits |= 0x01;  // bHiddenInSceneCapture = true
            oss << "Set bHiddenInSceneCapture=true on lens mesh component\n";
            LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on lens mesh");
        }

        // // --- Hide hand widget components from scene capture ---
        // // The W_GripDebug_L/R widget components on the player character are 3D widgets
        // // that the scene capture camera also sees, creating dark rectangles in the NVG view.
        // {
        //     auto* player = GameContext::GetPlayerCharacter();
        //     if (player)
        //     {
        //         SDK::UWidgetComponent* widgetComps[] = { player->W_GripDebug_L, player->W_GripDebug_R };
        //         const char* names[] = { "W_GripDebug_L", "W_GripDebug_R" };
        //         for (int i = 0; i < 2; ++i)
        //         {
        //             if (widgetComps[i])
        //             {
        //                 uint8_t& wBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(widgetComps[i]) + 0x0262);
        //                 wBits |= 0x01;  // bHiddenInSceneCapture = true
        //                 oss << "Set bHiddenInSceneCapture=true on " << names[i] << "\n";
        //                 LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on " << names[i]);
        //             }
        //             else
        //             {
        //                 oss << "WARNING: " << names[i] << " is null, cannot hide from capture\n";
        //             }
        //         }
        //     }
        //     else
        //     {
        //         oss << "WARNING: Player character null, cannot hide hand widgets from capture\n";
        //     }
        // }

        // --- Hide player helmet/head mesh from scene capture ---
        // The player's HeadMesh (helmet/headgear) renders in the NVG capture, encroaching the view.
        // We set bHiddenInSceneCapture on HeadMesh and any actors attached to the head area.
        hiddenFromCaptureComps_.clear();
        {
            auto* player = GameContext::GetPlayerCharacter();
            if (player)
            {
                // 1. HeadMesh — the actual helmet/head skeletal mesh
                SDK::USkeletalMeshComponent* headMesh = player->HeadMesh;
                if (headMesh)
                {
                    uint8_t& hideBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(headMesh) + 0x0262);
                    if (!(hideBits & 0x01))  // Only if not already hidden
                    {
                        hideBits |= 0x01;
                        hiddenFromCaptureComps_.push_back(reinterpret_cast<uintptr_t>(headMesh));
                        oss << "Set bHiddenInSceneCapture on HeadMesh\n";
                        LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on HeadMesh");
                    }
                }
                else
                {
                    oss << "WARNING: HeadMesh is null\n";
                    LOG_WARN("[NVG] SetupLens: HeadMesh is null, cannot hide helmet");
                }

                // 2. Head_GripArea / Head_GripArea1 — collision boxes that might render
                SDK::UBoxComponent* headGrips[] = { player->Head_GripArea, player->Head_GripArea1 };
                const char* gripNames[] = { "Head_GripArea", "Head_GripArea1" };
                for (int i = 0; i < 2; ++i)
                {
                    if (headGrips[i])
                    {
                        uint8_t& hBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(headGrips[i]) + 0x0262);
                        if (!(hBits & 0x01))
                        {
                            hBits |= 0x01;
                            hiddenFromCaptureComps_.push_back(reinterpret_cast<uintptr_t>(headGrips[i]));
                            oss << "Set bHiddenInSceneCapture on " << gripNames[i] << "\n";
                            LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on " << gripNames[i]);
                        }
                    }
                }

                // 3. Gloves — hand skeletal mesh (can clip into view)
                SDK::USkeletalMeshComponent* gloveMesh = player->Gloves;
                if (gloveMesh)
                {
                    uint8_t& gBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(gloveMesh) + 0x0262);
                    if (!(gBits & 0x01))
                    {
                        gBits |= 0x01;
                        hiddenFromCaptureComps_.push_back(reinterpret_cast<uintptr_t>(gloveMesh));
                        oss << "Set bHiddenInSceneCapture on Gloves\n";
                        LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on Gloves");
                    }
                }

                // 4. HeadSlotSocket children — items mounted on head (attached actors)
                if (player->HeadSlotSocket)
                {
                    // Walk all child components looking for UPrimitiveComponent derivatives
                    SDK::TArray<SDK::USceneComponent*> children;
                    player->HeadSlotSocket->GetChildrenComponents(true, &children);
                    int childCount = children.Num();
                    LOG_INFO("[NVG] SetupLens: HeadSlotSocket has " << childCount << " child components");
                    oss << "HeadSlotSocket children: " << childCount << "\n";
                    for (int i = 0; i < childCount; ++i)
                    {
                        SDK::USceneComponent* child = children[i];
                        if (!child) continue;

                        // Check if it's a primitive component (has the bHiddenInSceneCapture bit)
                        // All UPrimitiveComponent subclasses have this at 0x0262
                        if (child->IsA(SDK::UPrimitiveComponent::StaticClass()))
                        {
                            uint8_t& cBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(child) + 0x0262);
                            if (!(cBits & 0x01))
                            {
                                cBits |= 0x01;
                                hiddenFromCaptureComps_.push_back(reinterpret_cast<uintptr_t>(child));
                                LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on HeadSlotSocket child: "
                                    << child->GetName() << " (class: " << child->Class->GetName() << ")");
                                oss << "  Hidden HeadSlot child: " << child->GetName() << "\n";
                            }
                        }
                    }
                }

                // 5. BPC_Head_Slot — holster component; walk its child components too
                if (player->BPC_Head_Slot)
                {
                    // The holster is a UActorComponent, walk its owner's attached actors
                    // Actually BPC_Head_Slot is a component, we need child scene components
                    SDK::TArray<SDK::USceneComponent*> headSlotChildren;
                    auto* asScene = static_cast<SDK::USceneComponent*>(static_cast<SDK::UObject*>(player->BPC_Head_Slot));
                    // BPC_PlayerCharacterHolster_C inherits from UActorComponent (scene component)
                    // Use GetChildrenComponents if it's a scene component
                    if (asScene && asScene->IsA(SDK::USceneComponent::StaticClass()))
                    {
                        asScene->GetChildrenComponents(true, &headSlotChildren);
                        int count = headSlotChildren.Num();
                        LOG_INFO("[NVG] SetupLens: BPC_Head_Slot has " << count << " child components");
                        for (int i = 0; i < count; ++i)
                        {
                            SDK::USceneComponent* child = headSlotChildren[i];
                            if (!child) continue;
                            if (child->IsA(SDK::UPrimitiveComponent::StaticClass()))
                            {
                                uint8_t& cBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(child) + 0x0262);
                                if (!(cBits & 0x01))
                                {
                                    cBits |= 0x01;
                                    hiddenFromCaptureComps_.push_back(reinterpret_cast<uintptr_t>(child));
                                    LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on BPC_Head_Slot child: "
                                        << child->GetName() << " (class: " << child->Class->GetName() << ")");
                                    oss << "  Hidden HeadSlot child: " << child->GetName() << "\n";
                                }
                            }
                        }
                    }
                }

                LOG_INFO("[NVG] SetupLens: Total components hidden from capture: " << hiddenFromCaptureComps_.size());
                oss << "Total components hidden from capture: " << hiddenFromCaptureComps_.size() << "\n";
            }
            else
            {
                oss << "WARNING: Player null, cannot hide helmet from capture\n";
            }
        }

        // --- Step 7: Spawn second lens mesh (right eye, mirrored Y) ---
        // Uses the same mesh, material (lensMID_), and scale as the first lens.
        // Positioned at {lensDistance_, -lensOffsetY_, lensOffsetZ_} so right eye sees it.
        // Also needs bHiddenInSceneCapture to avoid recursive capture loop.
        lensMeshActor2_ = nullptr;
        {
            SDK::FTransform transform2{};
            transform2.Translation = SDK::FVector{0.0, 0.0, 0.0};
            transform2.Rotation = SDK::FQuat{0.0, 0.0, 0.0, 1.0};
            transform2.Scale3D = SDK::FVector{1.0, 1.0, 1.0};

            SDK::AActor* meshActor2 = nullptr;
            {
                Mod::ScopedProcessEventGuard guard;
                meshActor2 = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
                    world,
                    SDK::AStaticMeshActor::StaticClass(),
                    transform2,
                    SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                    nullptr,
                    SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime
                );
                if (meshActor2)
                    SDK::UGameplayStatics::FinishSpawningActor(meshActor2, transform2, SDK::ESpawnActorScaleMethod::SelectDefaultAtRuntime);
            }

            if (!meshActor2)
            {
                oss << "WARNING: Failed to spawn second (right-eye) lens mesh actor — continuing with single lens\n";
                LOG_WARN("[NVG] SetupLens: failed to spawn second mesh actor (right eye)");
            }
            else
            {
                lensMeshActor2_ = meshActor2;
                oss << "Right-eye mesh actor spawned: " << meshActor2->GetName() << "\n";

                auto* meshComp2 = static_cast<SDK::AStaticMeshActor*>(meshActor2)->StaticMeshComponent;
                if (meshComp2)
                {
                    // Movable so transform calls work
                    {
                        Mod::ScopedProcessEventGuard guard;
                        static_cast<SDK::AStaticMeshActor*>(meshActor2)->SetMobility(SDK::EComponentMobility::Movable);
                        meshComp2->SetMobility(SDK::EComponentMobility::Movable);
                    }

                    // Set same mesh as left lens
                    {
                        Mod::ScopedProcessEventGuard guard;
                        bool ok = meshComp2->SetStaticMesh(static_cast<SDK::AStaticMeshActor*>(meshActor)->StaticMeshComponent->StaticMesh);
                        if (!ok && meshComp2->StaticMesh == nullptr)
                            meshComp2->StaticMesh = static_cast<SDK::AStaticMeshActor*>(meshActor)->StaticMeshComponent->StaticMesh;
                    }

                    // Apply same MID
                    {
                        Mod::ScopedProcessEventGuard guard;
                        meshComp2->SetMaterial(0, static_cast<SDK::UMaterialInterface*>(lensMID_));
                    }

                    // Disable collision
                    {
                        Mod::ScopedProcessEventGuard guard;
                        meshComp2->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
                    }

                    // Same scale as left lens
                    {
                        Mod::ScopedProcessEventGuard guard;
                        meshActor2->SetActorRelativeScale3D(meshScale);
                    }

                    // World-pre-place then attach
                    SDK::FVector meshWorldLoc2{
                        cameraWorldLoc.X + (cameraForward.X * lensDistance_),
                        cameraWorldLoc.Y + (cameraForward.Y * lensDistance_),
                        cameraWorldLoc.Z + (cameraForward.Z * lensDistance_)
                    };
                    {
                        SDK::FHitResult hit2{};
                        Mod::ScopedProcessEventGuard guard;
                        meshActor2->K2_SetActorLocationAndRotation(meshWorldLoc2, meshWorldRot, false, &hit2, true);
                    }

                    // Attach to camera with KeepWorld
                    {
                        Mod::ScopedProcessEventGuard guard;
                        meshActor2->K2_AttachToComponent(
                            camera,
                            SDK::FName{},
                            SDK::EAttachmentRule::KeepWorld,
                            SDK::EAttachmentRule::KeepWorld,
                            SDK::EAttachmentRule::KeepWorld,
                            false
                        );
                    }

                    // Relative offset: mirrored Y for right eye
                    {
                        SDK::FVector offset2{lensDistance_, -lensOffsetY_, lensOffsetZ_};
                        SDK::FRotator relRot2{lensRotPitch_, lensRotYaw_, lensRotRoll_};
                        SDK::FHitResult hit2{};
                        Mod::ScopedProcessEventGuard guard;
                        meshActor2->K2_SetActorRelativeLocation(offset2, false, &hit2, true);
                        meshActor2->K2_SetActorRelativeRotation(relRot2, false, &hit2, true);
                    }
                    oss << "Right-eye lens offset: (" << lensDistance_ << ", " << -lensOffsetY_ << ", " << lensOffsetZ_ << ")\n";

                    // Ensure visible
                    {
                        Mod::ScopedProcessEventGuard guard;
                        meshActor2->SetActorHiddenInGame(false);
                    }

                    // Hide from scene capture (same as left lens)
                    {
                        uint8_t& hideBits2 = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(meshComp2) + 0x0262);
                        hideBits2 |= 0x01;
                        oss << "Set bHiddenInSceneCapture=true on right-eye lens mesh\n";
                        LOG_INFO("[NVG] SetupLens: Set bHiddenInSceneCapture on right-eye lens mesh");
                    }
                }
                else
                {
                    oss << "WARNING: Right-eye StaticMeshComponent is null\n";
                }
            }
        }

        lensActive_ = true;
        lensDiagCounter_ = 0;  // Reset diagnostic counter for tick logging

        // --- Apply initial material params (ImageScale, Rim, ImageDepth) ---
        // This sets the dynamic projection params that control how the "painting" maps to lens.
        // Also auto-computes ImageScale from lens geometry if rtImageScale_ == 0.
        UpdateLensMaterialParams();
        {
            float effectiveImageScale = rtImageScale_ > 0.01f ? rtImageScale_ : ComputeAutoImageScale();
            oss << "Initial material params: ImageScale=" << effectiveImageScale
                << " (auto=" << (rtImageScale_ <= 0.01f ? "YES" : "NO") << ")"
                << " RimScale=" << rtRimScale_
                << " RimDepth=" << rtRimDepth_
                << " ImageDepth=" << rtImageDepth_ << "\n";
            LOG_INFO("[NVG] SetupLens: material params: ImageScale=" << effectiveImageScale
                << " RimScale=" << rtRimScale_ << " RimDepth=" << rtRimDepth_
                << " ImageDepth=" << rtImageDepth_);
        }

        // Apply auto-FOV if enabled
        if (autoFOV_)
        {
            float autoFov = ComputeAutoFOV();
            lensFOV_ = autoFov;
            if (lensCaptureActor_ && SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
            {
                auto* cc = static_cast<SDK::ASceneCapture2D*>(lensCaptureActor_)->CaptureComponent2D;
                if (cc) cc->FOVAngle = lensFOV_;
            }
            oss << "AutoFOV applied: " << autoFov << " deg\n";
            LOG_INFO("[NVG] SetupLens: AutoFOV=" << autoFov);
        }

        // --- POST-SETUP DIAGNOSTICS (comprehensive) ---
        {
            Mod::ScopedProcessEventGuard guard;
            auto meshLoc = meshActor->K2_GetActorLocation();
            auto meshRot = meshActor->K2_GetActorRotation();
            auto camLoc = camera->K2_GetComponentLocation();
            auto camRot = camera->K2_GetComponentRotation();
            oss << "\n--- POST-SETUP DIAGNOSTICS ---\n";
            oss << "  Camera component world pos: (" << camLoc.X << ", " << camLoc.Y << ", " << camLoc.Z << ")\n";
            oss << "  Camera component world rot: P=" << camRot.Pitch << " Y=" << camRot.Yaw << " R=" << camRot.Roll << "\n";
            oss << "  Mesh world pos: (" << meshLoc.X << ", " << meshLoc.Y << ", " << meshLoc.Z << ")\n";
            oss << "  Mesh world rot: P=" << meshRot.Pitch << " Y=" << meshRot.Yaw << " R=" << meshRot.Roll << "\n";

            // Distance check: mesh should be roughly lensDistance_ from camera
            double dx = meshLoc.X - camLoc.X;
            double dy = meshLoc.Y - camLoc.Y;
            double dz = meshLoc.Z - camLoc.Z;
            double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            oss << "  Distance from camera to mesh: " << dist << " (expected ~" << lensDistance_ << ")\n";

            if (lensCaptureActor_) {
                auto capLoc = lensCaptureActor_->K2_GetActorLocation();
                oss << "  Capture world pos: (" << capLoc.X << ", " << capLoc.Y << ", " << capLoc.Z << ")\n";
            }
        }

        // Log mesh component details
        {
            auto* sma = static_cast<SDK::AStaticMeshActor*>(meshActor);
            if (sma->StaticMeshComponent) {
                auto* smc = sma->StaticMeshComponent;
                oss << "  MeshComp HasMesh: " << (smc->StaticMesh ? "YES" : "NO") << "\n";
                oss << "  MeshComp NumMaterials: " << smc->GetNumMaterials() << "\n";
                auto mat0 = smc->GetMaterial(0);
                oss << "  MeshComp Material[0]: " << (mat0 ? mat0->GetName() : "NULL") << "\n";
                oss << "  MeshComp Visible: " << (smc->IsVisible() ? "YES" : "NO") << "\n";

                // Log the actual mesh object name for extra confirmation
                if (smc->StaticMesh)
                    oss << "  MeshComp MeshName: " << smc->StaticMesh->GetName() << "\n";
            }
        }

        oss << "\nLens system ACTIVE.\n"
            << "  FOV = " << lensFOV_ << "\n"
            << "  Scale = " << lensScale_ << "\n"
            << "  Distance = " << lensDistance_ << "\n"
            << "  Rotation = P" << lensRotPitch_ << " Y" << lensRotYaw_ << " R" << lensRotRoll_ << "\n"
            << "  Resolution = " << lensResolution_ << "x" << lensResolution_ << "\n";

        LOG_INFO("[NVG] SetupLens completed successfully");
        return oss.str();
    }

    std::string NVGSubsystem::TeardownLens(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== TeardownLens ===\n";

        if (lensMeshActor_)
        {
            if (SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
            {
                Mod::ScopedProcessEventGuard guard;
                lensMeshActor_->K2_DestroyActor();
                oss << "Destroyed lens mesh actor (left eye)\n";
            }
            lensMeshActor_ = nullptr;
        }

        if (lensMeshActor2_)
        {
            if (SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
            {
                Mod::ScopedProcessEventGuard guard;
                lensMeshActor2_->K2_DestroyActor();
                oss << "Destroyed lens mesh actor (right eye)\n";
            }
            lensMeshActor2_ = nullptr;
        }

        if (lensCaptureActor_)
        {
            if (SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
            {
                Mod::ScopedProcessEventGuard guard;
                lensCaptureActor_->K2_DestroyActor();
                oss << "Destroyed capture actor\n";
            }
            lensCaptureActor_ = nullptr;
        }

        if (lensRenderTarget_)
        {
            // Release the render target resources
            Mod::ScopedProcessEventGuard guard;
            SDK::UKismetRenderingLibrary::ReleaseRenderTarget2D(lensRenderTarget_);
            lensRenderTarget_ = nullptr;
            oss << "Released render target\n";
        }

        lensMID_ = nullptr;  // MID is garbage-collected along with world
        lensActive_ = false;

        // Restore hand widget visibility in scene captures
        {
            auto* player = GameContext::GetPlayerCharacter();
            if (player)
            {
                SDK::UWidgetComponent* widgetComps[] = { player->W_GripDebug_L, player->W_GripDebug_R };
                const char* names[] = { "W_GripDebug_L", "W_GripDebug_R" };
                for (int i = 0; i < 2; ++i)
                {
                    if (widgetComps[i])
                    {
                        uint8_t& wBits = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(widgetComps[i]) + 0x0262);
                        wBits &= ~0x01;  // bHiddenInSceneCapture = false (restore default)
                        oss << "Restored bHiddenInSceneCapture=false on " << names[i] << "\n";
                    }
                }
            }
        }

        // Restore all components we hid from scene capture (helmet, head slot items, gloves, etc.)
        {
            int restored = 0;
            for (uintptr_t addr : hiddenFromCaptureComps_)
            {
                // Minimal safety: check if address is non-zero
                if (addr != 0)
                {
                    uint8_t& bits = *reinterpret_cast<uint8_t*>(addr + 0x0262);
                    bits &= ~0x01;  // bHiddenInSceneCapture = false
                    ++restored;
                }
            }
            LOG_INFO("[NVG] TeardownLens: Restored bHiddenInSceneCapture on " << restored << " components");
            oss << "Restored bHiddenInSceneCapture on " << restored << " components\n";
            hiddenFromCaptureComps_.clear();
        }

        oss << "Lens system DEACTIVATED.\n";

        LOG_INFO("[NVG] TeardownLens completed");
        return oss.str();
    }

    void NVGSubsystem::UpdateLensCaptureSettings()
    {
        if (!lensCaptureActor_ || !SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
            return;

        auto* captureComp = static_cast<SDK::ASceneCapture2D*>(lensCaptureActor_)->CaptureComponent2D;
        if (!captureComp) return;

        auto& pp = captureComp->PostProcessSettings;

        // Enable override flags for NVG look
        pp.bOverride_ColorSaturation = 1;
        pp.bOverride_ColorGain = 1;
        pp.bOverride_ColorGamma = 1;
        pp.bOverride_ColorOffset = 1;
        pp.bOverride_SceneColorTint = 1;
        pp.bOverride_ColorGammaShadows = 1;
        pp.bOverride_ColorGainShadows = 1;
        pp.bOverride_FilmSlope = 1;
        pp.bOverride_FilmToe = 1;
        pp.bOverride_FilmShoulder = 1;
        pp.bOverride_BloomIntensity = 1;
        pp.bOverride_BloomThreshold = 1;
        pp.bOverride_FilmGrainIntensity = 1;
        pp.bOverride_FilmGrainIntensityShadows = 1;
        pp.bOverride_FilmGrainIntensityMidtones = 1;
        pp.bOverride_FilmGrainIntensityHighlights = 1;
        pp.bOverride_FilmGrainTexelSize = 1;
        pp.bOverride_AutoExposureBias = 1;
        pp.bOverride_AutoExposureMethod = 1;
        pp.bOverride_AutoExposureMinBrightness = 1;
        pp.bOverride_AutoExposureMaxBrightness = 1;
        pp.bOverride_AmbientOcclusionIntensity = 1;
        pp.bOverride_MotionBlurAmount = 1;
        pp.bOverride_VignetteIntensity = 1;

        // Read user-controlled values for lens capture PP
        const float intensity = intensity_.load(std::memory_order_relaxed);
        const float grain = grain_.load(std::memory_order_relaxed);
        const float bloom = bloom_.load(std::memory_order_relaxed);

        // Partial desaturation + green tint (subtle — avoid washed-out look)
        pp.ColorSaturation = SDK::FVector4{0.2, 0.5, 0.2, 1.0};
        float greenGain = lensNvgGreenGain_;
        pp.ColorGain = SDK::FVector4{0.3, greenGain, 0.25, 1.0};
        pp.ColorGamma = SDK::FVector4{0.95, 1.05, 0.95, 1.0};

        // Green floor (phosphor glow) — reduced
        pp.ColorOffset = SDK::FVector4{0.0, 0.008, 0.0, 0.0};

        // Scene color tint — subtler green
        pp.SceneColorTint = SDK::FLinearColor{0.8f, 1.0f, 0.8f, 1.0f};

        // Shadow-specific green boost — reduced
        pp.ColorGammaShadows = SDK::FVector4{1.0, 1.1, 1.0, 1.0};
        pp.ColorGainShadows = SDK::FVector4{0.4, 0.8, 0.35, 1.0};

        // Tone mapping
        pp.FilmToe = 0.7f + (intensity * 0.1f);
        pp.FilmSlope = 0.88f;
        pp.FilmShoulder = 0.26f;

        // Auto-exposure
        pp.AutoExposureMethod = SDK::EAutoExposureMethod::AEM_Manual;
        pp.AutoExposureBias = lensNvgExposureBias_;
        pp.AutoExposureMinBrightness = 0.03f;
        pp.AutoExposureMaxBrightness = 20.0f;

        // Bloom (NVG blooms around bright sources) — uses user grain/bloom controls
        pp.BloomIntensity = bloom > 0.01f ? bloom : lensNvgBloomIntensity_;
        pp.BloomThreshold = 0.5f;

        // Film grain for NVG noise — uses user grain control
        // grain_ controls ALL grain channels for visible effect
        pp.FilmGrainIntensity = grain > 0.01f ? grain * 0.5f : lensNvgGrainIntensity_;
        pp.FilmGrainIntensityShadows = grain * 0.8f;
        pp.FilmGrainIntensityMidtones = grain * 0.4f;
        pp.FilmGrainIntensityHighlights = grain * 0.1f;
        pp.FilmGrainTexelSize = 1.5f;

        // No vignette on capture (the M_Lens material handles the circular mask)
        pp.VignetteIntensity = 0.0f;

        // Disable AO and motion blur for cleaner capture
        pp.AmbientOcclusionIntensity = 0.0f;
        pp.MotionBlurAmount = 0.0f;

        // Update FOV if changed
        captureComp->FOVAngle = lensFOV_;
    }

    void NVGSubsystem::SetLensDistance(float dist)
    {
        if (!std::isfinite(dist)) dist = 20.0f;
        if (dist < 1.0f) dist = 1.0f;
        if (dist > 200.0f) dist = 200.0f;
        lensDistance_ = dist;

        if (lensActive_ && lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
        {
            SDK::FVector offset{lensDistance_, lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor_->K2_SetActorRelativeLocation(offset, false, &hit, true);

            auto meshLoc = lensMeshActor_->K2_GetActorLocation();
            LOG_INFO("[NVG] SetLensDistance -> " << dist
                << " Y=" << lensOffsetY_ << " Z=" << lensOffsetZ_
                << " (world pos=" << meshLoc.X << "," << meshLoc.Y << "," << meshLoc.Z << ")");
        }
        else
        {
            LOG_INFO("[NVG] SetLensDistance -> " << dist << " (stored; lens not active)");
        }

        // Mirror right-eye lens at -lensOffsetY_
        if (lensActive_ && lensMeshActor2_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
        {
            SDK::FVector offset2{lensDistance_, -lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit2{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor2_->K2_SetActorRelativeLocation(offset2, false, &hit2, true);
        }
    }

    void NVGSubsystem::SetLensRotation(float pitch, float yaw, float roll)
    {
        if (!std::isfinite(pitch)) pitch = 90.0f;
        if (!std::isfinite(yaw)) yaw = 0.0f;
        if (!std::isfinite(roll)) roll = 0.0f;
        lensRotPitch_ = pitch;
        lensRotYaw_ = yaw;
        lensRotRoll_ = roll;

        if (lensActive_ && lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
        {
            SDK::FRotator rot{pitch, yaw, roll};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor_->K2_SetActorRelativeRotation(rot, false, &hit, true);

            // Log resulting world rotation + position for diagnostics
            auto worldRot = lensMeshActor_->K2_GetActorRotation();
            auto worldLoc = lensMeshActor_->K2_GetActorLocation();
            LOG_INFO("[NVG] SetLensRotation -> P=" << pitch << " Y=" << yaw << " R=" << roll
                << " (world rot: P=" << worldRot.Pitch << " Y=" << worldRot.Yaw << " R=" << worldRot.Roll
                << ", pos=" << worldLoc.X << "," << worldLoc.Y << "," << worldLoc.Z << ")");
        }
        else
        {
            LOG_INFO("[NVG] SetLensRotation -> P=" << pitch << " Y=" << yaw << " R=" << roll
                << " (stored; lens not active)");
        }

        // Apply same rotation to right-eye lens
        if (lensActive_ && lensMeshActor2_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
        {
            SDK::FRotator rot2{pitch, yaw, roll};
            SDK::FHitResult hit2{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor2_->K2_SetActorRelativeRotation(rot2, false, &hit2, true);
        }
    }

    // -----------------------------------------------------------------------
    // StepLensRot* — rotate a single axis in 90-degree increments.
    // Normalises the accumulated value to [0, 360) so displayed values stay
    // clean, then delegates to SetLensRotation which applies immediately if
    // the lens mesh actor is alive.  Heavy logging is intentional while
    // testing the correct orientation for the cylinder lens.
    // -----------------------------------------------------------------------
    static float NormaliseAngle(float deg)
    {
        // fmod can return negative values for negative inputs
        float n = std::fmod(deg, 360.0f);
        if (n < 0.0f) n += 360.0f;
        return n;
    }

    void NVGSubsystem::StepLensRotPitch(float deltaDeg)
    {
        float newPitch = NormaliseAngle(lensRotPitch_ + deltaDeg);
        LOG_INFO("[NVG] StepLensRotPitch: " << lensRotPitch_ << " + " << deltaDeg
            << " -> " << newPitch
            << " (current PYR: " << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_ << ")");
        SetLensRotation(newPitch, lensRotYaw_, lensRotRoll_);
        LOG_INFO("[NVG] StepLensRotPitch done: PYR=" << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_
            << " lensActive=" << lensActive_
            << " meshActor=" << (lensMeshActor_ ? lensMeshActor_->GetName() : "null"));
    }

    void NVGSubsystem::StepLensRotYaw(float deltaDeg)
    {
        float newYaw = NormaliseAngle(lensRotYaw_ + deltaDeg);
        LOG_INFO("[NVG] StepLensRotYaw: " << lensRotYaw_ << " + " << deltaDeg
            << " -> " << newYaw
            << " (current PYR: " << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_ << ")");
        SetLensRotation(lensRotPitch_, newYaw, lensRotRoll_);
        LOG_INFO("[NVG] StepLensRotYaw done: PYR=" << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_
            << " lensActive=" << lensActive_
            << " meshActor=" << (lensMeshActor_ ? lensMeshActor_->GetName() : "null"));
    }

    void NVGSubsystem::StepLensRotRoll(float deltaDeg)
    {
        float newRoll = NormaliseAngle(lensRotRoll_ + deltaDeg);
        LOG_INFO("[NVG] StepLensRotRoll: " << lensRotRoll_ << " + " << deltaDeg
            << " -> " << newRoll
            << " (current PYR: " << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_ << ")");
        SetLensRotation(lensRotPitch_, lensRotYaw_, newRoll);
        LOG_INFO("[NVG] StepLensRotRoll done: PYR=" << lensRotPitch_ << "/" << lensRotYaw_ << "/" << lensRotRoll_
            << " lensActive=" << lensActive_
            << " meshActor=" << (lensMeshActor_ ? lensMeshActor_->GetName() : "null"));
    }

    void NVGSubsystem::SetLensMatType(int type)
    {
        if (type < 0 || type > 2) type = 0;
        lensMatType_ = type;
        const char* names[] = {"M_Lens circular", "M_Particle simple", "MI_Lens_Magnifer"};
        LOG_INFO("[NVG] SetLensMatType: " << type << " (" << names[type] << ")");
        dirty_.store(true, std::memory_order_release);  // Triggers teardown+setup on next tick
    }

    void NVGSubsystem::SetLensMeshType(int type)
    {
        if (type < 0 || type > 1) type = 0;
        lensMeshType_ = type;
        const char* names[] = {"Plane (square)", "Cylinder (disc)"};
        LOG_INFO("[NVG] SetLensMeshType: " << type << " (" << names[type] << ")");
        dirty_.store(true, std::memory_order_release);
    }

    void NVGSubsystem::SetLensOffset(float y, float z)
    {
        if (!std::isfinite(y)) y = 0.0f;
        if (!std::isfinite(z)) z = 0.0f;
        lensOffsetY_ = y;
        lensOffsetZ_ = z;

        // Apply immediately if lens is active
        if (lensActive_ && lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
        {
            SDK::FVector offset{lensDistance_, lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor_->K2_SetActorRelativeLocation(offset, false, &hit, true);
            LOG_INFO("[NVG] SetLensOffset -> Y=" << y << " Z=" << z << " (applied live)");
        }
        else
        {
            LOG_INFO("[NVG] SetLensOffset -> Y=" << y << " Z=" << z << " (stored; lens not active)");
        }

        // Mirror right-eye lens at -lensOffsetY_
        if (lensActive_ && lensMeshActor2_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
        {
            SDK::FVector offset2{lensDistance_, -lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit2{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor2_->K2_SetActorRelativeLocation(offset2, false, &hit2, true);
        }
    }

    void NVGSubsystem::SetLensAdjustMode(bool enabled)
    {
        lensAdjustMode_.store(enabled, std::memory_order_relaxed);
        LOG_INFO("[NVG] LensAdjustMode: " << (enabled ? "ON - left stick moves lens" : "OFF - locked"));
        if (enabled)
        {
            LOG_INFO("[NVG] Current lens offset: Y=" << lensOffsetY_ << " Z=" << lensOffsetZ_
                << " dist=" << lensDistance_);
        }
    }

    void NVGSubsystem::ApplyLensAdjust(float stickX, float stickY)
    {
        if (!lensAdjustMode_.load(std::memory_order_relaxed)) return;

        // Apply deadzone
        const float deadzone = 0.15f;
        if (std::abs(stickX) < deadzone) stickX = 0.0f;
        if (std::abs(stickY) < deadzone) stickY = 0.0f;
        if (stickX == 0.0f && stickY == 0.0f) return;

        // Map stick to offset: X = left/right (Y offset), Y = forward/back (Z offset)
        lensOffsetY_ += stickX * lensAdjustSpeed_;
        lensOffsetZ_ += stickY * lensAdjustSpeed_;

        // Apply immediately
        if (lensActive_ && lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
        {
            SDK::FVector offset{lensDistance_, lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor_->K2_SetActorRelativeLocation(offset, false, &hit, true);
        }

        // Mirror right-eye lens at -lensOffsetY_
        if (lensActive_ && lensMeshActor2_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
        {
            SDK::FVector offset2{lensDistance_, -lensOffsetY_, lensOffsetZ_};
            SDK::FHitResult hit2{};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor2_->K2_SetActorRelativeLocation(offset2, false, &hit2, true);
        }

        // Log position every ~30 calls (≈0.5s at 60fps)
        static int logCounter = 0;
        if (++logCounter % 30 == 0)
        {
            LOG_INFO("[NVG] LensAdjust pos: Y=" << lensOffsetY_ << " Z=" << lensOffsetZ_
                << " dist=" << lensDistance_);
        }
    }

    void NVGSubsystem::SetCaptureOffset(float x, float y, float z)
    {
        if (!std::isfinite(x)) x = 0.0f;
        if (!std::isfinite(y)) y = 0.0f;
        if (!std::isfinite(z)) z = 0.0f;

        // Clamp to reasonable range (cm)
        if (x < -50.0f) x = -50.0f; if (x > 50.0f) x = 50.0f;
        if (y < -50.0f) y = -50.0f; if (y > 50.0f) y = 50.0f;
        if (z < -50.0f) z = -50.0f; if (z > 50.0f) z = 50.0f;

        captureOffsetX_ = x;
        captureOffsetY_ = y;
        captureOffsetZ_ = z;

        // Apply immediately if capture actor is alive
        if (lensActive_ && lensCaptureActor_ && SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
        {
            SDK::FVector offset{captureOffsetX_, captureOffsetY_, captureOffsetZ_};
            SDK::FHitResult hit{};
            Mod::ScopedProcessEventGuard guard;
            lensCaptureActor_->K2_SetActorRelativeLocation(offset, false, &hit, true);

            auto capLoc = lensCaptureActor_->K2_GetActorLocation();
            LOG_INFO("[NVG] SetCaptureOffset -> X=" << x << " Y=" << y << " Z=" << z
                << " (world pos=" << capLoc.X << "," << capLoc.Y << "," << capLoc.Z << ")");
        }
        else
        {
            LOG_INFO("[NVG] SetCaptureOffset -> X=" << x << " Y=" << y << " Z=" << z
                << " (stored; capture not active)");
        }
    }

    // -----------------------------------------------------------------------
    // Compute auto ImageScale from lens geometry.
    // ImageScale controls how much the render target is "zoomed" on the mesh.
    // Goal: the RT "painting" should be big enough that stereo parallax never
    // reveals the edges. We compute based on lens visual angle vs capture FOV.
    // -----------------------------------------------------------------------
    float NVGSubsystem::ComputeAutoImageScale() const
    {
        // Cylinder default radius = 50 UU (verified in UE). At lensScale_, displayed radius = 50 * lensScale_.
        float radius = 50.0f * lensScale_;
        if (radius < 0.1f) radius = 0.1f;
        float dist = lensDistance_;
        if (dist < 1.0f) dist = 1.0f;

        // Visual half-angle of the lens from eye center
        float halfAngleRad = std::atan2(radius, dist);
        float visualAngleDeg = 2.0f * halfAngleRad * (180.0f / 3.14159265f);
        if (visualAngleDeg < 1.0f) visualAngleDeg = 1.0f;

        // ImageScale = capture FOV / visual angle, with overscan factor for IPD offset
        // The IPD offset means each eye sees from ~3.2cm off-center, which shifts the
        // effective viewing angle. Adding 40% overscan ensures edges are never visible.
        float autoScale = (lensFOV_ / visualAngleDeg) * 1.4f;

        // Clamp to reasonable range
        if (autoScale < 0.5f) autoScale = 0.5f;
        if (autoScale > 50.0f) autoScale = 50.0f;

        return autoScale;
    }

    // -----------------------------------------------------------------------
    // Compute auto FOV: narrow the capture to match the lens visual angle.
    // This gives higher effective resolution since we don't waste pixels.
    // -----------------------------------------------------------------------
    float NVGSubsystem::ComputeAutoFOV() const
    {
        float radius = 50.0f * lensScale_;
        if (radius < 0.1f) radius = 0.1f;
        float dist = lensDistance_;
        if (dist < 1.0f) dist = 1.0f;

        // Visual angle + 30% overscan for stereo parallax safety
        float halfAngleRad = std::atan2(radius, dist);
        float visualAngleDeg = 2.0f * halfAngleRad * (180.0f / 3.14159265f);
        float autoFov = visualAngleDeg * 1.3f;

        // Clamp
        if (autoFov < 5.0f) autoFov = 5.0f;
        if (autoFov > 170.0f) autoFov = 170.0f;

        return autoFov;
    }

    // -----------------------------------------------------------------------
    // Dynamically update MID material params (ImageScale, Rim, ImageDepth).
    // Called every tick when lens is active. Cheap — just SetScalarParameterValue calls.
    // -----------------------------------------------------------------------
    void NVGSubsystem::UpdateLensMaterialParams()
    {
        if (!lensMID_ || !SDK::UKismetSystemLibrary::IsValid(lensMID_))
            return;

        // M_Particle (type 1) doesn't have these scope material params
        if (lensMatType_ == 1)
            return;

        // Compute effective ImageScale (0 = auto from lens geometry)
        float imageScale = rtImageScale_;
        if (imageScale <= 0.01f)
            imageScale = ComputeAutoImageScale();

        Mod::ScopedProcessEventGuard guard;

        // ImageScale: how "zoomed" the render target is on the mesh.
        // ScaleUVsByCenter divides UVs by this value, so larger = more zoom = bigger "painting".
        {
            SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(L"ImageScale");
            lensMID_->SetScalarParameterValue(fn, imageScale);
        }

        // ImageDepth: opacity of the image layer. Very negative = fully opaque via ExponentialDensity.
        {
            SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(L"ImageDepth");
            lensMID_->SetScalarParameterValue(fn, rtImageDepth_);
        }

        // Rim Scale: radius of the RadialGradientExponential circular mask.
        // Very large pushes the rim far outside the visible mesh area.
        {
            SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(L"Rim Scale");
            lensMID_->SetScalarParameterValue(fn, rtRimScale_);
        }

        // RimDepth: sharpness of the rim. 0 = ExponentialDensity(0) = 0 → no rim at all.
        {
            SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(L"RimDepth");
            lensMID_->SetScalarParameterValue(fn, rtRimDepth_);
        }

        // OuterRimDepth: always 0 (invisible) for NVG
        {
            SDK::FName fn = SDK::BasicFilesImpleUtils::StringToName(L"OuterRimDepth");
            lensMID_->SetScalarParameterValue(fn, 0.0f);
        }

        // Log every 300 ticks (~5s at 60fps)
        if (lensDiagCounter_ % 300 == 1)
        {
            LOG_INFO("[NVG] UpdateLensMaterialParams: ImageScale=" << imageScale
                << " (auto=" << (rtImageScale_ <= 0.01f ? "YES" : "NO") << ")"
                << " RimScale=" << rtRimScale_
                << " RimDepth=" << rtRimDepth_
                << " ImageDepth=" << rtImageDepth_
                << " autoFOV=" << (autoFOV_ ? "ON" : "OFF")
                << " captureFOV=" << lensFOV_);
        }
    }

    // -----------------------------------------------------------------------
    // Setters for render target / material projection controls
    // -----------------------------------------------------------------------
    void NVGSubsystem::SetRTImageScale(float scale)
    {
        if (!std::isfinite(scale)) scale = 0.0f;
        if (scale < 0.0f) scale = 0.0f;   // 0 = auto
        if (scale > 100.0f) scale = 100.0f;
        rtImageScale_ = scale;
        LOG_INFO("[NVG] SetRTImageScale -> " << scale << " (0=auto, computed=" << ComputeAutoImageScale() << ")");
    }

    void NVGSubsystem::SetRTRimScale(float scale)
    {
        if (!std::isfinite(scale)) scale = 50.0f;
        if (scale < 0.1f) scale = 0.1f;
        if (scale > 200.0f) scale = 200.0f;
        rtRimScale_ = scale;
        LOG_INFO("[NVG] SetRTRimScale -> " << scale);
    }

    void NVGSubsystem::SetRTRimDepth(float depth)
    {
        if (!std::isfinite(depth)) depth = 0.0f;
        if (depth < -500.0f) depth = -500.0f;
        if (depth > 0.0f) depth = 0.0f;  // Only 0 or negative make sense
        rtRimDepth_ = depth;
        LOG_INFO("[NVG] SetRTRimDepth -> " << depth << " (0=no rim, negative=visible rim)");
    }

    void NVGSubsystem::SetRTImageDepth(float depth)
    {
        if (!std::isfinite(depth)) depth = -500.0f;
        if (depth < -1000.0f) depth = -1000.0f;
        if (depth > 0.0f) depth = 0.0f;
        rtImageDepth_ = depth;
        LOG_INFO("[NVG] SetRTImageDepth -> " << depth);
    }

    void NVGSubsystem::SetAutoFOV(bool enabled)
    {
        autoFOV_ = enabled;
        if (enabled && lensActive_)
        {
            float autoFov = ComputeAutoFOV();
            lensFOV_ = autoFov;
            if (lensCaptureActor_ && SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
            {
                auto* cc = static_cast<SDK::ASceneCapture2D*>(lensCaptureActor_)->CaptureComponent2D;
                if (cc)
                {
                    cc->FOVAngle = lensFOV_;
                    LOG_INFO("[NVG] SetAutoFOV: ON, applied FOV=" << autoFov);
                }
            }
        }
        else
        {
            LOG_INFO("[NVG] SetAutoFOV: " << (enabled ? "ON" : "OFF") << " (computed=" << ComputeAutoFOV() << ")");
        }
    }

    void NVGSubsystem::SetLensScale(float scale)
    {
        if (!std::isfinite(scale)) scale = 0.25f;
        if (scale < 0.01f) scale = 0.01f;
        if (scale > 10.0f) scale = 10.0f;
        lensScale_ = scale;

        if (lensActive_ && lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
        {
            // Use lensMeshType_ to decide scale layout — matching SetupLens logic exactly.
            // lensMeshType_ 0=Plane, 1=Cylinder.
            // Cylinder: uniform XYZ so the circular face stays circular after any rotation.
            // Plane: Z irrelevant (plane has no thickness), keep at 1.0.
            SDK::FVector meshScale;
            if (lensMeshType_ == 1)
            {
                meshScale = SDK::FVector{lensScale_, lensScale_, lensScale_};
                LOG_INFO("[NVG] SetLensScale (cylinder, uniform): " << lensScale_);
            }
            else
            {
                meshScale = SDK::FVector{lensScale_, lensScale_, 1.0};
                LOG_INFO("[NVG] SetLensScale (plane): XY=" << lensScale_ << " Z=1.0");
            }
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor_->SetActorRelativeScale3D(meshScale);
            // Log resulting actor scale for diagnostics
            auto actualScale = lensMeshActor_->GetActorRelativeScale3D();
            LOG_INFO("[NVG] SetLensScale applied: actor scale = "
                << actualScale.X << ", " << actualScale.Y << ", " << actualScale.Z);
        }
        else
        {
            LOG_INFO("[NVG] SetLensScale -> " << scale << " (stored; lens not active, meshType=" << lensMeshType_ << ")");
        }

        // Apply same scale to right-eye lens
        if (lensActive_ && lensMeshActor2_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor2_))
        {
            SDK::FVector meshScale2 = (lensMeshType_ == 1)
                ? SDK::FVector{lensScale_, lensScale_, lensScale_}
                : SDK::FVector{lensScale_, lensScale_, 1.0};
            Mod::ScopedProcessEventGuard guard;
            lensMeshActor2_->SetActorRelativeScale3D(meshScale2);
        }
    }

    void NVGSubsystem::SetLensFOV(float fov)
    {
        if (!std::isfinite(fov)) fov = 90.0f;
        if (fov < 10.0f) fov = 10.0f;
        if (fov > 170.0f) fov = 170.0f;
        lensFOV_ = fov;

        if (lensActive_ && lensCaptureActor_ && SDK::UKismetSystemLibrary::IsValid(lensCaptureActor_))
        {
            auto* captureComp = static_cast<SDK::ASceneCapture2D*>(lensCaptureActor_)->CaptureComponent2D;
            if (captureComp)
                captureComp->FOVAngle = fov;
        }
        LOG_INFO("[NVG] SetLensFOV -> " << fov);
    }

    void NVGSubsystem::SetLensResolution(int res)
    {
        if (res < 64) res = 64;
        if (res > 4096) res = 4096;
        lensResolution_ = res;
        LOG_INFO("[NVG] SetLensResolution -> " << res << " (requires lens restart to take effect)");
    }

    std::string NVGSubsystem::SetLensMaterialParam(const std::string& paramName, float value)
    {
        std::ostringstream oss;
        oss << "SetLensMaterialParam: ";

        if (!lensMID_ || !SDK::UKismetSystemLibrary::IsValid(lensMID_))
        {
            oss << "ERROR: no valid lens MID. Setup lens first.\n";
            return oss.str();
        }

        std::wstring wparamName(paramName.begin(), paramName.end());
        SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(wparamName.c_str());

        {
            Mod::ScopedProcessEventGuard guard;
            lensMID_->SetScalarParameterValue(fname, value);
        }

        oss << "\"" << paramName << "\" = " << value << " (OK)\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // Main update (called every tick)
    // -----------------------------------------------------------------------
    void NVGSubsystem::Update(SDK::UWorld* world)
    {
        if (!world)
            return;

        // Re-entrancy guard: ProcessEvent calls inside SetupLens can trigger
        // game ticks which call Update() again, causing infinite recursion.
        if (inUpdate_) return;
        inUpdate_ = true;
        struct UpdateGuard { bool& flag; ~UpdateGuard() { flag = false; } } updateGuard{inUpdate_};

        // Handle world change: clean up stale state
        if (lastWorld_ != world)
        {
            if (wasApplied_ && lastWorld_)
            {
                LOG_INFO("[NVG] World changed, cleaning up old NVG state");
                RemoveCameraPP(lastWorld_);
                RemoveLensMask(lastWorld_);
                RemoveGameNVG(lastWorld_);
            }
            // Tear down lens system (actors are stale after world change)
            if (lensActive_)
            {
                // Don't try to destroy actors in old world — just null pointers
                lensCaptureActor_ = nullptr;
                lensMeshActor_ = nullptr;
                lensMeshActor2_ = nullptr;
                lensRenderTarget_ = nullptr;
                lensMID_ = nullptr;
                lensActive_ = false;
                LOG_INFO("[NVG] World changed, cleared stale lens system pointers");
            }
            lastWorld_ = world;
            wasApplied_ = false;
            lensSetupFailed_ = false;  // Reset failure flag on world change
            originalCameraPP_.captured = false;
            probeComplete_ = false;
            probeFoundMaterial_ = false;
            discoveredNVGMaterial_ = nullptr;
            discoveredPPElementIndex_ = -1;
            gameNVGActive_ = false;
            nvgMid_ = nullptr;
            midApplied_ = false;
            replacedSlotOriginalObj_ = nullptr;
            replacedSlotIndex_ = -1;
            dirty_.store(true, std::memory_order_release);
        }

        const bool shouldBeActive = enabled_.load(std::memory_order_relaxed);
        const bool isDirty = dirty_.load(std::memory_order_acquire);
        const NVGMode mode = mode_.load(std::memory_order_relaxed);
        const bool isGameNVGMode = (mode == NVGMode::GameNVGOnly);
        const bool isLensMeshMode = (mode == NVGMode::LensOverlay || mode == NVGMode::LensMeshBlackout);

        // Turning OFF
        if (!shouldBeActive && wasApplied_)
        {
            LOG_INFO("[NVG] Deactivating NVG (mode=" << ModeToString(mode) << ")");
            if (isGameNVGMode)
            {
                RemoveGameNVG(world);
            }
            else
            {
                RemoveCameraPP(world);
                RemoveLensMask(world);
            }
            // Always tear down lens system when deactivating
            if (lensActive_)
            {
                TeardownLens(world);
            }
            wasApplied_ = false;
            dirty_.store(false, std::memory_order_release);
            return;
        }

        // Active: apply or reapply if dirty
        if (shouldBeActive)
        {
            if (isDirty || !wasApplied_)
            {
                // Clean up previous mode before switching
                if (wasApplied_)
                {
                    RemoveGameNVG(world);
                    RemoveCameraPP(world);
                    RemoveLensMask(world);
                    if (lensActive_)
                    {
                        TeardownLens(world);
                    }
                }

                if (isGameNVGMode)
                {
                    LOG_INFO("[NVG] Activating GameNVGOnly mode (pure SetNV, no camera PP)");
                    ApplyGameNVG(world);
                }
                else if (isLensMeshMode)
                {
                    // Mesh-based lens modes: create the lens system
                    if (lensSetupFailed_ && !isDirty)
                    {
                        // SetupLens already failed this session — don't retry every tick.
                        // Will retry on next mode change (dirty flag) or world change.
                        wasApplied_ = true;
                        dirty_.store(false, std::memory_order_release);
                        return;
                    }
                    lensSetupFailed_ = false;  // Reset on new attempt (dirty flag was set)
                    LOG_INFO("[NVG] Activating " << ModeToString(mode) << " (mesh-based NVG lens)");
                    std::string result = SetupLens(world);
                    LOG_INFO("[NVG] SetupLens result:\n" << result);

                    if (!lensActive_)
                    {
                        // SetupLens failed — mark it so we don't retry every tick
                        lensSetupFailed_ = true;
                        LOG_WARN("[NVG] SetupLens FAILED — will not retry until mode change or world change");
                    }

                    if (mode == NVGMode::LensMeshBlackout)
                    {
                        // Darken the main camera so everything outside the lens is dark.
                        // Use ApplyCameraPP to capture originals, then override to moderate dark.
                        // We use moderate ColorGain (not extreme) because the Unlit lens mesh
                        // also goes through camera PP. BrightnessMult on the lens MID compensates.
                        ApplyCameraPP(world);
                        auto* camera = FindPlayerCamera(world);
                        if (camera)
                        {
                            auto& pp = camera->PostProcessSettings;
                            pp.ColorSaturation = SDK::FVector4{0.0, 0.0, 0.0, 1.0};  // grayscale
                            pp.ColorGain = SDK::FVector4{0.1, 0.1, 0.1, 1.0};         // 10% brightness
                            pp.ColorOffset = SDK::FVector4{0.0, 0.0, 0.0, 0.0};
                            pp.AutoExposureBias = 0.0f;  // neutral (no extreme darkening)
                            pp.BloomIntensity = 0.0f;
                            pp.FilmGrainIntensity = 0.0f;
                            pp.VignetteIntensity = 0.0f;
                            LOG_INFO("[NVG] LensMeshBlackout: applied moderate dark camera PP (ColorGain=0.1)");
                        }
                        // Compensate: set BrightnessMult on lens MID so lens content is visible
                        // through the camera darkening. 1/0.1 = 10
                        if (lensMID_ && SDK::UKismetSystemLibrary::IsValid(lensMID_))
                        {
                            std::wstring paramW = L"BrightnessMult";
                            SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(paramW.c_str());
                            Mod::ScopedProcessEventGuard guard;
                            lensMID_->SetScalarParameterValue(fname, 10.0f);
                            LOG_INFO("[NVG] LensMeshBlackout: set BrightnessMult=10 on lens MID");
                        }
                    }
                    // LensOverlay: no camera PP modification — normal view outside lens
                }
                else
                {
                    // Fullscreen / LensBlackout: camera PP + optional lens blendable
                    ApplyCameraPP(world);
                    ApplyLensMask(world);
                }

                wasApplied_ = true;
                dirty_.store(false, std::memory_order_release);
            }
            else
            {
                // Continuously reapply to fight the game resetting values
                if (isGameNVGMode)
                {
                    if (!gameNVGActive_)
                    {
                        LOG_INFO("[NVG] GameNVGOnly: game cleared SetNV, reapplying");
                        ApplyGameNVG(world);
                    }
                }
                else if (isLensMeshMode)
                {
                    // Update capture PP settings each tick (in case intensity changed)
                    if (lensActive_)
                    {
                        UpdateLensCaptureSettings();
                        UpdateLensMaterialParams();

                        // For LensMeshBlackout: re-apply moderate darkening each tick
                        if (mode == NVGMode::LensMeshBlackout)
                        {
                            auto* camera = FindPlayerCamera(world);
                            if (camera)
                            {
                                auto& pp = camera->PostProcessSettings;
                                pp.ColorGain = SDK::FVector4{0.1, 0.1, 0.1, 1.0};
                                pp.AutoExposureBias = 0.0f;
                            }
                        }

                        // Periodic diagnostic logging (every ~300 ticks ≈ every 5s at 60fps)
                        lensDiagCounter_++;
                        if (lensDiagCounter_ % 300 == 1)
                        {
                            if (lensMeshActor_ && SDK::UKismetSystemLibrary::IsValid(lensMeshActor_))
                            {
                                Mod::ScopedProcessEventGuard guard;
                                auto meshLoc = lensMeshActor_->K2_GetActorLocation();
                                auto meshRot = lensMeshActor_->K2_GetActorRotation();
                                auto* cam = FindPlayerCamera(world);
                                if (cam) {
                                    auto camLoc = cam->K2_GetComponentLocation();
                                    double dx = meshLoc.X - camLoc.X;
                                    double dy = meshLoc.Y - camLoc.Y;
                                    double dz = meshLoc.Z - camLoc.Z;
                                    double dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                                    LOG_INFO("[NVG] LENS DIAG #" << lensDiagCounter_
                                        << " mesh pos=(" << meshLoc.X << "," << meshLoc.Y << "," << meshLoc.Z << ")"
                                        << " rot=P" << meshRot.Pitch << " Y" << meshRot.Yaw << " R" << meshRot.Roll
                                        << " cam=(" << camLoc.X << "," << camLoc.Y << "," << camLoc.Z << ")"
                                        << " dist=" << dist);
                                } else {
                                    LOG_INFO("[NVG] LENS DIAG #" << lensDiagCounter_
                                        << " mesh pos=(" << meshLoc.X << "," << meshLoc.Y << "," << meshLoc.Z << ")"
                                        << " rot=P" << meshRot.Pitch << " Y" << meshRot.Yaw << " R" << meshRot.Roll
                                        << " (camera not found)");
                                }
                            }
                            else
                            {
                                LOG_WARN("[NVG] LENS DIAG #" << lensDiagCounter_ << " mesh actor is INVALID/null!");
                            }
                        }
                    }
                }
                else
                {
                    ApplyCameraPP(world);
                    ApplyLensMask(world);
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // DumpMaterialParams: Read all parameters from MI_PP_NightVision
    // CRITICAL for understanding the lens circle and any stereo control params
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::DumpMaterialParams(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== MI_PP_NightVision Material Parameters ===\n";

        auto* camera = FindPlayerCamera(world);
        if (!camera)
        {
            oss << "ERROR: VR camera not found\n";
            LOG_WARN("[NVG] DumpMaterialParams: camera not found");
            return oss.str();
        }

        auto& wb = camera->PostProcessSettings.WeightedBlendables;

        // Find MI_PP_NightVision — use discovered pointer first, then scan by name
        SDK::UObject* nvgObj = nullptr;
        int nvgIdx = -1;

        if (discoveredNVGMaterial_ && SDK::UKismetSystemLibrary::IsValid(discoveredNVGMaterial_))
        {
            nvgObj = discoveredNVGMaterial_;
            nvgIdx = discoveredPPElementIndex_;
            oss << "Source: discovered via probe\n";
        }
        else
        {
            oss << "Note: probe not run — scanning by name (any weight)\n";
            for (int i = 0; i < wb.Array.Num(); i++)
            {
                auto* obj = wb.Array[i].Object;
                if (!obj || !SDK::UKismetSystemLibrary::IsValid(obj)) continue;
                std::string n = obj->GetName();
                if (n.find("NightVision") != std::string::npos ||
                    n.find("nightvision") != std::string::npos)
                {
                    nvgObj = obj;
                    nvgIdx = i;
                    break;
                }
            }
        }

        if (!nvgObj)
        {
            oss << "ERROR: MI_PP_NightVision not found. Run nvg_probe first.\n";
            LOG_WARN("[NVG] DumpMaterialParams: material not found");
            return oss.str();
        }

        oss << "Blendable index: " << nvgIdx << "\n";
        if (nvgIdx >= 0 && nvgIdx < wb.Array.Num())
            oss << "Current weight: " << wb.Array[nvgIdx].Weight << "\n";
        oss << "Class: " << nvgObj->Class->GetName() << "\n";
        oss << "Name: " << nvgObj->GetName() << "\n";

        // Cast UObject* -> UMaterialInstance* (class is MaterialInstanceConstant which IS a UMaterialInstance)
        auto* matInst = static_cast<SDK::UMaterialInstance*>(nvgObj);

        if (matInst->Parent && SDK::UKismetSystemLibrary::IsValid(matInst->Parent))
        {
            oss << "Parent material: " << matInst->Parent->GetName() << "\n";
        }

        // Scalar parameters
        oss << "\nScalarParameters[" << matInst->ScalarParameterValues.Num() << "]:\n";
        for (int i = 0; i < matInst->ScalarParameterValues.Num(); i++)
        {
            auto& param = matInst->ScalarParameterValues[i];
            SDK::FString fname;
            {
                Mod::ScopedProcessEventGuard guard;
                fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name);
            }
            oss << "  [" << i << "] \"" << fname.ToString() << "\" = " << param.ParameterValue << "\n";
        }

        // Vector parameters
        oss << "\nVectorParameters[" << matInst->VectorParameterValues.Num() << "]:\n";
        for (int i = 0; i < matInst->VectorParameterValues.Num(); i++)
        {
            auto& param = matInst->VectorParameterValues[i];
            SDK::FString fname;
            {
                Mod::ScopedProcessEventGuard guard;
                fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name);
            }
            auto& c = param.ParameterValue;
            oss << "  [" << i << "] \"" << fname.ToString() << "\" = ("
                << c.R << ", " << c.G << ", " << c.B << ", " << c.A << ")\n";
        }

        // Texture parameters
        oss << "\nTextureParameters[" << matInst->TextureParameterValues.Num() << "]:\n";
        for (int i = 0; i < matInst->TextureParameterValues.Num(); i++)
        {
            auto& param = matInst->TextureParameterValues[i];
            SDK::FString fname;
            {
                Mod::ScopedProcessEventGuard guard;
                fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name);
            }
            std::string texName = "null";
            if (param.ParameterValue && SDK::UKismetSystemLibrary::IsValid(param.ParameterValue))
                texName = param.ParameterValue->GetName();
            oss << "  [" << i << "] \"" << fname.ToString() << "\" = " << texName << "\n";
        }

        std::string result = oss.str();
        LOG_INFO("[NVG] DumpMaterialParams:\n" << result);
        return result;
    }

    // -----------------------------------------------------------------------
    // DumpMobileSubsystem: Dump UMobilePostProcessSubsystem NV fields
    // The game calls GetEngineSubsystem in BPC_PlayerPostProcess for NV effects.
    // On PC (non-mobile) the game's BPC skips the mobile path but the subsystem
    // still exists. Helpful to monitor if SetNV modifies these values on PC.
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::DumpMobileSubsystem()
    {
        std::ostringstream oss;
        oss << "=== MobilePostProcessSubsystem ===\n";

        Mod::ScopedProcessEventGuard guard;
        auto* cls = SDK::UMobilePostProcessSubsystem::StaticClass();
        if (!cls)
        {
            oss << "ERROR: StaticClass() is null (class not found in GObjects)\n";
            LOG_WARN("[NVG] DumpMobileSubsystem: StaticClass null");
            return oss.str();
        }

        auto* sub = static_cast<SDK::UMobilePostProcessSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetEngineSubsystem(cls));

        if (!sub)
        {
            oss << "ERROR: GetEngineSubsystem returned null\n";
            oss << "       (Subsystem not created — likely PC non-mobile path)\n";
            LOG_WARN("[NVG] DumpMobileSubsystem: subsystem null");
            return oss.str();
        }

        oss << "  NightVisionFactor          = " << sub->NightVisionFactor << "\n";
        oss << "  NightVisionInputLumMin     = " << sub->NightVisionInputLuminanceMin << "\n";
        oss << "  NightVisionInputLumMax     = " << sub->NightVisionInputLuminanceMax << "\n";
        oss << "  NightVisionOutputLumMin    = " << sub->NightVisionOutputLuminanceMin << "\n";
        oss << "  NightVisionOutputLumMax    = " << sub->NightVisionOutputLuminanceMax << "\n";
        oss << "  VignetteRadius             = " << sub->VignetteRadius << "\n";
        oss << "  BlinkRadius                = " << sub->BlinkRadius << "\n";
        oss << "  FadeToBlack                = " << sub->FadeToBlack << "\n";
        oss << "  bGasMaskOn                 = " << (int)sub->bGasMaskOn << "\n";
        oss << "  RecoilFactor               = " << sub->RecoilFactor << "\n";
        oss << "  HealFactor                 = " << sub->HealFactor << "\n";
        oss << "  DeadFactor                 = " << sub->DeadFactor << "\n";
        oss << "  LowHealthFactor            = " << sub->LowHealthFactor << "\n";
        oss << "  Saturation                 = " << sub->Saturation << "\n";
        oss << "  Contrast                   = " << sub->Contrast << "\n";
        oss << "  UnderwaterFactor           = " << sub->UnderwaterFactor << "\n";
        oss << "  bIsTideEnabled             = " << (int)sub->bIsTideEnabled << "\n";

        std::string result = oss.str();
        LOG_INFO("[NVG] DumpMobileSubsystem:\n" << result);
        return result;
    }

    // -----------------------------------------------------------------------
    // CreateAndApplyNVGMid: Create UMaterialInstanceDynamic from MI_PP_NightVision
    // and add it as an ADDITIONAL entry in the camera blendables.
    //
    // Hypothesis: MID may render to both eyes differently than the static MIC.
    // Also allows runtime parameter modification (SetMIDScalarParam etc).
    // After creation, use nvg_dump_mat_params and nvg_mid_param to experiment.
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::CreateAndApplyNVGMid(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== CreateAndApplyNVGMid ===\n";

        if (!world) { oss << "ERROR: world null\n"; return oss.str(); }

        // Require probe to have been run
        if (!discoveredNVGMaterial_ || !SDK::UKismetSystemLibrary::IsValid(discoveredNVGMaterial_))
        {
            oss << "ERROR: discoveredNVGMaterial_ is null. Run nvg_probe first.\n";
            LOG_WARN("[NVG] CreateAndApplyNVGMid: probe not run");
            return oss.str();
        }

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        // Remove existing MID if any
        if (nvgMid_) RemoveNVGMid(world);

        // Create the MID
        oss << "Parent material: " << discoveredNVGMaterial_->GetName() << "\n";
        oss << "Calling UKismetMaterialLibrary::CreateDynamicMaterialInstance...\n";

        SDK::UMaterialInstanceDynamic* mid = nullptr;
        {
            Mod::ScopedProcessEventGuard guard;
            mid = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
                world,
                static_cast<SDK::UMaterialInterface*>(discoveredNVGMaterial_),
                SDK::FName{},
                SDK::EMIDCreationFlags::Transient);
        }

        if (!mid)
        {
            oss << "ERROR: CreateDynamicMaterialInstance returned null\n";
            LOG_WARN("[NVG] CreateAndApplyNVGMid: CreateDynamicMaterialInstance failed");
            return oss.str();
        }

        nvgMid_ = mid;
        oss << "MID created: " << mid->GetName() << "\n";

        // Log its initial scalar params (should mirror the parent)
        auto* matInst = static_cast<SDK::UMaterialInstance*>(mid);
        oss << "MID ScalarParams[" << matInst->ScalarParameterValues.Num() << "]:\n";
        for (int i = 0; i < matInst->ScalarParameterValues.Num(); i++)
        {
            auto& param = matInst->ScalarParameterValues[i];
            SDK::FString fname;
            {
                Mod::ScopedProcessEventGuard guard;
                fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name);
            }
            oss << "  [" << i << "] \"" << fname.ToString() << "\" = " << param.ParameterValue << "\n";
        }

        // Add MID to camera blendables with weight=1
        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        SDK::FWeightedBlendable entry;
        entry.Weight = 1.0f;
        entry.Object = mid;
        wb.Array.Add(entry);
        midApplied_ = true;

        oss << "MID added to WeightedBlendables (now " << wb.Array.Num() << " entries total)\n";
        oss << "Current blendables: " << ReadBlendables(camera) << "\n";
        oss << "\nNOTE: If this renders right-eye-only same as the original, the stereo issue\n"
            << "is in the shader code (StereoPassIndex check), not in the instance type.\n"
            << "Use nvg_mid_param <name> <value> to set parameters if dump revealed any stereo controls.\n";

        LOG_INFO("[NVG] CreateAndApplyNVGMid:\n" << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // RemoveNVGMid: Remove the MID from camera blendables and clear state
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::RemoveNVGMid(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "RemoveNVGMid: ";

        if (!nvgMid_)
        {
            oss << "no MID to remove\n";
            return oss.str();
        }

        if (world)
        {
            auto* camera = FindPlayerCamera(world);
            if (camera)
            {
                auto& wb = camera->PostProcessSettings.WeightedBlendables;
                int removed = 0;
                for (int i = wb.Array.Num() - 1; i >= 0; i--)
                {
                    if (wb.Array[i].Object == nvgMid_)
                    {
                        // Set weight=0 rather than removing to avoid array corruption
                        wb.Array[i].Weight = 0.0f;
                        wb.Array[i].Object = nullptr;
                        removed++;
                    }
                }
                oss << "removed " << removed << " MID entries from blendables\n";
                oss << "Remaining blendables: " << ReadBlendables(camera) << "\n";
            }
        }

        nvgMid_ = nullptr;
        midApplied_ = false;
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // SetMIDScalarParam: Set a scalar parameter on the stored NVG MID
    // Use nvg_dump_mat_params to find parameter names first
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::SetMIDScalarParam(const std::string& paramName, float value)
    {
        std::ostringstream oss;
        oss << "SetMIDScalarParam: ";

        if (!nvgMid_ || !SDK::UKismetSystemLibrary::IsValid(nvgMid_))
        {
            oss << "ERROR: no valid MID. Run nvg_create_mid first.\n";
            return oss.str();
        }

        // Convert std::string param name to FName
        std::wstring wparamName(paramName.begin(), paramName.end());
        SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(wparamName.c_str());

        {
            Mod::ScopedProcessEventGuard guard;
            nvgMid_->SetScalarParameterValue(fname, value);
        }

        oss << "\"" << paramName << "\" = " << value << " (OK)\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // SetMIDVectorParam: Set a vector (linear color) parameter on the MID
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::SetMIDVectorParam(const std::string& paramName, float r, float g, float b, float a)
    {
        std::ostringstream oss;
        oss << "SetMIDVectorParam: ";

        if (!nvgMid_ || !SDK::UKismetSystemLibrary::IsValid(nvgMid_))
        {
            oss << "ERROR: no valid MID. Run nvg_create_mid first.\n";
            return oss.str();
        }

        std::wstring wparamName(paramName.begin(), paramName.end());
        SDK::FName fname = SDK::BasicFilesImpleUtils::StringToName(wparamName.c_str());
        SDK::FLinearColor col = {r, g, b, a};

        {
            Mod::ScopedProcessEventGuard guard;
            nvgMid_->SetVectorParameterValue(fname, col);
        }

        oss << "\"" << paramName << "\" = (" << r << "," << g << "," << b << "," << a << ") (OK)\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // DumpAllBlendableParams: read params for ALL camera blendable materials
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::DumpAllBlendableParams(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== ALL Blendable Material Parameters ===\n";

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        oss << "Total blendables: " << wb.Array.Num() << "\n";

        for (int i = 0; i < wb.Array.Num(); i++)
        {
            auto* obj = wb.Array[i].Object;
            float w = wb.Array[i].Weight;

            oss << "\n--- [" << i << "] weight=" << w << " ---\n";

            if (!obj) { oss << "  (null)\n"; continue; }
            if (!SDK::UKismetSystemLibrary::IsValid(obj)) { oss << "  (INVALID)\n"; continue; }

            std::string className = obj->Class->GetName();
            std::string objName = obj->GetName();
            oss << "  Class: " << className << "\n";
            oss << "  Name: " << objName << "\n";

            if (className.find("MaterialInstance") != std::string::npos)
            {
                auto* matInst = static_cast<SDK::UMaterialInstance*>(obj);

                if (matInst->Parent && SDK::UKismetSystemLibrary::IsValid(matInst->Parent))
                    oss << "  Parent: " << matInst->Parent->GetName() << "\n";

                oss << "  Scalars[" << matInst->ScalarParameterValues.Num() << "]:";
                for (int j = 0; j < matInst->ScalarParameterValues.Num(); j++)
                {
                    auto& param = matInst->ScalarParameterValues[j];
                    SDK::FString fname;
                    { Mod::ScopedProcessEventGuard guard; fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name); }
                    oss << " " << fname.ToString() << "=" << param.ParameterValue;
                }
                oss << "\n";

                oss << "  Vectors[" << matInst->VectorParameterValues.Num() << "]:";
                for (int j = 0; j < matInst->VectorParameterValues.Num(); j++)
                {
                    auto& param = matInst->VectorParameterValues[j];
                    SDK::FString fname;
                    { Mod::ScopedProcessEventGuard guard; fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name); }
                    auto& c = param.ParameterValue;
                    oss << " " << fname.ToString() << "=(" << c.R << "," << c.G << "," << c.B << "," << c.A << ")";
                }
                oss << "\n";

                oss << "  Textures[" << matInst->TextureParameterValues.Num() << "]:";
                for (int j = 0; j < matInst->TextureParameterValues.Num(); j++)
                {
                    auto& param = matInst->TextureParameterValues[j];
                    SDK::FString fname;
                    { Mod::ScopedProcessEventGuard guard; fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name); }
                    std::string texName = (param.ParameterValue && SDK::UKismetSystemLibrary::IsValid(param.ParameterValue))
                        ? param.ParameterValue->GetName() : "null";
                    oss << " " << fname.ToString() << "=" << texName;
                }
                oss << "\n";
            }
            else
            {
                oss << "  (Material base — no instance params to read)\n";
            }
        }

        std::string result = oss.str();
        LOG_INFO("[NVG] DumpAllBlendableParams:\n" << result);
        return result;
    }

    // -----------------------------------------------------------------------
    // WriteMobileSubsystem: directly write to UMobilePostProcessSubsystem
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::WriteMobileSubsystem(const std::string& field, float value)
    {
        std::ostringstream oss;
        oss << "WriteMobileSubsystem: ";

        Mod::ScopedProcessEventGuard guard;
        auto* sub = static_cast<SDK::UMobilePostProcessSubsystem*>(
            SDK::USubsystemBlueprintLibrary::GetEngineSubsystem(
                SDK::UMobilePostProcessSubsystem::StaticClass()));

        if (!sub) { oss << "ERROR: subsystem null\n"; return oss.str(); }

        if (field == "NightVisionFactor" || field == "nvfactor") {
            float old = sub->NightVisionFactor; sub->NightVisionFactor = value;
            oss << "NightVisionFactor: " << old << " -> " << value;
        }
        else if (field == "VignetteRadius" || field == "vigradius") {
            float old = sub->VignetteRadius; sub->VignetteRadius = value;
            oss << "VignetteRadius: " << old << " -> " << value;
        }
        else if (field == "NightVisionInputLumMin" || field == "nvilmin") {
            float old = sub->NightVisionInputLuminanceMin; sub->NightVisionInputLuminanceMin = value;
            oss << "NightVisionInputLuminanceMin: " << old << " -> " << value;
        }
        else if (field == "NightVisionInputLumMax" || field == "nvilmax") {
            float old = sub->NightVisionInputLuminanceMax; sub->NightVisionInputLuminanceMax = value;
            oss << "NightVisionInputLuminanceMax: " << old << " -> " << value;
        }
        else if (field == "NightVisionOutputLumMin" || field == "nvolmin") {
            float old = sub->NightVisionOutputLuminanceMin; sub->NightVisionOutputLuminanceMin = value;
            oss << "NightVisionOutputLuminanceMin: " << old << " -> " << value;
        }
        else if (field == "NightVisionOutputLumMax" || field == "nvolmax") {
            float old = sub->NightVisionOutputLuminanceMax; sub->NightVisionOutputLuminanceMax = value;
            oss << "NightVisionOutputLuminanceMax: " << old << " -> " << value;
        }
        else if (field == "Saturation" || field == "sat") {
            float old = sub->Saturation; sub->Saturation = value;
            oss << "Saturation: " << old << " -> " << value;
        }
        else if (field == "Contrast" || field == "con") {
            float old = sub->Contrast; sub->Contrast = value;
            oss << "Contrast: " << old << " -> " << value;
        }
        else if (field == "BlinkRadius" || field == "blink") {
            float old = sub->BlinkRadius; sub->BlinkRadius = value;
            oss << "BlinkRadius: " << old << " -> " << value;
        }
        else if (field == "FadeToBlack" || field == "fade") {
            float old = sub->FadeToBlack; sub->FadeToBlack = value;
            oss << "FadeToBlack: " << old << " -> " << value;
        }
        else {
            oss << "Unknown: " << field << "\nValid: nvfactor vigradius nvilmin nvilmax nvolmin nvolmax sat con blink fade";
        }

        oss << "\n";
        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // MIDReplaceSlot: create MID and REPLACE an existing blendable slot
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::MIDReplaceSlot(SDK::UWorld* world, int index)
    {
        std::ostringstream oss;
        oss << "=== MIDReplaceSlot (index=" << index << ") ===\n";

        if (!world) { oss << "ERROR: world null\n"; return oss.str(); }

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        if (index < 0 || index >= wb.Array.Num())
        {
            oss << "ERROR: index " << index << " out of range [0," << wb.Array.Num() << ")\n";
            return oss.str();
        }

        auto* originalObj = wb.Array[index].Object;
        if (!originalObj || !SDK::UKismetSystemLibrary::IsValid(originalObj))
        {
            oss << "ERROR: slot " << index << " has null/invalid object\n";
            return oss.str();
        }

        oss << "Original: " << originalObj->GetName() << " (" << originalObj->Class->GetName() << ")\n";

        // Restore previous replacement if any
        if (replacedSlotIndex_ >= 0)
        {
            oss << "Restoring previous slot " << replacedSlotIndex_ << " first\n";
            MIDRestoreSlot(world);
        }

        if (nvgMid_) { nvgMid_ = nullptr; midApplied_ = false; }

        oss << "Creating MID from " << originalObj->GetName() << "...\n";
        SDK::UMaterialInstanceDynamic* mid = nullptr;
        {
            Mod::ScopedProcessEventGuard guard;
            mid = SDK::UKismetMaterialLibrary::CreateDynamicMaterialInstance(
                world,
                static_cast<SDK::UMaterialInterface*>(originalObj),
                SDK::FName{},
                SDK::EMIDCreationFlags::Transient);
        }

        if (!mid)
        {
            oss << "ERROR: CreateDynamicMaterialInstance returned null\n";
            return oss.str();
        }

        oss << "MID created: " << mid->GetName() << "\n";

        auto* midInst = static_cast<SDK::UMaterialInstance*>(mid);
        oss << "MID ScalarParams[" << midInst->ScalarParameterValues.Num() << "]:";
        for (int j = 0; j < midInst->ScalarParameterValues.Num(); j++)
        {
            auto& param = midInst->ScalarParameterValues[j];
            SDK::FString fname;
            { Mod::ScopedProcessEventGuard guard; fname = SDK::UKismetStringLibrary::Conv_NameToString(param.ParameterInfo.Name); }
            oss << " " << fname.ToString() << "=" << param.ParameterValue;
        }
        oss << "\n";

        replacedSlotOriginalObj_ = originalObj;
        replacedSlotIndex_ = index;
        nvgMid_ = mid;
        midApplied_ = true;

        wb.Array[index].Object = mid;
        wb.Array[index].Weight = 1.0f;

        oss << "Slot " << index << " REPLACED: Object=MID, Weight=1\n";
        oss << "Blendables now:\n" << ReadBlendables(camera) << "\n";
        oss << "\nIs the NVG lens on BOTH eyes? If yes, MID avoids stereo check!\n"
            << "Use nvg_mid_param to tweak. nvg_mid_undo to restore.\n";

        LOG_INFO("[NVG] MIDReplaceSlot:\n" << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // MIDRestoreSlot: restore original object in MID-replaced slot
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::MIDRestoreSlot(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "MIDRestoreSlot: ";

        if (replacedSlotIndex_ < 0)
        {
            oss << "no slot to restore\n";
            return oss.str();
        }

        auto* camera = FindPlayerCamera(world);
        if (camera)
        {
            auto& wb = camera->PostProcessSettings.WeightedBlendables;
            if (replacedSlotIndex_ < wb.Array.Num())
            {
                wb.Array[replacedSlotIndex_].Object = replacedSlotOriginalObj_;
                wb.Array[replacedSlotIndex_].Weight = 0.0f;
                oss << "restored slot " << replacedSlotIndex_ << " to "
                    << (replacedSlotOriginalObj_ ? replacedSlotOriginalObj_->GetName() : "null")
                    << " weight=0\n";
            }
        }

        nvgMid_ = nullptr;
        midApplied_ = false;
        replacedSlotOriginalObj_ = nullptr;
        replacedSlotIndex_ = -1;

        LOG_INFO("[NVG] " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // SetCameraPPField: directly set a camera PP field by name
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::SetCameraPPField(SDK::UWorld* world, const std::string& field, float value)
    {
        std::ostringstream oss;

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        auto& pp = camera->PostProcessSettings;

        if (field == "VignetteIntensity" || field == "vig") {
            pp.bOverride_VignetteIntensity = 1;
            float old = pp.VignetteIntensity; pp.VignetteIntensity = value;
            oss << "VignetteIntensity: " << old << " -> " << value;
        }
        else if (field == "AutoExposureBias" || field == "aeb") {
            pp.bOverride_AutoExposureBias = 1;
            float old = pp.AutoExposureBias; pp.AutoExposureBias = value;
            oss << "AutoExposureBias: " << old << " -> " << value;
        }
        else if (field == "BloomIntensity" || field == "bloom") {
            pp.bOverride_BloomIntensity = 1;
            float old = pp.BloomIntensity; pp.BloomIntensity = value;
            oss << "BloomIntensity: " << old << " -> " << value;
        }
        else if (field == "FilmGrainIntensity" || field == "grain") {
            pp.bOverride_FilmGrainIntensity = 1;
            float old = pp.FilmGrainIntensity; pp.FilmGrainIntensity = value;
            oss << "FilmGrainIntensity: " << old << " -> " << value;
        }
        else if (field == "SceneFringeIntensity" || field == "fringe") {
            pp.bOverride_SceneFringeIntensity = 1;
            float old = pp.SceneFringeIntensity; pp.SceneFringeIntensity = value;
            oss << "SceneFringeIntensity: " << old << " -> " << value;
        }
        else if (field == "FilmToe" || field == "toe") {
            pp.bOverride_FilmToe = 1;
            float old = pp.FilmToe; pp.FilmToe = value;
            oss << "FilmToe: " << old << " -> " << value;
        }
        else if (field == "FilmSlope" || field == "slope") {
            pp.bOverride_FilmSlope = 1;
            float old = pp.FilmSlope; pp.FilmSlope = value;
            oss << "FilmSlope: " << old << " -> " << value;
        }
        else if (field == "ColorGainG" || field == "green") {
            pp.bOverride_ColorGain = 1;
            float old = pp.ColorGain.Y; pp.ColorGain.Y = value;
            oss << "ColorGain.Y(green): " << old << " -> " << value;
        }
        else if (field == "ColorGainR" || field == "red") {
            pp.bOverride_ColorGain = 1;
            float old = pp.ColorGain.X; pp.ColorGain.X = value;
            oss << "ColorGain.X(red): " << old << " -> " << value;
        }
        else if (field == "ColorGainB" || field == "blue") {
            pp.bOverride_ColorGain = 1;
            float old = pp.ColorGain.Z; pp.ColorGain.Z = value;
            oss << "ColorGain.Z(blue): " << old << " -> " << value;
        }
        else if (field == "Sharpen" || field == "sharp") {
            pp.bOverride_Sharpen = 1;
            float old = pp.Sharpen; pp.Sharpen = value;
            oss << "Sharpen: " << old << " -> " << value;
        }
        else if (field == "PostProcessBlendWeight" || field == "ppw") {
            float old = camera->PostProcessBlendWeight; camera->PostProcessBlendWeight = value;
            oss << "PostProcessBlendWeight: " << old << " -> " << value;
        }
        else if (field == "WhiteTemp" || field == "temp") {
            pp.bOverride_WhiteTemp = 1;
            float old = pp.WhiteTemp; pp.WhiteTemp = value;
            oss << "WhiteTemp: " << old << " -> " << value;
        }
        else {
            oss << "Unknown: " << field << "\nValid: vig aeb bloom grain fringe toe slope green red blue sharp ppw temp";
        }

        oss << "\n";
        LOG_INFO("[NVG] SetCameraPPField: " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // DumpCameraPP: dump NVG-relevant camera PP values
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::DumpCameraPP(SDK::UWorld* world)
    {
        std::ostringstream oss;
        oss << "=== Camera PostProcess Settings ===\n";

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        auto& pp = camera->PostProcessSettings;

        oss << "PostProcessBlendWeight: " << camera->PostProcessBlendWeight << "\n";
        oss << "ColorSaturation: (" << pp.ColorSaturation.X << "," << pp.ColorSaturation.Y
            << "," << pp.ColorSaturation.Z << "," << pp.ColorSaturation.W << ")\n";
        oss << "ColorGain: (" << pp.ColorGain.X << "," << pp.ColorGain.Y
            << "," << pp.ColorGain.Z << "," << pp.ColorGain.W << ")\n";
        oss << "ColorGamma: (" << pp.ColorGamma.X << "," << pp.ColorGamma.Y
            << "," << pp.ColorGamma.Z << "," << pp.ColorGamma.W << ")\n";
        oss << "ColorOffset: (" << pp.ColorOffset.X << "," << pp.ColorOffset.Y
            << "," << pp.ColorOffset.Z << "," << pp.ColorOffset.W << ")\n";
        oss << "SceneColorTint: (" << pp.SceneColorTint.R << "," << pp.SceneColorTint.G
            << "," << pp.SceneColorTint.B << "," << pp.SceneColorTint.A << ")\n";
        oss << "ColorGammaShadows: (" << pp.ColorGammaShadows.X << "," << pp.ColorGammaShadows.Y
            << "," << pp.ColorGammaShadows.Z << "," << pp.ColorGammaShadows.W << ")\n";
        oss << "ColorGainShadows: (" << pp.ColorGainShadows.X << "," << pp.ColorGainShadows.Y
            << "," << pp.ColorGainShadows.Z << "," << pp.ColorGainShadows.W << ")\n";
        oss << "FilmSlope=" << pp.FilmSlope << " FilmToe=" << pp.FilmToe
            << " FilmShoulder=" << pp.FilmShoulder << "\n";
        oss << "AutoExposureMethod: " << (int)pp.AutoExposureMethod << "\n";
        oss << "AutoExposureBias: " << pp.AutoExposureBias << "\n";
        oss << "AutoExposureMin/Max: " << pp.AutoExposureMinBrightness
            << "/" << pp.AutoExposureMaxBrightness << "\n";
        oss << "BloomIntensity=" << pp.BloomIntensity << " BloomThreshold=" << pp.BloomThreshold << "\n";
        oss << "VignetteIntensity: " << pp.VignetteIntensity << "\n";
        oss << "FilmGrainIntensity: " << pp.FilmGrainIntensity << "\n";
        oss << "SceneFringeIntensity: " << pp.SceneFringeIntensity << "\n";
        oss << "AmbientOcclusionIntensity: " << pp.AmbientOcclusionIntensity << "\n";
        oss << "MotionBlurAmount: " << pp.MotionBlurAmount << "\n";
        oss << "Sharpen: " << pp.Sharpen << "\n";
        oss << "WhiteTemp: " << pp.WhiteTemp << "\n";

        LOG_INFO("[NVG] DumpCameraPP:\n" << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // ActivateBlendable: set weight on any blendable by index
    // -----------------------------------------------------------------------
    std::string NVGSubsystem::ActivateBlendable(SDK::UWorld* world, int index, float weight)
    {
        std::ostringstream oss;

        auto* camera = FindPlayerCamera(world);
        if (!camera) { oss << "ERROR: camera not found\n"; return oss.str(); }

        auto& wb = camera->PostProcessSettings.WeightedBlendables;
        if (index < 0 || index >= wb.Array.Num())
        {
            oss << "ERROR: index " << index << " out of range [0," << wb.Array.Num() << ")\n";
            return oss.str();
        }

        float oldWeight = wb.Array[index].Weight;
        wb.Array[index].Weight = weight;

        std::string name = "unknown";
        if (wb.Array[index].Object && SDK::UKismetSystemLibrary::IsValid(wb.Array[index].Object))
            name = wb.Array[index].Object->GetName();

        oss << "[" << index << "] " << name << ": weight " << oldWeight << " -> " << weight << "\n";
        oss << ReadBlendables(camera) << "\n";

        LOG_INFO("[NVG] ActivateBlendable: " << oss.str());
        return oss.str();
    }

    // -----------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------
    void NVGSubsystem::Shutdown()
    {
        LOG_INFO("[NVG] Shutdown called");

        if (lastWorld_)
        {
            if (wasApplied_)
            {
                RemoveCameraPP(lastWorld_);
                RemoveLensMask(lastWorld_);
            }
            RemoveGameNVG(lastWorld_);  // Always try to clean up game NVG
            if (lensActive_) TeardownLens(lastWorld_);  // Clean up lens system
            if (nvgMid_) RemoveNVGMid(lastWorld_);  // Clean up MID if active
            if (replacedSlotIndex_ >= 0) MIDRestoreSlot(lastWorld_);  // Restore replaced slot
        }

        enabled_.store(false, std::memory_order_relaxed);
        wasApplied_ = false;
        originalCameraPP_.captured = false;
        probeComplete_ = false;
        probeFoundMaterial_ = false;
        discoveredNVGMaterial_ = nullptr;
        discoveredPPElementIndex_ = -1;
        gameNVGActive_ = false;
        nvgMid_ = nullptr;
        midApplied_ = false;
        replacedSlotOriginalObj_ = nullptr;
        replacedSlotIndex_ = -1;
        lensCaptureActor_ = nullptr;
        lensMeshActor_ = nullptr;
        lensRenderTarget_ = nullptr;
        lensMID_ = nullptr;
        lensActive_ = false;
        lensSetupFailed_ = false;
        inUpdate_ = false;
        lastWorld_ = nullptr;
        logCounter_ = 0;
    }
}
