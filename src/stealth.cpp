#include "stealth.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace crypto {

std::string StealthAddress::encode() const {
    return "STX" + to_hex(spend_public_key) + to_hex(view_public_key);
}

StealthAddress StealthAddress::decode(const std::string& encoded) {
    if (encoded.length() != 3 + 64 + 64 || encoded.substr(0, 3) != "STX") {
        throw std::invalid_argument("Formato inválido de dirección stealth.");
    }
    StealthAddress addr;
    auto spend_bytes = from_hex(encoded.substr(3, 64));
    auto view_bytes = from_hex(encoded.substr(67, 64));
    std::memcpy(addr.spend_public_key.data(), spend_bytes.data(), 32);
    std::memcpy(addr.view_public_key.data(), view_bytes.data(), 32);
    return addr;
}

StealthWallet StealthWallet::generate_random() {
    StealthWallet wallet;
    // Generar escalar aleatorio a (spend)
    crypto_core_ed25519_scalar_random(wallet.spend_private_key.data());
    // Derivar A = aG
    crypto_scalarmult_ed25519_base_noclamp(wallet.spend_public_key.data(), wallet.spend_private_key.data());

    // Generar escalar aleatorio b (view)
    crypto_core_ed25519_scalar_random(wallet.view_private_key.data());
    // Derivar B = bG
    crypto_scalarmult_ed25519_base_noclamp(wallet.view_public_key.data(), wallet.view_private_key.data());

    return wallet;
}

OneTimeOutput StealthProtocol::create_one_time_output(
    const StealthAddress& recipient_address,
    Amount amount,
    const Hash256* deterministic_seed
) {
    OneTimeOutput output;
    output.amount = amount;

    // 1. Emisor genera secreto efímero r (determinista si viene semilla, aleatorio si es interno)
    Key256 r;
    if (deterministic_seed != nullptr) {
        uint8_t hash_r[64];
        crypto_generichash(hash_r, 64, deterministic_seed->data(), 32, nullptr, 0);
        crypto_core_ed25519_scalar_reduce(r.data(), hash_r);
    } else {
        crypto_core_ed25519_scalar_random(r.data());
    }

    // 2. Emisor calcula R = rG
    crypto_scalarmult_ed25519_base_noclamp(output.ephemeral_public_key.data(), r.data());

    // 3. Emisor calcula punto compartido rB = r * (bG)
    Key256 rB;
    if (crypto_scalarmult_ed25519_noclamp(rB.data(), r.data(), recipient_address.view_public_key.data()) != 0) {
        secure_wipe(r);
        throw std::runtime_error("Fallo de multiplicación escalar rB en stealth output.");
    }

    // 4. Hash del secreto compartido S = H(rB) (64 bytes para reducción exacta de libsodium)
    uint8_t hash_s[64];
    crypto_generichash(hash_s, 64, rB.data(), 32, nullptr, 0);

    // Reducir hash a escalar válido
    Key256 s_scalar;
    crypto_core_ed25519_scalar_reduce(s_scalar.data(), hash_s);

    // 5. Calcular hG = H(S)*G
    Key256 hG;
    crypto_scalarmult_ed25519_base_noclamp(hG.data(), s_scalar.data());

    // 6. Destino P = hG + A
    if (crypto_core_ed25519_add(output.destination_one_time.data(), hG.data(), recipient_address.spend_public_key.data()) != 0) {
        secure_wipe(r);
        secure_wipe(s_scalar);
        throw std::runtime_error("Fallo al sumar puntos hG + A para dirección de destino.");
    }

    // Limpieza de memoria
    secure_wipe(r);
    secure_wipe(rB);
    secure_wipe(s_scalar);

    return output;
}

bool StealthProtocol::scan_output(
    const StealthWallet& recipient_wallet,
    const OneTimeOutput& output
) {
    // Receptor ve (R, P). Calcula bR = b * (rG) = r * (bG) = rB
    Key256 bR;
    if (crypto_scalarmult_ed25519_noclamp(bR.data(), recipient_wallet.view_private_key.data(), output.ephemeral_public_key.data()) != 0) {
        return false;
    }

    // Hash S = H(bR) (64 bytes para reducción exacta)
    uint8_t hash_s[64];
    crypto_generichash(hash_s, 64, bR.data(), 32, nullptr, 0);

    Key256 s_scalar;
    crypto_core_ed25519_scalar_reduce(s_scalar.data(), hash_s);

    // Calcular hG = H(S)*G
    Key256 hG;
    crypto_scalarmult_ed25519_base_noclamp(hG.data(), s_scalar.data());

    // P' = hG + A
    Key256 expected_P;
    if (crypto_core_ed25519_add(expected_P.data(), hG.data(), recipient_wallet.spend_public_key.data()) != 0) {
        return false;
    }

    bool match = (std::memcmp(expected_P.data(), output.destination_one_time.data(), 32) == 0);

    secure_wipe(bR);
    sodium_memzero(hash_s, sizeof(hash_s));
    secure_wipe(s_scalar);

    return match;
}

Key256 StealthProtocol::derive_one_time_private_key(
    const StealthWallet& recipient_wallet,
    const OneTimeOutput& output
) {
    // x = H(bR) + a (mod l)
    Key256 bR;
    if (crypto_scalarmult_ed25519_noclamp(bR.data(), recipient_wallet.view_private_key.data(), output.ephemeral_public_key.data()) != 0) {
        throw std::runtime_error("Punto bR inválido en derivación de clave privada.");
    }

    uint8_t hash_s[64];
    crypto_generichash(hash_s, 64, bR.data(), 32, nullptr, 0);

    Key256 s_scalar;
    crypto_core_ed25519_scalar_reduce(s_scalar.data(), hash_s);

    Key256 one_time_private;
    crypto_core_ed25519_scalar_add(one_time_private.data(), s_scalar.data(), recipient_wallet.spend_private_key.data());

    secure_wipe(bR);
    sodium_memzero(hash_s, sizeof(hash_s));
    secure_wipe(s_scalar);

    return one_time_private;
}

} // namespace crypto
