# NVG Lens Test Script — sends commands to the mod's TCP server
# Usage: .\test_nvg_lens.ps1
# Make sure the game is running with the mod loaded before running this.

param(
    [string]$ITR2Host = "127.0.0.1",
    [int]$Port = 7777
)

function Send-ModCommand {
    param([string]$Command)
    try {
        $client = New-Object System.Net.Sockets.TcpClient($ITR2Host, $Port)
        $stream = $client.GetStream()
        $writer = New-Object System.IO.StreamWriter($stream)
        $reader = New-Object System.IO.StreamReader($stream)
        $writer.WriteLine($Command)
        $writer.Flush()
        # Wait for response
        $stream.ReadTimeout = 5000
        $response = $reader.ReadLine()
        $client.Close()
        Write-Host "  > $Command" -ForegroundColor Cyan
        Write-Host "  < $response" -ForegroundColor Green
        return $response
    } catch {
        Write-Host "  > $Command" -ForegroundColor Cyan
        Write-Host "  ERROR: $_" -ForegroundColor Red
        return $null
    }
}

Write-Host "=== NVG Lens Test Script ===" -ForegroundColor Yellow
Write-Host "Connecting to mod on ${ITR2Host}:${Port}..." -ForegroundColor Gray

# Step 1: Current status
Write-Host "`n--- Step 1: Check current NVG status ---" -ForegroundColor Yellow
Send-ModCommand "nvg"

# Step 2: Set to mode 2 (LensOverlay) — normal view + lens overlay
Write-Host "`n--- Step 2: Set LensOverlay mode (2) ---" -ForegroundColor Yellow
Send-ModCommand "nvg_mode 2"
Start-Sleep -Milliseconds 500

# Step 3: Enable NVG (this triggers SetupLens)
Write-Host "`n--- Step 3: Enable NVG ---" -ForegroundColor Yellow
Send-ModCommand "nvg"
Start-Sleep -Seconds 10

# Step 4: Check status — should report lens=ACTIVE 
Write-Host "`n--- Step 4: Check status (lens should be ACTIVE) ---" -ForegroundColor Yellow
Send-ModCommand "nvg"

Write-Host "`n--- Step 5: Try different rotations (if default P90 doesn't work) ---" -ForegroundColor Yellow
Write-Host "Press Enter to try P90 Y0 R0 (default)..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot 90 0 0"
Start-Sleep -Seconds 10

Write-Host "Press Enter to try P-90 Y0 R0..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot -90 0 0"
Start-Sleep -Seconds 10

Write-Host "Press Enter to try P0 Y90 R0..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot 0 90 0"
Start-Sleep -Seconds 10

Write-Host "Press Enter to try P0 Y-90 R0..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot 0 -90 0"
Start-Sleep -Seconds 2

Write-Host "Press Enter to try P0 Y0 R90..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot 0 0 90"
Start-Sleep -Seconds 2

Write-Host "Press Enter to try P0 Y0 R-90..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_rot 0 0 -90"
Start-Sleep -Seconds 2

# Step 6: Try scale/distance adjustments
Write-Host "`n--- Step 6: Scale + distance tweaks ---" -ForegroundColor Yellow
Write-Host "Press Enter to set scale=0.5, dist=30..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_scale 0.5"
Send-ModCommand "nvg_lens_dist 30"
Start-Sleep -Seconds 2

# Step 7: Try large scale to verify visibility
Write-Host "Press Enter to set scale=2.0 (should be very visible)..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg_lens_scale 2.0"
Start-Sleep -Seconds 2

# Step 8: Toggle off to clean up
Write-Host "`n--- Step 8: Disable NVG ---" -ForegroundColor Yellow
Write-Host "Press Enter to disable NVG..." -ForegroundColor Gray
Read-Host
Send-ModCommand "nvg"

Write-Host "`n=== Test complete. Check log for diagnostics. ===" -ForegroundColor Yellow
