#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cmath>
#include <sodium.h>

namespace crypto {

// Unidad atómica: micro-USDT (1 USDT = 1,000,000 unidades)
// Exactamente como el estándar TRC-20 y ERC-20 de USDT (6 decimales).
using Amount = uint64_t;
constexpr Amount USDT_UNIT = 1'000'000ULL;

using Key256 = std::array<uint8_t, 32>;
using Hash256 = std::array<uint8_t, 32>;
using KeyImage = std::array<uint8_t, 32>;
using Signature64 = std::array<uint8_t, 64>;

constexpr uint32_t PROTOCOL_VERSION = 3;
constexpr Amount MAX_TRANSACTION_AMOUNT = 100'000'000'000'000ULL; // 100,000,000 USDT límite protocolario

// Adición aritmética segura contra desbordamiento de enteros (AUD-H0-P0-02)
inline bool safe_add_amount(Amount a, Amount b, Amount& result) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, &result);
#else
    if (UINT64_MAX - a < b) return false;
    result = a + b;
    return true;
#endif
}

// Sustracción aritmética segura contra desbordamiento de enteros (underflow)
inline bool safe_sub_amount(Amount a, Amount b, Amount& result) {
    if (a < b) return false;
    result = a - b;
    return true;
}

// Cálculo exacto y seguro de comisiones en punto fijo con enteros de 128 bits (Auditoría v3 - P1-04)
inline Amount safe_fee_calc(Amount amount, uint32_t bps) {
    if (bps > 10000) {
        throw std::invalid_argument("Tasa de comisión excede el límite máximo de 10000 bps (100%).");
    }
#if defined(__SIZEOF_INT128__)
    unsigned __int128 product = static_cast<unsigned __int128>(amount) * bps;
    return static_cast<Amount>(product / 10000ULL);
#else
    if (bps == 0 || amount == 0) return 0;
    if (UINT64_MAX / bps < amount) {
        throw std::overflow_error("Desbordamiento aritmético en cálculo de comisión.");
    }
    return (amount * bps) / 10000ULL;
#endif
}

// Borrado seguro de memoria RAM para claves privadas
inline void secure_wipe(void* ptr, size_t len) {
    if (ptr && len > 0) {
        sodium_memzero(ptr, len);
    }
}

template <size_t N>
inline void secure_wipe(std::array<uint8_t, N>& arr) {
    sodium_memzero(arr.data(), N);
}

// Conversión a cadena Hexadecimal
inline std::string to_hex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    }
    return oss.str();
}

template <size_t N>
inline std::string to_hex(const std::array<uint8_t, N>& arr) {
    return to_hex(arr.data(), N);
}

inline std::vector<uint8_t> from_hex(const std::string& hex) {
    if (hex.length() % 2 != 0) {
        throw std::invalid_argument("Cadena hexadecimal con longitud inválida.");
    }
    for (char c : hex) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument("Carácter hexadecimal inválido.");
        }
    }
    std::vector<uint8_t> bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        size_t idx = 0;
        uint8_t byte = static_cast<uint8_t>(std::stoul(byteString, &idx, 16));
        if (idx != 2) {
            throw std::invalid_argument("Error parseando byte hexadecimal.");
        }
        bytes.push_back(byte);
    }
    return bytes;
}

// Formateo financiero exacto (sin punto flotante)
inline std::string format_usdt(Amount micro_usdt) {
    uint64_t whole = micro_usdt / USDT_UNIT;
    uint64_t frac = micro_usdt % USDT_UNIT;
    std::ostringstream oss;
    oss << whole << "." << std::setw(6) << std::setfill('0') << frac << " USDT";
    return oss.str();
}

inline Amount parse_usdt(double usdt) {
    if (std::isnan(usdt) || std::isinf(usdt) || usdt < 0.0 || usdt > 1e12) {
        throw std::invalid_argument("Monto en USDT inválido o fuera de rango (AUD-MED-01).");
    }
    return static_cast<Amount>(usdt * 1'000'000.0 + 0.5);
}

// Representante canónico en el subgrupo primo multiplicando por el cofactor 8 (AUD-CRIT-01)
// Se calcula mediante 3 duplicaciones sucesivas de punto en curva (2P, 4P, 8P) con adición completa de Edwards.
// Garantiza que cualquier punto I y todos sus alias de torsión (I + Tk) colapsen al mismo punto canónico 8*I.
inline KeyImage canonical_key_image(const KeyImage& image) {
    KeyImage p2, p4, p8;
    if (crypto_core_ed25519_add(p2.data(), image.data(), image.data()) != 0) return image;
    if (crypto_core_ed25519_add(p4.data(), p2.data(), p2.data()) != 0) return image;
    if (crypto_core_ed25519_add(p8.data(), p4.data(), p4.data()) != 0) return image;
    return p8;
}

inline Hash256 compute_deposit_attestation_hash(
    uint64_t chain_id,
    const std::string& contract_address,
    const std::string& tx_hash,
    uint32_t log_index,
    Amount gross_amount,
    const Key256& view_pub,
    const Key256& spend_pub,
    uint64_t l2_block_number
) {
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>("DEPOSIT_ATTESTATION_V1"), 22);
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(&chain_id), sizeof(chain_id));
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(contract_address.data()), contract_address.size());
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(tx_hash.data()), tx_hash.size());
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(&log_index), sizeof(log_index));
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(&gross_amount), sizeof(gross_amount));
    crypto_hash_sha256_update(&state, view_pub.data(), 32);
    crypto_hash_sha256_update(&state, spend_pub.data(), 32);
    crypto_hash_sha256_update(&state, reinterpret_cast<const uint8_t*>(&l2_block_number), sizeof(l2_block_number));
    Hash256 h;
    crypto_hash_sha256_final(&state, h.data());
    return h;
}

// Validación canónica de dirección pública EVM (0x + 40 caracteres hexadecimales, no-cero) (AUD-CP-03)
inline bool is_valid_evm_address(const std::string& addr) {
    if (addr.size() != 42) return false;
    if (addr[0] != '0' || (addr[1] != 'x' && addr[1] != 'X')) return false;
    bool all_zero = true;
    for (size_t i = 2; i < 42; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(addr[i]))) return false;
        if (addr[i] != '0') all_zero = false;
    }
    if (all_zero) return false; // Rechazar address(0)
    return true;
}

} // namespace crypto
