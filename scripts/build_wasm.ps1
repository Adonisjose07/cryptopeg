# Script de construcción WebAssembly para PowerShell
$ErrorActionPreference = "Stop"

Write-Host "=== Compilando Módulo Criptográfico Wasm (CryptoPeg) ===" -ForegroundColor Cyan
if (-not (Test-Path "public\wasm")) {
    New-Item -ItemType Directory -Path "public\wasm" | Out-Null
}

docker build -f Dockerfile.wasm -t cryptopeg-wasm:latest .
$containerId = (docker create cryptopeg-wasm:latest).Trim()
docker cp "${containerId}:/dist/cryptopeg_crypto.js" "public\wasm\cryptopeg_crypto.js"
docker cp "${containerId}:/dist/cryptopeg_crypto.wasm" "public\wasm\cryptopeg_crypto.wasm"
docker rm -f $containerId | Out-Null

Write-Host "=== [OK] Artefactos Wasm generados en public\wasm\ ===" -ForegroundColor Green
Get-ChildItem -Path "public\wasm"
