# NVG Lens Auto Test v5 — bHiddenInSceneCapture fix
# ROOT CAUSE: The scene capture camera was seeing the lens mesh actor itself,
#   creating a black/inverted shape in the NVG view. The hand widgets were
#   also visible to the capture camera.
# FIX: Set bHiddenInSceneCapture=true on lens mesh component AND on
#   W_GripDebug_L/R hand widget components. Restored on teardown.
# ALSO: GridDepth=0 + Grid texture override + M_Particle alternative material.
#
# Just launch the game, wait for it to load, then run this.
# Watch in VR. Check itr2_mod_log.txt after.

param(
    [string]$ITR2Host = "127.0.0.1",
    [int]$Port = 7777
)

function Send-Cmd {
    param([string]$Command)
    try {
        $client = New-Object System.Net.Sockets.TcpClient($ITR2Host, $Port)
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $reader = New-Object System.IO.StreamReader($stream)
        $writer.WriteLine($Command)
        $writer.Flush()
        $stream.ReadTimeout = 5000
        $response = $reader.ReadLine()
        $client.Close()
        Write-Host "[$(Get-Date -Format 'HH:mm:ss')] $Command => $response"
        return $response
    } catch {
        Write-Host "[$(Get-Date -Format 'HH:mm:ss')] $Command => ERROR: $_" -ForegroundColor Red
        return $null
    }
}

Write-Host "=== NVG LENS AUTO TEST v5 ===" -ForegroundColor Yellow
Write-Host "Tests: bHiddenInSceneCapture fix (lens mesh + hand widgets hidden from capture)."
Write-Host "Defaults: P180/Y270/R270, scale=0.1, dist=15, mat_type=0 (M_Lens)."
Write-Host ""

# ---------------------------------------------------------------
# TEST 1: Mode 2 (LensOverlay) with M_Lens (default, mat type 0)
# This should show a GREEN CIRCLE with NO BLACK SQUARE in the center
# ---------------------------------------------------------------
Write-Host "--- TEST 1: Mode 2 + M_Lens (type 0) — mesh hidden from capture ---" -ForegroundColor Cyan
Send-Cmd "nvg_lens_mat_type 0"
Start-Sleep -Milliseconds 300
Send-Cmd "nvg_mode 2"
Start-Sleep -Milliseconds 500
Send-Cmd "nvg_on"
Start-Sleep -Seconds 15

Write-Host "CHECK: Green NVG circle visible. NO BLACK SHAPE anywhere!"
Write-Host "CHECK: bHiddenInSceneCapture prevents lens mesh from appearing in its own capture."
Write-Host "CHECK: Hand widgets also hidden from capture (no dark rectangles)."
Send-Cmd "nvg_status"
Start-Sleep -Seconds 15

# ---------------------------------------------------------------
# TEST 2: Turn off, verify color restoration
# ---------------------------------------------------------------
Write-Host "`n--- TEST 2: Color restoration ---" -ForegroundColor Cyan
Send-Cmd "nvg_off"
Start-Sleep -Seconds 15
Write-Host "CHECK: World colors completely normal — no green tint, no dimming."
Start-Sleep -Seconds 15

# ---------------------------------------------------------------
# TEST 3: M_Particle alternative material (type 1)
# Simple translucent — no circle mask, no scope artifacts, no black square
# Shows square NVG view (no circular mask)
# ---------------------------------------------------------------
Write-Host "`n--- TEST 3: Mode 2 + M_Particle (type 1) — simple translucent ---" -ForegroundColor Cyan
Send-Cmd "nvg_lens_mat_type 1"
Start-Sleep -Milliseconds 500
Send-Cmd "nvg_mode 2"
Start-Sleep -Milliseconds 500
Send-Cmd "nvg_on"
Start-Sleep -Seconds 15

Write-Host "CHECK: NVG view visible (SQUARE, not circular — M_Particle has no rim mask)."
Write-Host "CHECK: No black square, no scope markings, no circle."
Write-Host "CHECK: Should have semi-transparent edges (Translucent blend)."
Send-Cmd "nvg_status"
Start-Sleep -Seconds 15

# ---------------------------------------------------------------
# TEST 4: Switch back to M_Lens
# ---------------------------------------------------------------
Write-Host "`n--- TEST 4: Switch back to M_Lens (type 0) ---" -ForegroundColor Cyan
Send-Cmd "nvg_lens_mat_type 0"
Start-Sleep -Seconds 15
Write-Host "CHECK: Should rebuild lens with M_Lens circle material."
Send-Cmd "nvg_status"
Start-Sleep -Seconds 15

# ---------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------
Write-Host "`n--- Cleanup ---" -ForegroundColor Cyan
Send-Cmd "nvg_off"
Start-Sleep -Seconds 15

Write-Host "`n=== AUTO TEST v5 COMPLETE ===" -ForegroundColor Yellow
Write-Host "Key results to report:" -ForegroundColor Green
Write-Host "  1. Is the BLACK SHAPE gone? (bHiddenInSceneCapture fix)" -ForegroundColor White
Write-Host "  2. Is the NVG circle clean with no artifacts?" -ForegroundColor White
Write-Host "  3. M_Particle (type 1): Clean view without black shapes?" -ForegroundColor White
Write-Host "  4. Color restoration: Normal world after NVG off?" -ForegroundColor White
Write-Host "  5. Mod menu: Hand menu visible? (test in VR)" -ForegroundColor White
Write-Host ""
Write-Host "If black shape persists: check log for 'bHiddenInSceneCapture' entries."
Write-Host "If hand widget shows in NVG: the widget offset might need adjusting."
