#include "ring_signature.hpp"
#include <cstring>
#include <stdexcept>

namespace crypto {

Key256 RingSignatureEngine::hash_to_point(const Key256& pubkey) {
    Key256 uniform;
    crypto_generichash(uniform.data(), 32, pubkey.data(), 32, nullptr, 0);

    Key256 Hp;
    if (crypto_core_ed25519_from_uniform(Hp.data(), uniform.data()) != 0) {
        throw std::runtime_error("Fallo al mapear clave pública a punto en la curva.");
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
    return scalar_out;
}

RingSignature RingSignatureEngine::sign(
    const Hash256& message_hash,
    const std::vector<Key256>& ring,
    size_t real_index,
    const Key256& real_privkey
) {
    size_t n = ring.size();
    if (n < 2) {
        throw std::invalid_argument("El anillo de señuelos debe tener al menos 2 participantes.");
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
    if (n < 2 || signature.responses.size() != n) {
        return false;
    }

    Key256 current_c = signature.c0;

    for (size_t i = 0; i < n; ++i) {
        // L_i = s_i * G + c_i * P_i
        Key256 sG, cP, L_i;
        crypto_scalarmult_ed25519_base_noclamp(sG.data(), signature.responses[i].data());
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

    // El anillo es válido si regresa exactamente a c0
    return std::memcmp(current_c.data(), signature.c0.data(), 32) == 0;
}

bool KeyImageLedger::register_key_image(const KeyImage& image) {
    std::string hex_img = to_hex(image);
    if (spent_images_.find(hex_img) != spent_images_.end()) {
        return false; // Ya gastado (Doble Gasto)
    }
    spent_images_.insert(hex_img);
    return true;
}

bool KeyImageLedger::is_spent(const KeyImage& image) const {
    return spent_images_.find(to_hex(image)) != spent_images_.end();
}

void KeyImageLedger::restore_key_image(const KeyImage& image) {
    spent_images_.insert(to_hex(image));
}

} // namespace crypto
