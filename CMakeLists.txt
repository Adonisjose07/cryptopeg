cmake_minimum_required(VERSION 3.20)
project(CryptoPegUSDT VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimization flags
add_compile_options(-Wall -Wextra -pedantic -O3)

find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
    pkg_check_modules(LIBSODIUM QUIET libsodium)
endif()

if (NOT LIBSODIUM_FOUND)
    find_library(LIBSODIUM_LIBRARIES NAMES sodium REQUIRED)
    find_path(LIBSODIUM_INCLUDE_DIRS NAMES sodium.h REQUIRED)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_library(LMDB_LIBRARIES NAMES lmdb REQUIRED)
find_path(LMDB_INCLUDE_DIRS NAMES lmdb.h REQUIRED)

include_directories(
    include
    ${LIBSODIUM_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${LMDB_INCLUDE_DIRS}
)

# Core Library
add_library(crypto_core STATIC
    src/vault.cpp
    src/stealth.cpp
    src/pedersen.cpp
    src/ring_signature.cpp
    src/tumbler.cpp
    src/block.cpp
    src/blockchain_db.cpp
    src/rpc_server.cpp
    src/node.cpp
    src/mnemonic.cpp
    src/wallet_manager.cpp
    src/p2p.cpp
)

add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)

target_link_libraries(crypto_core
    PUBLIC
    ${LIBSODIUM_LIBRARIES}
    ${LMDB_LIBRARIES}
    OpenSSL::Crypto
    OpenSSL::SSL
    Threads::Threads
)

# Main Executable / Node Daemon CLI
add_executable(crypto_node src/main.cpp)
target_link_libraries(crypto_node PRIVATE crypto_core)

# Independent Wallet Client CLI
add_executable(crypto_wallet_cli src/wallet_cli.cpp)
target_link_libraries(crypto_wallet_cli PRIVATE crypto_core)

# Tests
add_executable(test_vault_precision tests/test_vault_precision.cpp)
target_link_libraries(test_vault_precision PRIVATE crypto_core)

add_executable(test_stealth_privacy tests/test_stealth_privacy.cpp)
target_link_libraries(test_stealth_privacy PRIVATE crypto_core)

add_executable(test_tumbler_mixer tests/test_tumbler_mixer.cpp)
target_link_libraries(test_tumbler_mixer PRIVATE crypto_core)

add_executable(test_lmdb_persistence tests/test_lmdb_persistence.cpp)
target_link_libraries(test_lmdb_persistence PRIVATE crypto_core)

add_executable(test_rpc_api tests/test_rpc_api.cpp)
target_link_libraries(test_rpc_api PRIVATE crypto_core)

add_executable(test_mnemonic_wallet tests/test_mnemonic_wallet.cpp)
target_link_libraries(test_mnemonic_wallet PRIVATE crypto_core)

add_executable(test_p2p_network tests/test_p2p_network.cpp)
target_link_libraries(test_p2p_network PRIVATE crypto_core)

# Binary output destination
set_target_properties(
    crypto_node
    crypto_wallet_cli
    test_vault_precision
    test_stealth_privacy
    test_tumbler_mixer
    test_lmdb_persistence
    test_rpc_api
    test_mnemonic_wallet
    test_p2p_network
    PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
