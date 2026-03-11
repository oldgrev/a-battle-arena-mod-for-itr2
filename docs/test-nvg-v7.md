# NVG Test Instructions v7 — Dark Edge Fix, Grain Fix, Helmet Hiding, Camera Offset

## What Changed in v7

### 1. Dark Edge Encroachment Fix (NEW — the big one)
- **Root Cause**: The M_Lens material has a circular rim mask (controlled by `Rim Scale` and `RimDepth`) that creates dark borders around the visible NVG circle. In VR, each eye views the lens mesh from a slightly different angle due to inter-pupillary distance (~6.4cm). This parallax makes the rim border appear shifted differently for each eye → "dark from different sides."
- **Fix — Rim elimination**: `RimDepth` now defaults to **0.0** which makes `ExponentialDensity(0)=0` → the circular rim mask is completely eliminated. The cylinder mesh geometry itself provides the circular shape.
- **Fix — Oversized painting**: `ImageScale` is now **auto-computed** from lens geometry (capture FOV / visual angle × 1.4 overscan). This ensures the render target "painting" is always bigger than the "window" (lens), so parallax shift never reveals edges.
- **Fix — Full opacity**: `ImageDepth` now defaults to **-500** (extremely opaque via ExponentialDensity) so there's no opacity falloff at edges.
- **All dynamically tuneable** via VR menu and telnet commands.

### 2. Grain & Bloom Now Respond to User Controls
- **Root Cause Fixed**: `UpdateLensCaptureSettings()` was using `lensNvgGrainIntensity_` (0.1) instead of `grain_` atomic. Now uses user controls.

### 3. Player Helmet Hidden from NVG Capture
- Components hidden via `bHiddenInSceneCapture`: HeadMesh, Head_GripArea/1, Gloves, HeadSlotSocket children, BPC_Head_Slot children

### 4. Capture Camera Offset Controls
- 3-axis offset for positioning the NVG capture camera independently
- Range: ±50cm per axis. Menu: CamOff X/Y/Z +/-. Command: `nvg_cam_offset <x> <y> <z>`

### 5. Updated Defaults
- Scale: 0.20, Distance: 34.0, Material: MI_Magnifer (type 2)
- ImageScale: auto (computed ~3.8 for current geometry)
- RimDepth: 0 (no circular rim mask)
- Rim Scale: 50 (pushed far out as insurance even if rim were enabled)
- ImageDepth: -500 (fully opaque)

---

## Test Steps (telnet localhost 7777)

### Test 1: Dark Edges Gone
```
nvg_mode 2
nvg_on
```
**Expected**: NVG lens visible. **NO dark borders from any eye angle.** The NVG "painting" should fill the entire lens without visible edges. Move your head around — both eyes should see clean green NVG content filling the circular lens.

### Test 2: Image Scale Tuning (if edges still visible)
If you still see dark edges:
```
nvg_rt_scale 0
```
**Expected**: Auto-computed ImageScale (should log the computed value). Should work well by default.
```
nvg_rt_scale 5
```
Try larger values if needed:
```
nvg_rt_scale 10
nvg_rt_scale 20
```
**Expected**: Larger values make the "painting" bigger relative to the lens. At some point edges should be completely invisible.
```
nvg_rt_scale 0
```
Reset to auto.

### Test 3: Rim Control
```
nvg_rim_depth 0
```
**Expected**: No circular border at all (just the cylinder mesh shape).
```
nvg_rim_depth -100
```
**Expected**: Sharp dark circular border appears (the original scope-style rim). You should see this shift between eyes — this was the old dark encroachment source.
```
nvg_rim_depth 0
```
Restore to no rim.

### Test 4: Auto FOV Matching
```
nvg_auto_fov 1
```
**Expected**: Capture FOV narrows to match the lens visual angle (~43°). Should give sharper/higher resolution NVG view since we're not wasting render target pixels on scene outside the lens.
```
nvg_auto_fov 0
```
Restore to manual FOV (90°).

### Test 5: Grain Responsiveness
```
nvg_grain 0.0
```
**Expected**: No grain.
```
nvg_grain 1.0
```
**Expected**: Heavy grain visible.

### Test 6: Helmet Not Visible
With NVG on, look around. **Expected**: No helmet/head mesh in NVG view.

### Test 7: VR Menu Controls
VR hand menu → NVG Lens page. New buttons:
- **ImgScale +/-**: Adjust render target scale (how big the "painting" is). Current value shown.
- **ImgScale 0**: Reset to auto-compute mode.
- **RimScale +/-**: Adjust the circular rim radius (higher = rim pushed further out).
- **Rim On/Off**: Toggle the circular rim border (OFF = no dark edge, ON = scope-style rim).
- **AutoFOV**: Toggle auto FOV matching.

### Cleanup
```
nvg_off
```

---

## Key Feedback Needed
1. **Are the dark edges completely gone?** This is the primary test. Try both eyes, move your head.
2. If edges remain, try `nvg_rt_scale 10` and report whether that fixes it.
3. Does `nvg_auto_fov 1` look noticeably sharper?
4. What ImgScale value looks best? (0=auto should be good, but worth trying 3, 5, 10)

## Known Limitations
- **Offset Y moves both eyes**: Single lens mesh means both eyes get the same Y offset. Future: two separate lens meshes with per-eye rendering.
- **Scene scale through lens**: With ImageScale auto-compute, the NVG scene should be roughly 1:1 with the real world. If things look zoomed in or out, adjust ImageScale manually.
