$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'cmake-build-debug'

# ── Build C++ ─────────────────────────────────────────────────────────────────
Write-Host '[dev] building all targets ...'
cmake --build $build

foreach ($svc in @('kairos', 'hephaestus', 'hermes')) {
    $bin = Join-Path $build "$svc\$svc.exe"
    if (-not (Test-Path $bin)) {
        Write-Error "$svc.exe not found at $bin - build the project first (cmake --build cmake-build-debug)"
    }
}

# ── Kairos    :8080 ───────────────────────────────────────────────────────────
Write-Host '[dev] starting Kairos on :8080 ...'
$kairosProc = Start-Process -FilePath (Join-Path $build 'kairos\kairos.exe') `
    -WorkingDirectory (Join-Path $build 'kairos') -PassThru

# ── Hephaestus :8082 ─────────────────────────────────────────────────────────
Write-Host '[dev] starting Hephaestus on :8082 ...'
$hephProc = Start-Process -FilePath (Join-Path $build 'hephaestus\hephaestus.exe') `
    -ArgumentList '--kairos-url', 'http://localhost:8080', '--port', '8082' `
    -WorkingDirectory (Join-Path $build 'hephaestus') -PassThru

# ── Hermes   :8000 ────────────────────────────────────────────────────────────
Write-Host '[dev] starting Hermes on :8000 ...'
$hermesProc = Start-Process -FilePath (Join-Path $build 'hermes\hermes.exe') `
    -ArgumentList '--kairos-url', 'http://localhost:8080',
                  '--hephaestus-url', 'http://localhost:8082',
                  '--hades-url', 'http://localhost:5173',
                  '--port', '8000' `
    -WorkingDirectory (Join-Path $build 'hermes') -PassThru

# ── Hades    :5173 ────────────────────────────────────────────────────────────
Write-Host '[dev] starting Hades on :5173 ...'
Write-Host ''
Write-Host '  Hades UI (hot reload):    http://localhost:5173'
Write-Host '  Full pipeline via Hermes: http://localhost:8000'
Write-Host '  Kairos direct:            http://localhost:8080'
Write-Host ''
try {
    Set-Location (Join-Path $root 'hades')
    pnpm dev
} finally {
    Write-Host '[dev] shutting down ...'
    $hermesProc, $hephProc, $kairosProc | ForEach-Object {
        if ($_ -and -not $_.HasExited) {
            Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
