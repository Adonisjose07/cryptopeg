#include "ring_signature.hpp"
#include <cstring>
#include <stdexcept>

namespace crypto {

Key256 RingSignatureEngine::hash_to_point(const Key256& pubkey) {
    Key256 uniform;
    crypto_generichash(uniform.data(), 32, pubkey.data(), 32, nullptr, 0);

    Key256 p_raw;
    if (crypto_core_ed25519_from_uniform(p_raw.data(), uniform.data()) != 0) {
        throw std::runtime_error("Fallo al mapear clave pública a punto en la curva.");
    }

    // Limpiar cofactor 8 multiplicando por 8 para garantizar que Hp resida estrictamente en el subgrupo de orden primo L (AUD-CRIT-01)
    static const unsigned char eight_scalar[32] = {8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Key256 Hp;
    if (crypto_scalarmult_ed25519_noclamp(Hp.data(), eight_scalar, p_raw.data()) != 0) {
        throw std::runtime_error("Fallo al multiplicar por cofactor 8 en hash_to_point.");
    }
    return Hp;
}

KeyImage RingSignatureEngine::compute_key_image(const Key256& privkey, const Key256& pubkey) {
    Key256 Hp = hash_to_point(pubkey);
    KeyImage image;
    if (crypto_scalarmult_ed25519_noclamp(image.data(), privkey.data(), Hp.data()) != 0) {
        throw std::runtime_error("Fallo al calcular imagen de clave.");
    }
    return image;
}

// Función auxiliar de hash para generar los desafíos del anillo c_{i+1} = H(m, L_i, R_i)
static Key256 ring_hash_challenge(
    const Hash256& message_hash,
    const Key256& L,
    const Key256& R
) {
    uint8_t buffer[32 + 32 + 32];
    std::memcpy(buffer, message_hash.data(), 32);
    std::memcpy(buffer + 32, L.data(), 32);
    std::memcpy(buffer + 64, R.data(), 32);

    uint8_t hash_out[64];
    crypto_generichash(hash_out, 64, buffer, sizeof(buffer), nullptr, 0);

    Key256 scalar_out;
    crypto_core_ed25519_scalar_reduce(scalar_out.data(), hash_out);

    sodium_memzero(buffer, sizeof(buffer));
    sodium_memzero(hash_out, sizeof(hash_out));
    return scalar_out;
}

Hash256 RingSignatureEngine::compute_canonical_tx_hash(
    const std::vector<OneTimeOutput>& outputs,
    Amount public_fee,
    const std::vector<Key256>& ring_pubkeys,
    const KeyImage& key_image
) {
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, 32);

    // 1. Salidas (destinos, claves efímeras y montos)
    uint32_t out_count = static_cast<uint32_t>(outputs.size());
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(&out_count), sizeof(out_count));
    for (const auto& out : outputs) {
        crypto_generichash_update(&state, out.destination_one_time.data(), 32);
        crypto_generichash_update(&state, out.ephemeral_public_key.data(), 32);
        crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(&out.amount), sizeof(out.amount));
    }

    // 2. Comisión pública
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(&public_fee), sizeof(public_fee));

    // 3. Claves públicas del anillo
    uint32_t ring_count = static_cast<uint32_t>(ring_pubkeys.size());
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(&ring_count), sizeof(ring_count));
    for (const auto& pk : ring_pubkeys) {
        crypto_generichash_update(&state, pk.data(), 32);
    }

    // 4. Imagen de clave
    crypto_generichash_update(&state, key_image.data(), 32);

    Hash256 canonical_hash;
    crypto_generichash_final(&state, canonical_hash.data(), 32);
    return canonical_hash;
}

RingSignature RingSignatureEngine::sign(
    const Hash256& message_hash,
    const std::vector<Key256>& ring,
    size_t real_index,
    const Key256& real_privkey
) {
    size_t n = ring.size();
    if (n < 1) {
        throw std::invalid_argument("El anillo de señuelos debe tener al menos 1 participante.");
    }
    if (real_index >= n) {
        throw std::out_of_range("El índice real está fuera de los límites del anillo.");
    }

    RingSignature sig;
    sig.ring_pubkeys = ring;
    sig.responses.resize(n);

    // 1. Calcular la imagen de clave I = x * H_p(P_pi)
    sig.key_image = compute_key_image(real_privkey, ring[real_index]);

    // 2. Generar secreto efímero alfa para el índice real
    Key256 alpha;
    crypto_core_ed25519_scalar_random(alpha.data());

    // L_pi = alpha * G
    Key256 L_pi;
    crypto_scalarmult_ed25519_base_noclamp(L_pi.data(), alpha.data());

    // R_pi = alpha * H_p(P_pi)
    Key256 Hp_pi = hash_to_point(ring[real_index]);
    Key256 R_pi;
    if (crypto_scalarmult_ed25519_noclamp(R_pi.data(), alpha.data(), Hp_pi.data()) != 0) {
        secure_wipe(alpha);
        throw std::runtime_error("Fallo al calcular R_pi en firma de anillo.");
    }

    std::vector<Key256> c(n);
    // c_{pi+1} = H(m, L_pi, R_pi)
    size_t next = (real_index + 1) % n;
    c[next] = ring_hash_challenge(message_hash, L_pi, R_pi);

    // 3. Simular el anillo para todos los señuelos i != pi
    for (size_t step = 1; step < n; ++step) {
        size_t i = (real_index + step) % n;
        size_t i_next = (i + 1) % n;

        // Generar respuesta aleatoria s_i para el señuelo
        crypto_core_ed25519_scalar_random(sig.responses[i].data());

        // L_i = s_i * G + c_i * P_i
        Key256 sG, cP, L_i;
        crypto_scalarmult_ed25519_base_noclamp(sG.data(), sig.responses[i].data());
        if (crypto_scalarmult_ed25519_noclamp(cP.data(), c[i].data(), ring[i].data()) != 0) {
            secure_wipe(alpha);
            throw std::runtime_error("Fallo al calcular cP en simulación del anillo.");
        }
        if (crypto_core_ed25519_add(L_i.data(), sG.data(), cP.data()) != 0) {
            secure_wipe(alpha);
            throw std::runtime_error("Fallo al sumar sG + cP en simulación del anillo.");
        }

        // R_i = s_i * H_p(P_i) + c_i * I
        Key256 Hp_i = hash_to_point(ring[i]);
        Key256 sHp, cI, R_i;
        if (crypto_scalarmult_ed25519_noclamp(sHp.data(), sig.responses[i].data(), Hp_i.data()) != 0) {
            secure_wipe(alpha);
            throw std::runtime_error("Fallo al calcular sHp en simulación del anillo.");
        }
        if (crypto_scalarmult_ed25519_noclamp(cI.data(), c[i].data(), sig.key_image.data()) != 0) {
            secure_wipe(alpha);
            throw std::runtime_error("Fallo al calcular cI en simulación del anillo.");
        }
        if (crypto_core_ed25519_add(R_i.data(), sHp.data(), cI.data()) != 0) {
            secure_wipe(alpha);
            throw std::runtime_error("Fallo al sumar sHp + cI en simulación del anillo.");
        }

        // c_{i+1} = H(m, L_i, R_i)
        c[i_next] = ring_hash_challenge(message_hash, L_i, R_i);
    }

    // 4. Cerrar el anillo resolviendo para s_pi: s_pi = alpha - c_pi * x (mod L)
    Key256 cx;
    crypto_core_ed25519_scalar_mul(cx.data(), c[real_index].data(), real_privkey.data());
    crypto_core_ed25519_scalar_sub(sig.responses[real_index].data(), alpha.data(), cx.data());

    sig.c0 = c[0];

    secure_wipe(alpha);
    secure_wipe(cx);

    return sig;
}

bool RingSignatureEngine::verify(
    const Hash256& message_hash,
    const RingSignature& signature
) {
    size_t n = signature.ring_pubkeys.size();
    if (n < 1 || signature.responses.size() != n) {
        return false;
    }

    // 1. Validación estricta de puntos canónicos en curva Ed25519 (AUD-INFO-01)
    if (crypto_core_ed25519_is_valid_point(signature.key_image.data()) == 0) {
        return false;
    }
    for (const auto& pk : signature.ring_pubkeys) {
        if (crypto_core_ed25519_is_valid_point(pk.data()) == 0) {
            return false;
        }
    }

    // 2. Mitigación completa de subgrupo de baja torsión y cofactor 8 (AUD-INFO-01 & AUD-CRIT-01)
    Key256 I8;
    static const unsigned char eight_scalar[32] = {8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (crypto_scalarmult_ed25519_noclamp(I8.data(), eight_scalar, signature.key_image.data()) != 0) {
        return false;
    }
    static const uint8_t ed25519_identity[32] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    if (sodium_memcmp(I8.data(), ed25519_identity, 32) == 0) {
        return false; // Rechazar imágenes de clave en subgrupo de torsión pequeña pura
    }

    Key256 current_c = signature.c0;

    for (size_t i = 0; i < n; ++i) {
        // L_i = s_i * G + c_i * P_i
        Key256 sG, cP, L_i;
        if (crypto_scalarmult_ed25519_base_noclamp(sG.data(), signature.responses[i].data()) != 0) {
            return false; // Verificación estricta de retorno (AUD-H0-P2-01)
        }
        if (crypto_scalarmult_ed25519_noclamp(cP.data(), current_c.data(), signature.ring_pubkeys[i].data()) != 0) {
            return false;
        }
        if (crypto_core_ed25519_add(L_i.data(), sG.data(), cP.data()) != 0) {
            return false;
        }

        // R_i = s_i * H_p(P_i) + c_i * I
        Key256 Hp_i = hash_to_point(signature.ring_pubkeys[i]);
        Key256 sHp, cI, R_i;
        if (crypto_scalarmult_ed25519_noclamp(sHp.data(), signature.responses[i].data(), Hp_i.data()) != 0) {
            return false;
        }
        if (crypto_scalarmult_ed25519_noclamp(cI.data(), current_c.data(), signature.key_image.data()) != 0) {
            return false;
        }
        if (crypto_core_ed25519_add(R_i.data(), sHp.data(), cI.data()) != 0) {
            return false;
        }

        // Siguiente desafío en el anillo
        current_c = ring_hash_challenge(message_hash, L_i, R_i);
    }

    // El anillo es válido si regresa exactamente a c0 (tiempo constante AUD-MED-01)
    return sodium_memcmp(current_c.data(), signature.c0.data(), 32) == 0;
}

Hash256 RingSignatureEngine::compute_burn_message_hash(
    const std::string& order_id,
    Amount gross_burned,
    const std::string& destination_address,
    const Key256& change_output_pubkey
) {
    crypto_generichash_state state;
    crypto_generichash_init(&state, nullptr, 0, 32);
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>("BURN_PROOF_V2"), 13);
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(order_id.data()), order_id.size());
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(&gross_burned), sizeof(gross_burned));
    crypto_generichash_update(&state, reinterpret_cast<const uint8_t*>(destination_address.data()), destination_address.size());
    crypto_generichash_update(&state, change_output_pubkey.data(), change_output_pubkey.size());
    Hash256 h;
    crypto_generichash_final(&state, h.data(), 32);
    return h;
}

void RingSignatureEngine::sign_burn_proof(
    const Hash256& burn_message_hash,
    const Key256& one_time_pubkey,
    const Key256& one_time_privkey,
    KeyImage& out_key_image,
    Key256& out_c0,
    Key256& out_s
) {
    // 1. Imagen de clave I = x * H_p(P)
    out_key_image = compute_key_image(one_time_privkey, one_time_pubkey);

    // 2. Secreto efímero alfa
    Key256 alpha;
    crypto_core_ed25519_scalar_random(alpha.data());

    // 3. L = alpha * G
    Key256 L;
    if (crypto_scalarmult_ed25519_base_noclamp(L.data(), alpha.data()) != 0) {
        secure_wipe(alpha);
        throw std::runtime_error("Fallo al calcular L en prueba de quema.");
    }

    // 4. R = alpha * H_p(P)
    Key256 Hp = hash_to_point(one_time_pubkey);
    Key256 R;
    if (crypto_scalarmult_ed25519_noclamp(R.data(), alpha.data(), Hp.data()) != 0) {
        secure_wipe(alpha);
        throw std::runtime_error("Fallo al calcular R en prueba de quema.");
    }

    // 5. Desafío c0 = H(m, L, R)
    out_c0 = ring_hash_challenge(burn_message_hash, L, R);

    // 6. Respuesta s = alpha - c0 * x (mod L)
    Key256 cx;
    crypto_core_ed25519_scalar_mul(cx.data(), out_c0.data(), one_time_privkey.data());
    crypto_core_ed25519_scalar_sub(out_s.data(), alpha.data(), cx.data());

    secure_wipe(alpha);
    secure_wipe(cx);
}

bool RingSignatureEngine::verify_burn_proof(
    const Hash256& burn_message_hash,
    const Key256& one_time_pubkey,
    const KeyImage& key_image,
    const Key256& c0,
    const Key256& s
) {
    if (crypto_core_ed25519_is_valid_point(one_time_pubkey.data()) == 0) {
        return false;
    }
    if (crypto_core_ed25519_is_valid_point(key_image.data()) == 0) {
        return false;
    }

    // Mitigación de cofactor 8 en key image
    unsigned char ki_8[32];
    static const unsigned char eight_scalar[32] = {8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (crypto_scalarmult_ed25519_noclamp(ki_8, eight_scalar, key_image.data()) != 0) {
        return false;
    }
    static const uint8_t ed25519_identity[32] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    if (sodium_memcmp(ki_8, ed25519_identity, 32) == 0) {
        return false;
    }

    // L = s * G + c0 * P
    Key256 sG, cP, L;
    if (crypto_scalarmult_ed25519_base_noclamp(sG.data(), s.data()) != 0) {
        return false;
    }
    if (crypto_scalarmult_ed25519_noclamp(cP.data(), c0.data(), one_time_pubkey.data()) != 0) {
        return false;
    }
    if (crypto_core_ed25519_add(L.data(), sG.data(), cP.data()) != 0) {
        return false;
    }

    // R = s * H_p(P) + c0 * I
    Key256 Hp = hash_to_point(one_time_pubkey);
    Key256 sHp, cI, R;
    if (crypto_scalarmult_ed25519_noclamp(sHp.data(), s.data(), Hp.data()) != 0) {
        return false;
    }
    if (crypto_scalarmult_ed25519_noclamp(cI.data(), c0.data(), key_image.data()) != 0) {
        return false;
    }
    if (crypto_core_ed25519_add(R.data(), sHp.data(), cI.data()) != 0) {
        return false;
    }

    Key256 c_check = ring_hash_challenge(burn_message_hash, L, R);
    return (sodium_memcmp(c0.data(), c_check.data(), 32) == 0);
}

bool KeyImageLedger::register_key_image(const KeyImage& image) {
    std::string hex_img = to_hex(canonical_key_image(image));
    if (spent_images_.find(hex_img) != spent_images_.end()) {
        return false; // Ya gastado (Doble Gasto)
    }
    spent_images_.insert(hex_img);
    return true;
}

bool KeyImageLedger::is_spent(const KeyImage& image) const {
    return spent_images_.find(to_hex(canonical_key_image(image))) != spent_images_.end();
}

void KeyImageLedger::restore_key_image(const KeyImage& image) {
    // Las imágenes recuperadas desde LMDB ya fueron canonicalizadas a (8 * I).
    spent_images_.insert(to_hex(image));
}

} // namespace crypto
