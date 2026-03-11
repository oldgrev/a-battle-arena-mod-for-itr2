# NVG Test Instructions v4 — Black Square Fix

## What Changed
- **GridDepth=0**: Makes the PSOgrid layer fully transparent (was causing black square)
- **Grid texture → white**: Triple-redundancy to eliminate grid overlay
- **Rim Scale=0.7**: Circle centering fix (was 0.45, default is 0.704)
- **M_Particle alternative**: New material type 1, simple translucent (no circle mask)
- **nvg_lens_mat_type command**: Switch between M_Lens (0) and M_Particle (1)

## Quick Test (Auto Script)
```powershell
cd tools
.\test_nvg_lens_auto.ps1
```
Watch in VR while script runs. Takes ~30 seconds.

## Manual Test Steps (telnet localhost 7777)

### Test 1: Black Square Fix
```
nvg_mode 2
nvg_on
```
**Expected**: Green circle visible. **NO black square in center.** No scope crosshair/grid markings.

### Test 2: Circle Centering
Still in Mode 2 with NVG on from Test 1.
**Expected**: Circle is centered in the lens mesh. Not cut off on one side.

### Test 3: Color Restoration
```
nvg_off
```
**Expected**: World colors completely normal. No green tint, no dimming.

### Test 4: Alternative Material
```
nvg_lens_mat_type 1
nvg_mode 2
nvg_on
```
**Expected**: NVG view visible but SQUARE (no circular mask). No artifacts. Transparent edges (M_Particle is Translucent blend).

### Test 5: Switch Back
```
nvg_lens_mat_type 0
```
**Expected**: Rebuilds with M_Lens circle material after a moment. Circle appears again.

### Cleanup
```
nvg_off
```

## What to Report
1. Is the **black square gone** with M_Lens (type 0)?
2. Is the **circle centered** (not offset/cut off)?
3. Does **M_Particle** (type 1) show a clean NVG view?
4. Do **world colors restore** after `nvg_off`?
5. Does the **hand menu** still work?
6. Any **new artifacts** or visual issues?

## Log Verification
Check `itr2_mod_log.txt` for these entries confirming the fix:
- `MID scalar: GridDepth = 0` (grid layer transparent)
- `MID scalar: GridScale = 100` (grid zoomed out)
- `Found white texture:` (grid texture override found)
- `Overrode Grid texture with white texture` (override applied)
- `MID scalar: Rim Scale = 0.7` (circle centering)

## If Black Square Persists
The grid layer may not be responding to GridDepth=0. Try:
```
nvg_lens_mat GridDepth -1000
nvg_lens_mat GridScale 10000
```
Or switch to M_Particle which has no grid at all:
```
nvg_lens_mat_type 1
```

## If Circle is Still Offset
Try adjusting Rim Scale:
```
nvg_lens_mat Rim Scale 0.704
nvg_lens_mat Rim Scale 0.5
nvg_lens_mat Rim Scale 1.0
```
Report which value looks best.
