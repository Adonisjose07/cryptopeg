# ==========================================
# Stage 1: Build & Compile C++20 Project
# ==========================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    make \
    g++-13 \
    pkg-config \
    libsodium-dev \
    libssl-dev \
    liblmdb-dev \
    nlohmann-json3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/
COPY public/ ./public/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13
RUN cmake --build build -j$(nproc)

# ==========================================
# Stage 2: Minimal Production Runtime
# ==========================================
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsodium23 \
    libssl3 \
    liblmdb0 \
    ca-certificates \
    curl \
    nodejs \
    npm \
    dos2unix \
    gosu \
    && rm -rf /var/lib/apt/lists/*

# Crear usuario y grupo de sistema sin privilegios (AUD-LOW-01)
RUN (userdel -r ubuntu 2>/dev/null || true) && \
    (groupdel ubuntu 2>/dev/null || true) && \
    groupadd -g 1000 cryptogroup && \
    useradd -m -u 1000 -g cryptogroup -s /bin/bash cryptouser

WORKDIR /app
RUN mkdir -p /app/data/lmdb /app/contracts

# Copy compiled binaries from builder
COPY --from=builder /build/build/bin/crypto_node /app/crypto_node
COPY --from=builder /build/build/bin/crypto_wallet_cli /app/crypto_wallet_cli
COPY --from=builder /build/build/bin/test_vault_precision /app/test_vault_precision
COPY --from=builder /build/build/bin/test_stealth_privacy /app/test_stealth_privacy
COPY --from=builder /build/build/bin/test_tumbler_mixer /app/test_tumbler_mixer
COPY --from=builder /build/build/bin/test_lmdb_persistence /app/test_lmdb_persistence
COPY --from=builder /build/build/bin/test_rpc_api /app/test_rpc_api
COPY --from=builder /build/build/bin/test_mnemonic_wallet /app/test_mnemonic_wallet
COPY --from=builder /build/build/bin/test_p2p_network /app/test_p2p_network
COPY --from=builder /build/public /app/public

# Copy contracts & oracle service
COPY contracts/ /app/contracts/
RUN cd /app/contracts && npm install --omit=dev --no-audit --no-fund

COPY entrypoint.sh /app/entrypoint.sh
RUN dos2unix /app/entrypoint.sh && chmod +x /app/entrypoint.sh

# Asignar permisos de directorio a cryptouser
RUN chown -R cryptouser:cryptogroup /app

EXPOSE 8080

ENTRYPOINT ["/app/entrypoint.sh"]
