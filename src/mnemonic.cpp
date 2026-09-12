#include "mnemonic.hpp"
#include "bip39_words.hpp"
#include <openssl/sha.h>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace crypto {

// Mapa para búsqueda rápida de palabra -> índice 0..2047
static const std::unordered_map<std::string_view, size_t>& get_word_index_map() {
    static const auto map = []() {
        std::unordered_map<std::string_view, size_t> m;
        m.reserve(BIP39_WORDS.size());
        for (size_t i = 0; i < BIP39_WORDS.size(); ++i) {
            m[BIP39_WORDS[i]] = i;
        }
        return m;
    }();
    return map;
}

std::vector<std::string> MnemonicEngine::split_words(const std::string& phrase) {
    std::vector<std::string> words;
    std::istringstream iss(phrase);
    std::string w;
    while (iss >> w) {
        words.push_back(w);
    }
    return words;
}

std::string MnemonicEngine::generate_24_words() {
    // 1. 256 bits de entropía criptográfica (32 bytes)
    std::array<uint8_t, 32> entropy;
    randombytes_buf(entropy.data(), 32);

    // 2. Checksum SHA-256 (primeros 8 bits = 1 byte)
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(entropy.data(), entropy.size(), hash);
    uint8_t checksum = hash[0];

    // 3. Empaquetar 256 bits + 8 bits = 264 bits (33 bytes)
    std::array<uint8_t, 33> bits_stream;
    std::memcpy(bits_stream.data(), entropy.data(), 32);
    bits_stream[32] = checksum;

    // 4. Dividir en 24 grupos de 11 bits
    std::ostringstream oss;
    for (size_t i = 0; i < 24; ++i) {
        size_t bit_offset = i * 11;
        size_t byte_idx = bit_offset / 8;
        size_t bit_rem = bit_offset % 8;

        // Extraer 11 bits desde bit_offset
        uint32_t val = (static_cast<uint32_t>(bits_stream[byte_idx]) << 16) |
                       (static_cast<uint32_t>(bits_stream[byte_idx + 1]) << 8);
        if (byte_idx + 2 < 33) {
            val |= static_cast<uint32_t>(bits_stream[byte_idx + 2]);
        }

        uint16_t word_idx = (val >> (24 - 11 - bit_rem)) & 0x07FF; // 11 bits (0..2047)

        if (i > 0) oss << " ";
        oss << BIP39_WORDS[word_idx];
    }

    sodium_memzero(bits_stream.data(), bits_stream.size());
    secure_wipe(entropy);
    return oss.str();
}

bool MnemonicEngine::validate_mnemonic(const std::string& phrase) {
    auto words = split_words(phrase);
    if (words.size() != 24) return false;

    const auto& map = get_word_index_map();
    std::vector<uint16_t> indices;
    indices.reserve(24);

    for (const auto& w : words) {
        auto it = map.find(w);
        if (it == map.end()) return false;
        indices.push_back(static_cast<uint16_t>(it->second));
    }

    // Reconstruir los 264 bits (33 bytes)
    std::array<uint8_t, 33> bits_stream{};
    for (size_t i = 0; i < 24; ++i) {
        uint16_t idx = indices[i];
        size_t bit_offset = i * 11;
        for (int b = 0; b < 11; ++b) {
            if ((idx >> (10 - b)) & 1) {
                size_t target_bit = bit_offset + b;
                bits_stream[target_bit / 8] |= (1 << (7 - (target_bit % 8)));
            }
        }
    }

    // Verificar checksum
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(bits_stream.data(), 32, hash);

    return (hash[0] == bits_stream[32]);
}

Key256 MnemonicEngine::mnemonic_to_entropy(const std::string& phrase) {
    if (!validate_mnemonic(phrase)) {
        throw std::invalid_argument("Frase mnemónica inválida o checksum incorrecto.");
    }

    auto words = split_words(phrase);
    const auto& map = get_word_index_map();

    std::array<uint8_t, 33> bits_stream{};
    for (size_t i = 0; i < 24; ++i) {
        uint16_t idx = static_cast<uint16_t>(map.at(words[i]));
        size_t bit_offset = i * 11;
        for (int b = 0; b < 11; ++b) {
            if ((idx >> (10 - b)) & 1) {
                size_t target_bit = bit_offset + b;
                bits_stream[target_bit / 8] |= (1 << (7 - (target_bit % 8)));
            }
        }
    }

    Key256 entropy;
    std::memcpy(entropy.data(), bits_stream.data(), 32);
    return entropy;
}

StealthWallet MnemonicEngine::mnemonic_to_wallet(const std::string& phrase) {
    Key256 entropy = mnemonic_to_entropy(phrase);

    StealthWallet wallet;

    // Derivar Spend Key (a, A)
    uint8_t spend_seed_64[64];
    crypto_generichash(
        spend_seed_64, 64,
        entropy.data(), 32,
        reinterpret_cast<const uint8_t*>("CryptoPeg_SpendSeed"), 19
    );
    crypto_core_ed25519_scalar_reduce(wallet.spend_private_key.data(), spend_seed_64);
    crypto_scalarmult_ed25519_base_noclamp(wallet.spend_public_key.data(), wallet.spend_private_key.data());

    // Derivar View Key (b, B)
    uint8_t view_seed_64[64];
    crypto_generichash(
        view_seed_64, 64,
        entropy.data(), 32,
        reinterpret_cast<const uint8_t*>("CryptoPeg_ViewSeed"), 18
    );
    crypto_core_ed25519_scalar_reduce(wallet.view_private_key.data(), view_seed_64);
    crypto_scalarmult_ed25519_base_noclamp(wallet.view_public_key.data(), wallet.view_private_key.data());

    // Limpieza de memoria
    secure_wipe(entropy);
    sodium_memzero(spend_seed_64, sizeof(spend_seed_64));
    sodium_memzero(view_seed_64, sizeof(view_seed_64));

    return wallet;
}

} // namespace crypto
