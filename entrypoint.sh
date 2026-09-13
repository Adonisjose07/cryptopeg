#!/bin/bash
set -e

echo "================================================================="
echo "   INICIANDO NODO CRYPTOPEG USDT (1:1 BACKED RINGCT MLSAG)       "
echo "================================================================="

# Asegurar propiedad de datos montados para el usuario no privilegiado cryptouser (AUD-LOW-01)
if [ -d "/app/data" ]; then
    chown -R cryptouser:cryptogroup /app/data 2>/dev/null || true
fi

# Comprobar si el modo Oráculo de Arbitrum está activo
if [ "${ENABLE_ORACLE}" = "true" ]; then
    echo "[ENTRYPOINT] ENABLE_ORACLE=true detectado."
    echo "[ENTRYPOINT] Iniciando Oraculo de Enlace Arbitrum L2 bajo usuario sin privilegios..."
    (cd /app/contracts && exec gosu cryptouser node scripts/oracle_listener.js) &
else
    echo "[ENTRYPOINT] ENABLE_ORACLE=false (o no definido)."
    echo "[ENTRYPOINT] Modo activo: Nodo Validador Regular P2P (Privado y sin dependencias externas)."
fi

# Si no hay argumentos, iniciar el daemon del nodo como cryptouser
if [ $# -eq 0 ]; then
    exec gosu cryptouser /app/crypto_node
else
    exec gosu cryptouser "$@"
fi
