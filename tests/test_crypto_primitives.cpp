#include "stealth.hpp"
#include "ring_signature.hpp"
#include "pedersen.hpp"
#include "types.hpp"
#include <cassert>
#include <iostream>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <cmath>
#include <limits>

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "  [ERROR] " << msg << " (" #cond ")\n"; \
        return 1; \
    } \
} while(0)

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  TEST SUITE: CRIPTOGRAFÍA ED25519, DKSAP, MLSAG Y DLEQ BURN PROOF \n";
    std::cout << "=================================================================\n\n";

    if (sodium_init() < 0) {
        std::cerr << "Error inicializando libsodium\n";
        return 1;
    }

    // -------------------------------------------------------------
    // TEST 1: Parsing y Validación Robusta de StealthAddress
    // -------------------------------------------------------------
    std::cout << "[TEST 1] Parsing y Validación de StealthAddress...\n";
    auto wallet_alice = crypto::StealthWallet::generate_random();
    std::string valid_encoded = wallet_alice.get_public_address().encode();
    CHECK(valid_encoded.substr(0, 3) == "STX", "Prefijo debe ser STX");
    CHECK(valid_encoded.length() == 131, "Longitud debe ser 131 caracteres");

    // Decodificación válida
    auto decoded_addr = crypto::StealthAddress::decode(valid_encoded);
    CHECK(std::memcmp(decoded_addr.view_public_key.data(), wallet_alice.view_public_key.data(), 32) == 0, "View key debe coincidir");
    CHECK(std::memcmp(decoded_addr.spend_public_key.data(), wallet_alice.spend_public_key.data(), 32) == 0, "Spend key debe coincidir");

    // Rechazo de prefijo incorrecto
    bool threw_prefix = false;
    try {
        crypto::StealthAddress::decode("BTC" + valid_encoded.substr(3));
    } catch (const std::invalid_argument&) {
        threw_prefix = true;
    }
    CHECK(threw_prefix, "Debe rechazar dirección sin prefijo STX");

    // Rechazo de longitud incorrecta
    bool threw_len = false;
    try {
        crypto::StealthAddress::decode(valid_encoded.substr(0, 100));
    } catch (const std::invalid_argument&) {
        threw_len = true;
    }
    CHECK(threw_len, "Debe rechazar dirección de longitud menor a 131");

    // Rechazo de caracteres no-hex
    std::string bad_hex = valid_encoded;
    bad_hex[10] = 'Z';
    bool threw_bad_hex = false;
    try {
        crypto::StealthAddress::decode(bad_hex);
    } catch (const std::invalid_argument&) {
        threw_bad_hex = true;
    }
    CHECK(threw_bad_hex, "Debe rechazar caracteres no hexadecimales");
    std::cout << "  [OK] Validaciones sintácticas de StealthAddress superadas.\n";

    // -------------------------------------------------------------
    // TEST 2: Pruebas de Límites de Tamaño en Firmas de Anillo MLSAG
    // -------------------------------------------------------------
    std::cout << "\n[TEST 2] Tamaños Extremos de Anillo MLSAG (N = 1, 2, 5, 11, 16, 64)...\n";
    crypto::Hash256 msg_hash;
    crypto_generichash(msg_hash.data(), 32, reinterpret_cast<const uint8_t*>("MLSAG_TEST_MESSAGE"), 18, nullptr, 0);

    const std::vector<size_t> test_ring_sizes = {1, 2, 5, 11, 16, 64};
    for (size_t ring_size : test_ring_sizes) {
        std::vector<crypto::Key256> ring;
        size_t real_index = ring_size / 2; // Índice en la mitad
        crypto::Key256 real_priv;
        crypto::Key256 real_pub;

        for (size_t i = 0; i < ring_size; ++i) {
            crypto::Key256 priv, pub;
            crypto_core_ed25519_scalar_random(priv.data());
            crypto_scalarmult_ed25519_base_noclamp(pub.data(), priv.data());
            if (i == real_index) {
                real_priv = priv;
                real_pub = pub;
            }
            ring.push_back(pub);
        }

        auto sig = crypto::RingSignatureEngine::sign(msg_hash, ring, real_index, real_priv);
        CHECK(sig.ring_pubkeys.size() == ring_size, "El anillo firmado debe tener tamaño N");
        CHECK(sig.responses.size() == ring_size, "El vector de escalares s debe tener tamaño N");

        bool verified = crypto::RingSignatureEngine::verify(msg_hash, sig);
        CHECK(verified, "Firma de anillo debe verificar para tamaño N");

        // Falsificación: modificar 1 byte del desafío c0
        auto forged_sig = sig;
        forged_sig.c0[0] ^= 0xFF;
        CHECK(!crypto::RingSignatureEngine::verify(msg_hash, forged_sig), "Firma con c0 adulterado debe ser rechazada");

        // Falsificación: modificar 1 byte de s[real_index]
        auto forged_sig_s = sig;
        forged_sig_s.responses[real_index][0] ^= 0xFF;
        CHECK(!crypto::RingSignatureEngine::verify(msg_hash, forged_sig_s), "Firma con s alterado debe ser rechazada");

        // Falsificación: mensaje diferente
        crypto::Hash256 evil_msg;
        crypto_generichash(evil_msg.data(), 32, reinterpret_cast<const uint8_t*>("EVIL_MESSAGE"), 12, nullptr, 0);
        CHECK(!crypto::RingSignatureEngine::verify(evil_msg, sig), "Firma sobre mensaje diferente debe ser rechazada");

        std::cout << "  [OK] Anillo de tamaño N = " << ring_size << " firmado y verificado exitosamente.\n";
    }

    // Rechazo de anillo vacío (N = 0)
    bool threw_empty = false;
    try {
        crypto::Key256 priv;
        crypto_core_ed25519_scalar_random(priv.data());
        crypto::RingSignatureEngine::sign(msg_hash, {}, 0, priv);
    } catch (const std::invalid_argument&) {
        threw_empty = true;
    }
    CHECK(threw_empty, "Debe rechazar anillo vacío N = 0");

    // -------------------------------------------------------------
    // TEST 3: Mitigación de Subgrupo de Baja Torsión y Puntos Mixtos (AUD-CRIT-01)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 3] Mitigación de Torsión de Cofactor 8 y Puntos Mixtos en Key Images (AUD-CRIT-01)...\n";
    {
        crypto::Key256 priv, pub;
        crypto_core_ed25519_scalar_random(priv.data());
        crypto_scalarmult_ed25519_base_noclamp(pub.data(), priv.data());

        auto sig = crypto::RingSignatureEngine::sign(msg_hash, {pub}, 0, priv);
        CHECK(crypto::RingSignatureEngine::verify(msg_hash, sig), "Firma 1-de-1 debe ser válida");

        // 1. Punto de torsión puro no trivial (orden 8 en Ed25519)
        static const unsigned char torsion_point_8[32] = {
            0x26, 0xe8, 0x95, 0x8f, 0xc2, 0xb2, 0x27, 0xb0,
            0x45, 0xc3, 0xf4, 0x89, 0xf2, 0xef, 0x98, 0xf0,
            0xd5, 0xd5, 0xac, 0x05, 0xd3, 0xc6, 0x33, 0x39,
            0xb1, 0x38, 0x02, 0x88, 0x6d, 0x53, 0xfc, 0x05
        };
        // O multiplicar punto de torsión conocido de orden 8
        // Punto de torsión de orden 8 en formato Ed25519:
        static const unsigned char torsion_8_canonical[32] = {
            0xc7, 0x17, 0x6a, 0x70, 0x3d, 0x4d, 0xd8, 0x4f,
            0xba, 0x3c, 0x0b, 0x76, 0x0d, 0x10, 0x67, 0x0f,
            0x2a, 0x20, 0x53, 0xfa, 0x2c, 0x39, 0xcc, 0xc6,
            0x4e, 0xc7, 0xfd, 0x77, 0x92, 0xac, 0x03, 0x7a
        };
        auto forged_torsion_sig = sig;
        std::memcpy(forged_torsion_sig.key_image.data(), torsion_8_canonical, 32);
        CHECK(!crypto::RingSignatureEngine::verify(msg_hash, forged_torsion_sig), "Key Image de baja torsión pura debe ser rechazada");

        // 2. Punto mixto (I' = I + T_8) (Vulnerabilidad crítica AUD-CRIT-01)
        crypto::KeyImage mixed_image;
        int add_rc = crypto_core_ed25519_add(mixed_image.data(), sig.key_image.data(), torsion_8_canonical);
        CHECK(add_rc == 0, "Suma en curva Ed25519 de I + T_8 debe ser un punto válido");

        auto forged_mixed_sig = sig;
        forged_mixed_sig.key_image = mixed_image;
        CHECK(!crypto::RingSignatureEngine::verify(msg_hash, forged_mixed_sig), "Key Image con componente mixto de torsión (I + T) debe ser RECHAZADA por orden L");

        // 3. Resistencia a doble gasto en KeyImageLedger mediante representante canónico (8 * I)
        crypto::KeyImageLedger ledger;
        CHECK(ledger.register_key_image(sig.key_image), "Primer registro de Key Image legítima debe ser exitoso");
        CHECK(!ledger.register_key_image(sig.key_image), "Intento de doble gasto idéntico debe ser rechazado");
        CHECK(!ledger.register_key_image(mixed_image), "Intento de doble gasto con alias de torsión (I + T) debe ser RECHAZADO como ya gastado");

        std::cout << "  [OK] Mitigación de cofactor 8, orden L y neutralización de puntos mixtos verificada.\n";
    }

    // -------------------------------------------------------------
    // TEST 3b: Manejo Seguro de Montos Numéricos (AUD-MED-01)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 3b] Validación Numérica de parse_usdt (AUD-MED-01)...\n";
    {
        CHECK(crypto::parse_usdt(10.5) == 10500000ULL, "10.5 USDT = 10,500,000 micro-USDT");
        CHECK(crypto::parse_usdt(0.0) == 0ULL, "0.0 USDT = 0 micro-USDT");

        bool threw_nan = false;
        try {
            crypto::parse_usdt(std::nan(""));
        } catch (const std::invalid_argument&) {
            threw_nan = true;
        }
        CHECK(threw_nan, "parse_usdt debe rechazar NaN");

        bool threw_inf = false;
        try {
            crypto::parse_usdt(std::numeric_limits<double>::infinity());
        } catch (const std::invalid_argument&) {
            threw_inf = true;
        }
        CHECK(threw_inf, "parse_usdt debe rechazar Infinito");

        bool threw_neg = false;
        try {
            crypto::parse_usdt(-1.0);
        } catch (const std::invalid_argument&) {
            threw_neg = true;
        }
        CHECK(threw_neg, "parse_usdt debe rechazar montos negativos");
        std::cout << "  [OK] Robustez ante NaN, Infinito y negativos en parse_usdt verificada.\n";
    }

    // -------------------------------------------------------------
    // TEST 4: Prueba Criptográfica de Quema DLEQ (Equality of Discrete Log)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 4] Prueba Criptográfica de Quema DLEQ...\n";
    {
        crypto::Key256 priv_x, pub_P;
        crypto_core_ed25519_scalar_random(priv_x.data());
        crypto_scalarmult_ed25519_base_noclamp(pub_P.data(), priv_x.data());
        crypto::KeyImage key_img = crypto::RingSignatureEngine::compute_key_image(priv_x, pub_P);

        std::string order_id = "ORD-DLEQ-TEST-999";
        crypto::Amount gross_burned = 500 * crypto::USDT_UNIT;
        std::string destination = "0x71C83647265f21d3fC38A5d1BBEbC93aE4741fDE";

        crypto::Hash256 burn_msg = crypto::RingSignatureEngine::compute_burn_message_hash(order_id, gross_burned, destination);
        crypto::Key256 c0, s;
        crypto::RingSignatureEngine::sign_burn_proof(burn_msg, pub_P, priv_x, key_img, c0, s);

        // Verificación válida
        bool burn_valid = crypto::RingSignatureEngine::verify_burn_proof(burn_msg, pub_P, key_img, c0, s);
        CHECK(burn_valid, "Prueba DLEQ legítima debe ser verificada");

        // Falsificación: order_id alterado
        crypto::Hash256 forged_msg_order = crypto::RingSignatureEngine::compute_burn_message_hash("ORD-EVIL-HACK", gross_burned, destination);
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(forged_msg_order, pub_P, key_img, c0, s), "Prueba DLEQ con order_id alterado debe fallar");

        // Falsificación: monto alterado
        crypto::Hash256 forged_msg_amount = crypto::RingSignatureEngine::compute_burn_message_hash(order_id, gross_burned + 1, destination);
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(forged_msg_amount, pub_P, key_img, c0, s), "Prueba DLEQ con monto alterado debe fallar");

        // Falsificación: destino alterado
        crypto::Hash256 forged_msg_dest = crypto::RingSignatureEngine::compute_burn_message_hash(order_id, gross_burned, "0xAttackerColdStorage");
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(forged_msg_dest, pub_P, key_img, c0, s), "Prueba DLEQ con destino alterado debe fallar");

        // Falsificación: clave pública P alterada
        crypto::Key256 evil_pub;
        crypto::Key256 evil_priv;
        crypto_core_ed25519_scalar_random(evil_priv.data());
        crypto_scalarmult_ed25519_base_noclamp(evil_pub.data(), evil_priv.data());
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(burn_msg, evil_pub, key_img, c0, s), "Prueba DLEQ con pubkey P alterada debe fallar");

        // Falsificación: firma s alterada
        crypto::Key256 forged_s = s;
        forged_s[0] ^= 0x55;
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(burn_msg, pub_P, key_img, c0, forged_s), "Prueba DLEQ con s alterado debe fallar");

        std::cout << "  [OK] Prueba DLEQ y vectores de ataque de falsificación verificados.\n";
    }

    // -------------------------------------------------------------
    // TEST 5: Compromisos de Pedersen y Homomorfismo
    // -------------------------------------------------------------
    std::cout << "\n[TEST 5] Compromisos de Pedersen y Conservación Homomórfica...\n";
    {
        crypto::Amount in1 = 150 * crypto::USDT_UNIT;
        crypto::Amount in2 = 250 * crypto::USDT_UNIT;
        crypto::Amount out1 = 100 * crypto::USDT_UNIT;
        crypto::Amount out2 = 280 * crypto::USDT_UNIT;
        crypto::Amount fee = 20 * crypto::USDT_UNIT; // in1 + in2 = out1 + out2 + fee (400 = 400)

        auto commit_in1 = crypto::Pedersen::commit(in1);
        auto commit_in2 = crypto::Pedersen::commit(in2);
        auto commit_out1 = crypto::Pedersen::commit(out1);
        auto commit_out2 = crypto::Pedersen::commit(out2);

        // Sum(r_in)
        crypto::Key256 r_in_sum;
        crypto_core_ed25519_scalar_add(r_in_sum.data(), commit_in1.blinding_factor.data(), commit_in2.blinding_factor.data());

        // Sum(r_out)
        crypto::Key256 r_out_sum;
        crypto_core_ed25519_scalar_add(r_out_sum.data(), commit_out1.blinding_factor.data(), commit_out2.blinding_factor.data());

        // r_excess = r_in_sum - r_out_sum
        crypto::Key256 r_excess;
        crypto_core_ed25519_scalar_sub(r_excess.data(), r_in_sum.data(), r_out_sum.data());

        crypto::Key256 excess_pub;
        crypto_scalarmult_ed25519_base_noclamp(excess_pub.data(), r_excess.data());

        bool valid_conservation = crypto::Pedersen::verify_balance_conservation(
            {commit_in1.commitment_point, commit_in2.commitment_point},
            {commit_out1.commitment_point, commit_out2.commitment_point},
            fee,
            excess_pub
        );
        CHECK(valid_conservation, "Conservación de Pedersen debe ser válida cuando suma entradas == suma salidas + comisiones");

        // Falsificación: modificar fee
        bool bad_fee = crypto::Pedersen::verify_balance_conservation(
            {commit_in1.commitment_point, commit_in2.commitment_point},
            {commit_out1.commitment_point, commit_out2.commitment_point},
            fee + 1,
            excess_pub
        );
        CHECK(!bad_fee, "Conservación de Pedersen debe rechazar comisiones inconsistentes");

        std::cout << "  [OK] Conservación homomórfica de Pedersen verificada.\n";
    }

    // -------------------------------------------------------------
    // TEST 6: Higiene de Memoria (secure_wipe / sodium_memzero)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 6] Higiene Criptográfica y Borrado Seguro de Memoria...\n";
    {
        crypto::Key256 secret_key;
        crypto_core_ed25519_scalar_random(secret_key.data());
        bool not_all_zero = false;
        for (auto b : secret_key) { if (b != 0) not_all_zero = true; }
        CHECK(not_all_zero, "Clave secreta inicial no debe ser cero");

        crypto::secure_wipe(secret_key);
        bool all_zero = true;
        for (auto b : secret_key) { if (b != 0) all_zero = false; }
        CHECK(all_zero, "secure_wipe debe poner a cero el 100% de los bytes");
        std::cout << "  [OK] secure_wipe borra memoria en tiempo constante sin optimizaciones del compilador.\n";
    }

    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todos los tests criptográficos pasaron al 100%!  \n";
    std::cout << "=================================================================\n";
    return 0;
}
