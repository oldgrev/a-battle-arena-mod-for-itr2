# NVG Future Plans & Research

## 1. Per-Eye Lens Rendering (Stereo Fix)

**Problem**: Single lens mesh + single capture camera means both eyes see the same rendered texture from the same position. Each eye is offset ±3cm from center, causing dark edge encroachment from opposite sides.

**Solution**: Two separate lens meshes, each rendered only to one eye.

**Approaches**:
- **IStereoRenderTargetManager**: UE's built-in stereo layer system. Would need to hook into the VR compositor.
- **Eye-index visibility**: UE5 supports `UPrimitiveComponent::SetVisibleInStereoPass()` or similar. Set one mesh visible only in left eye, the other only in right.
- **Per-eye capture cameras**: Two SceneCaptureComponent2D actors, each offset by the inter-pupillary distance (±3.2cm on Y axis). Left capture feeds left lens, right capture feeds right lens.
- **Stereo Layer**: UE's `UStereoLayerComponent` can render quads directly into the VR compositor. Could attach two stereo layers, one per eye, each with its own render target.

**Complexity**: Medium-high. Requires careful sync of two capture cameras and proper stereo eye indexing.

---

## 2. Helmet Rail NVG Activation

**Problem**: Currently NVG is toggled manually via menu/command. The game has helmet rail mounting (HeadSlotSocket, BPC_Head_Slot) where items like the PVS-14 can be physically attached.

**Goal**: When a PVS-14 (or similar NVG item) is attached to the helmet rail → automatically enable NVG lens mode. When detached → disable.

**Approach**:
- Hook into `On_Ancestor_Changed` or `OnAttachToWeapon`-style events on the head slot
- Poll HeadSlotSocket children each tick for known NVG item class names (e.g., `BP_PVS14_C`, `BP_GPNVG18_C`)
- When NVG item detected attached → call `SetMode(2)` + `Toggle(true)`
- When NVG item detached → `Toggle(false)`

**Complexity**: Low-medium. Main challenge is identifying the exact class names and attachment events.

---

## 3. Weapon Scope NVG Mode

### Research Findings

**BPA_Scope_C** (base class for all weapon scopes):
- Has `SceneCaptureComponent2D` at offset **0x0840**
- Has `LensMesh` (UStaticMeshComponent*) at **0x0878**
- Has `LensMatInst` (UMaterialInstanceDynamic*) at **0x0858**
- Has `LensSourceMat` (UMaterialInterface*) at **0x0850**
- Has `LensMatID` (int32) at **0x0848**
- Methods: `OnAttachToWeapon`, `OnDetachFromWeapon`, `DoesBelongToLocalPlayer`, `ReceiveBeginPlay`

**BP_Scope_Magnifier_C** (the specific magnifier scope):
- Inherits from BPA_Scope_C
- FOVAngle: **10.0** (vs BPA_Scope base: 2.0)
- CustomNearClippingPlane: **11.0**
- Has fewer ShowFlagSettings disabled than the PSO (no PP/Bloom/DeferredLighting/LocalExposure/EyeAdaptation disabled)
- Only disables: InstancedGrass, Paper2DSprites, AntiAliasing

**MI_Lens_Magnifer** (the material we're already using for NVG lens):
- Parent: M_Lens (`/Game/ITR1/ART/Weapons/PSO/M_Lens`)
- Key params: GridDepth=-20, GridScale=0, ImageDepth=-122.197, ImageScale=6.359, RimScale=3.122, RimDepth=-100
- GlowingGridPart: false (no crosshair grid)
- ShadingModel: Unlit, FullyRough: true

**BPA_Scope's Capture Setup** (from JSON):
- Uses `RT_PSO` render target (`/Game/ITR1/ART/Weapons/PSO/RT_PSO`)
- CaptureSource: `SCS_SceneColorHDRNoAlpha`
- `bCaptureEveryFrame`: false, `bCaptureOnMovement`: false
- Disables many show flags: InstancedGrass, Paper2DSprites, PostProcessing, Bloom, DeferredLighting, LocalExposure, EyeAdaptation, AntiAliasing

### Implementation Plan: NVG-ified Weapon Scope

**Concept**: When weapon scope NVG mode is enabled, inject NVG post-processing into the scope's existing SceneCaptureComponent2D. No need to create our own capture camera — the scope already has one.

**Steps**:
1. **Find player's equipped scope**: Walk player's gripped items → check if any are `ABPA_Scope_C` or subclass → get its `SceneCaptureComponent2D` at offset 0x0840
2. **Enable PostProcessing on scope capture**: The scope disables `PostProcessing` in ShowFlagSettings. We need to re-enable it or override individual PP values.
3. **Apply NVG PP to scope capture**: Use the same PP settings as our NVG lens capture (green tint, grain, exposure boost, saturation shift) but applied to the scope's capture component instead.
4. **Toggle scope capture on**: Scope's `bCaptureEveryFrame` is false by default. Need to set it to true (or manually trigger `CaptureScene()` each frame).
5. **Control scope NVG independently**: New command `nvg_scope_mode 0/1` to toggle NVG on the weapon scope.

**Alternative approach**: Instead of modifying the scope's PP settings (which involves re-enabling PostProcessing show flag), we could:
- Create a **second lens material** that tints the scope's render target green. The scope already uses MI_Lens_Magnifer → we could swap to an NVG-tinted variant.
- Or apply a **color grading LUT** to the scope's render target.

**Key Offsets** (from SDK headers):
```
ABPA_Scope_C::SceneCaptureComponent2D   0x0840  USceneCaptureComponent2D*
ABPA_Scope_C::LensMesh                  0x0878  UStaticMeshComponent*
ABPA_Scope_C::LensMatInst               0x0858  UMaterialInstanceDynamic*
ABPA_Scope_C::LensSourceMat             0x0850  UMaterialInterface*
ABPA_Scope_C::LensMatID                 0x0848  int32
USceneCaptureComponent2D::FOVAngle      0x030C  float
USceneCaptureComponent2D::TextureTarget 0x0328  UTextureRenderTarget2D*
USceneCaptureComponent2D::PostProcessSettings          0x0340
USceneCaptureComponent2D::PostProcessBlendWeight       0x0A60
USceneCaptureComponent::bCaptureEveryFrame             0x0232 bit 0
```

**Complexity**: Medium. The scope already has the capture pipeline. We just need to inject NVG PP into it.

---

## 4. Priority Order

1. **Per-eye lens** — Fixes the dark edge encroachment, the biggest visual artifact
2. **Weapon scope NVG** — High gameplay value, leverages existing scope capture pipeline
3. **Helmet rail activation** — Quality of life, not critical for NVG function
