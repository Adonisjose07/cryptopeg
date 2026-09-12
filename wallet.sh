#!/usr/bin/env bash
# =========================================================================
#  CryptoPeg USDT - Lanzador de Billetera CLI para Linux / macOS / WSL
#  Ejecuta el cliente interactivo aislado montando la carpeta actual
# =========================================================================

if [ $# -eq 0 ]; then
    docker run -it --rm --network host -v "$(pwd)":/data -w /data --entrypoint /app/crypto_wallet_cli crypto-core-node:latest --daemon 127.0.0.1:8080
else
    docker run -it --rm --network host -v "$(pwd)":/data -w /data --entrypoint /app/crypto_wallet_cli crypto-core-node:latest "$@"
fi
