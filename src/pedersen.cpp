#include "pedersen.hpp"
#include <cstring>
#include <stdexcept>

namespace crypto {

static Key256 init_generator_H() {
    Key256 H_raw;
    const char* seed = "CryptoPegUSDT_RingCT_Pedersen_Generator_H_Point";
    uint8_t hash[crypto_core_ed25519_UNIFORMBYTES];
    crypto_generichash(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(seed), std::strlen(seed), nullptr, 0);

    if (crypto_core_ed25519_from_uniform(H_raw.data(), hash) != 0) {
        throw std::runtime_error("Fallo al derivar punto generador H de Pedersen.");
    }

    // Limpiar cofactor 8 multiplicando por 8 para asegurar que H resida estrictamente en el subgrupo de orden primo L (CP-SEC-03)
    static const unsigned char eight_scalar[32] = {8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Key256 H;
    if (crypto_scalarmult_ed25519_noclamp(H.data(), eight_scalar, H_raw.data()) != 0) {
        throw std::runtime_error("Fallo al multiplicar por cofactor 8 en generador H de Pedersen.");
    }
    return H;
}

const Key256& Pedersen::get_H() {
    static const Key256 H = init_generator_H();
    return H;
}

static Key256 amount_to_scalar(Amount amount) {
    uint8_t scalar_64[64] = {0};
    // Serializar uint64_t en little-endian
    for (size_t i = 0; i < sizeof(Amount); ++i) {
        scalar_64[i] = static_cast<uint8_t>((amount >> (i * 8)) & 0xFF);
    }
    Key256 reduced;
    crypto_core_ed25519_scalar_reduce(reduced.data(), scalar_64);
    return reduced;
}

PedersenCommitment Pedersen::commit(Amount amount) {
    PedersenCommitment p;
    p.value = amount;

    // 1. Generar factor de cegado aleatorio r
    crypto_core_ed25519_scalar_random(p.blinding_factor.data());

    // 2. C = rG + vH
    p.commitment_point = commit_with_blinding(amount, p.blinding_factor);
    return p;
}

Key256 Pedersen::commit_with_blinding(Amount amount, const Key256& blinding_factor) {
    // rG
    Key256 rG;
    crypto_scalarmult_ed25519_base_noclamp(rG.data(), blinding_factor.data());

    // vH
    Key256 v_scalar = amount_to_scalar(amount);
    Key256 vH;
    if (crypto_scalarmult_ed25519_noclamp(vH.data(), v_scalar.data(), get_H().data()) != 0) {
        throw std::runtime_error("Fallo en multiplicación escalar vH.");
    }

    // C = rG + vH
    Key256 C;
    if (crypto_core_ed25519_add(C.data(), rG.data(), vH.data()) != 0) {
        throw std::runtime_error("Fallo al sumar rG + vH.");
    }

    return C;
}

Key256 Pedersen::add_commitments(const Key256& c1, const Key256& c2) {
    Key256 res;
    if (crypto_core_ed25519_add(res.data(), c1.data(), c2.data()) != 0) {
        throw std::runtime_error("Fallo al sumar compromisos.");
    }
    return res;
}

Key256 Pedersen::sub_commitments(const Key256& c1, const Key256& c2) {
    Key256 res;
    if (crypto_core_ed25519_sub(res.data(), c1.data(), c2.data()) != 0) {
        throw std::runtime_error("Fallo al restar compromisos.");
    }
    return res;
}

bool Pedersen::verify_balance_conservation(
    const std::vector<Key256>& in_commitments,
    const std::vector<Key256>& out_commitments,
    Amount public_fee,
    const Key256& excess_blinding_pubkey
) {
    if (in_commitments.empty() || out_commitments.empty()) {
        return false;
    }

    // Validación de puntos en la curva Ed25519 (AUD-INFO-01)
    for (const auto& c : in_commitments) {
        if (crypto_core_ed25519_is_valid_point(c.data()) == 0) return false;
    }
    for (const auto& c : out_commitments) {
        if (crypto_core_ed25519_is_valid_point(c.data()) == 0) return false;
    }
    if (crypto_core_ed25519_is_valid_point(excess_blinding_pubkey.data()) == 0) {
        return false;
    }

    // Suma de entradas: Sum(C_in)
    Key256 sum_in = in_commitments[0];
    for (size_t i = 1; i < in_commitments.size(); ++i) {
        sum_in = add_commitments(sum_in, in_commitments[i]);
    }

    // Suma de salidas: Sum(C_out)
    Key256 sum_out = out_commitments[0];
    for (size_t i = 1; i < out_commitments.size(); ++i) {
        sum_out = add_commitments(sum_out, out_commitments[i]);
    }

    // Total salidas esperadas con comisión
    Key256 total_out_with_fee = sum_out;
    if (public_fee > 0) {
        Key256 fee_scalar = amount_to_scalar(public_fee);
        Key256 fee_H;
        if (crypto_scalarmult_ed25519_noclamp(fee_H.data(), fee_scalar.data(), get_H().data()) != 0) {
            return false;
        }
        total_out_with_fee = add_commitments(sum_out, fee_H);
    }

    // Diferencia: Sum(C_in) - (Sum(C_out) + Fee*H)
    Key256 diff = sub_commitments(sum_in, total_out_with_fee);

    // Debe ser exactamente igual a (Sum(r_in) - Sum(r_out))*G (tiempo constante AUD-MED-01)
    return sodium_memcmp(diff.data(), excess_blinding_pubkey.data(), 32) == 0;
}

} // namespace crypto
