#!/usr/bin/env bash
set -e

echo "=== Compilando Módulo Criptográfico Wasm (CryptoPeg) ==="
mkdir -p public/wasm

docker build -f Dockerfile.wasm -t cryptopeg-wasm:latest .
CONTAINER_ID=$(docker create cryptopeg-wasm:latest)
docker cp "${CONTAINER_ID}:/dist/cryptopeg_crypto.js" public/wasm/cryptopeg_crypto.js
docker cp "${CONTAINER_ID}:/dist/cryptopeg_crypto.wasm" public/wasm/cryptopeg_crypto.wasm
docker rm -f "${CONTAINER_ID}"

echo "=== [OK] Artefactos Wasm generados en public/wasm/ ==="
ls -lh public/wasm/
