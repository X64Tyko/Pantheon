$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# ── Build C++ ─────────────────────────────────────────────────────────────────
Write-Host '[dev] building all targets ...'
cmake --build "$root\cmake-build-debug"

# ── Kairos    :8080 ───────────────────────────────────────────────────────────
$kairos = Join-Path $root 'cmake-build-debug\kairos\kairos.exe'
if (-not (Test-Path $kairos)) {
    Write-Error "kairos.exe not found at $kairos - build the project first (cmake --build cmake-build-debug)"
}
Write-Host '[dev] starting Kairos on :8080 ...'
$kairosProc = Start-Process -FilePath $kairos -WorkingDirectory (Split-Path $kairos) -PassThru

# ── Tunarr server    :8000 ────────────────────────────────────────────────────
$tunarrDir = Join-Path $root 'tunarr'
if (-not (Test-Path (Join-Path $tunarrDir 'server\node_modules'))) {
    Write-Host '[dev] installing Tunarr dependencies ...'
    Push-Location $tunarrDir
    pnpm install
    Pop-Location
}
Write-Host '[dev] starting Tunarr server on :8000 ...'
$env:KAIROS_URL = 'http://localhost:8080'
$tunarrServerProc = Start-Process -FilePath 'pwsh' -ArgumentList '-NoExit', '-Command', 'pnpm dev' `
    -WorkingDirectory (Join-Path $tunarrDir 'server') -PassThru

# ── Tunarr web    :5180 ───────────────────────────────────────────────────────
Write-Host '[dev] starting Tunarr web on :5180 ...'
$tunarrWebProc = Start-Process -FilePath 'pwsh' -ArgumentList '-NoExit', '-Command', 'pnpm vite --port 5180' `
    -WorkingDirectory (Join-Path $tunarrDir 'web') -PassThru

# ── Hades    :5173 ────────────────────────────────────────────────────────────
Write-Host '[dev] starting Hades on :5173 ...'
try {
    Set-Location (Join-Path $root 'hades')
    pnpm dev
} finally {
    Write-Host '[dev] shutting down ...'
    $tunarrWebProc, $tunarrServerProc, $kairosProc | ForEach-Object {
        if ($_ -and -not $_.HasExited) {
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
