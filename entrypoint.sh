#!/bin/bash
set -e

echo "================================================================="
echo "   INICIANDO NODO CRYPTOPEG USDT (1:1 BACKED RINGCT MLSAG)       "
echo "================================================================="

# Comprobar si el modo Oráculo de Arbitrum está activo
if [ "${ENABLE_ORACLE}" = "true" ]; then
    echo "[ENTRYPOINT] ENABLE_ORACLE=true detectado."
    echo "[ENTRYPOINT] Iniciando Oraculo de Enlace Arbitrum L2 en segundo plano..."
    (cd /app/contracts && node scripts/oracle_listener.js) &
else
    echo "[ENTRYPOINT] ENABLE_ORACLE=false (o no definido)."
    echo "[ENTRYPOINT] Modo activo: Nodo Validador Regular P2P (Privado y sin dependencias externas)."
fi

# Si no hay argumentos, iniciar el daemon del nodo por defecto
if [ $# -eq 0 ]; then
    exec /app/crypto_node
else
    exec "$@"
fi
