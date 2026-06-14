$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$kairos = Join-Path $root 'cmake-build-debug\kairos\kairos.exe'
if (-not (Test-Path $kairos)) {
    Write-Error "kairos.exe not found at $kairos - build the project first (cmake --build cmake-build-debug)"
}

Write-Host '[dev] starting Kairos on :8080 ...'
Start-Process -FilePath $kairos -WorkingDirectory (Split-Path $kairos)

Write-Host '[dev] starting Tunarr server on :8000 ...'
$env:KAIROS_URL = 'http://localhost:8080'
Start-Process -FilePath 'pnpm' -ArgumentList 'dev' -WorkingDirectory (Join-Path $root 'tunarr\server')

Write-Host '[dev] starting Hades dev server on :5173 ...'
Set-Location (Join-Path $root 'hades')
pnpm dev
