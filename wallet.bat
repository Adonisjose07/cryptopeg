@echo off
REM =========================================================================
REM  CryptoPeg USDT - Lanzador de Billetera CLI para Windows
REM  Ejecuta el cliente interactivo aislado montando la carpeta actual
REM =========================================================================

REM Si no se pasan argumentos, conecta por defecto a 127.0.0.1:8080
if "%~1"=="" (
    docker run -it --rm --network host -v "%cd%":/data -w /data --entrypoint /app/crypto_wallet_cli crypto-core-node:latest --daemon 127.0.0.1:8080
) else (
    docker run -it --rm --network host -v "%cd%":/data -w /data --entrypoint /app/crypto_wallet_cli crypto-core-node:latest %*
)
