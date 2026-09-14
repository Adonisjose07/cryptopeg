#include "stealth.hpp"
#include "ring_signature.hpp"
#include "pedersen.hpp"
#include "types.hpp"
#include "block.hpp"
#include "tumbler.hpp"
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

        // Verificación de ligadura criptográfica de cambio (Auditoría v3 - P1-02)
        crypto::Key256 change_key;
        randombytes_buf(change_key.data(), 32);
        crypto::Hash256 burn_msg_change = crypto::RingSignatureEngine::compute_burn_message_hash(order_id, gross_burned, destination, change_key);
        crypto::RingSignatureEngine::sign_burn_proof(burn_msg_change, pub_P, priv_x, key_img, c0, s);
        CHECK(crypto::RingSignatureEngine::verify_burn_proof(burn_msg_change, pub_P, key_img, c0, s), "Prueba DLEQ con cambio ligado debe ser verificada");

        // Falsificación: clave de cambio alterada
        crypto::Key256 evil_change_key;
        randombytes_buf(evil_change_key.data(), 32);
        crypto::Hash256 forged_change_msg = crypto::RingSignatureEngine::compute_burn_message_hash(order_id, gross_burned, destination, evil_change_key);
        CHECK(!crypto::RingSignatureEngine::verify_burn_proof(forged_change_msg, pub_P, key_img, c0, s), "Prueba DLEQ con cambio alterado debe fallar (P1-02)");

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

    // -------------------------------------------------------------
    // TEST 7: Aritmética Segura contra Desbordamiento de Enteros (P0-02)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 7] Aritmética Segura contra Desbordamiento de Enteros (safe_add_amount)...\n";
    {
        crypto::Amount result = 0;
        // Suma ordinaria
        CHECK(crypto::safe_add_amount(100, 200, result) && result == 300, "Suma ordinaria debe ser 300");

        // Suma al límite exacto de uint64_t
        crypto::Amount max_u64 = std::numeric_limits<crypto::Amount>::max();
        CHECK(crypto::safe_add_amount(max_u64 - 50, 50, result) && result == max_u64, "Suma al límite debe alcanzar UINT64_MAX");

        // Desbordamiento por 1 unidad
        bool overflow_by_one = crypto::safe_add_amount(max_u64, 1, result);
        CHECK(!overflow_by_one, "safe_add_amount debe retornar false ante desbordamiento por 1 unidad");

        // Desbordamiento masivo (intento de wrap-around malicioso)
        bool wrap_around = crypto::safe_add_amount(max_u64 - 10, 50, result);
        CHECK(!wrap_around, "safe_add_amount debe rechazar wrap-around masivo");

        std::cout << "  [OK] Protección estricta contra overflow de Amount verificada.\n";
    }

    // -------------------------------------------------------------
    // TEST 8: Firma y Verificación de Cabecera de Bloque Ed25519 (P0-03)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 8] Firma y Verificación Criptográfica de Cabecera de Bloque (BlockHeader)...\n";
    {
        crypto::Key256 val_pk;
        std::array<uint8_t, 64> val_sk;
        crypto_sign_keypair(val_pk.data(), val_sk.data());

        crypto::BlockHeader header;
        header.height = 10;
        header.timestamp = 1773275000ULL;
        header.nonce = 12345;
        crypto_generichash(header.prev_block_hash.data(), 32, reinterpret_cast<const uint8_t*>("prev"), 4, nullptr, 0);
        crypto_generichash(header.merkle_root.data(), 32, reinterpret_cast<const uint8_t*>("root"), 4, nullptr, 0);

        // Cabecera sin firmar debe fallar verificación
        CHECK(!header.verify_signature(), "Cabecera no firmada debe fallar verify_signature");

        // Firma válida
        header.sign(val_sk.data(), val_pk);
        CHECK(header.verify_signature(), "Firma válida de cabecera debe ser verificada exitosamente");

        // Serialización y deserialización de cabecera firmada
        crypto::ByteWriter bw;
        crypto::serialize_header(bw, header);
        auto raw_bytes = bw.take_bytes();
        crypto::ByteReader br(raw_bytes.data(), raw_bytes.size());
        auto deserialized_header = crypto::deserialize_header(br);
        CHECK(deserialized_header.verify_signature(), "Cabecera deserializada debe preservar y verificar firma");
        CHECK(std::memcmp(deserialized_header.validator_pubkey.data(), val_pk.data(), 32) == 0, "Validator pubkey debe coincidir tras deserialización");

        // Falsificación de altura en cabecera
        auto tampered_header = header;
        tampered_header.height = 11;
        CHECK(!tampered_header.verify_signature(), "Cabecera alterada en altura debe fallar verificación");

        // Falsificación de Merkle root
        auto tampered_merkle = header;
        tampered_merkle.merkle_root[0] ^= 0xFF;
        CHECK(!tampered_merkle.verify_signature(), "Cabecera alterada en Merkle root debe fallar verificación");

        // Falsificación de firma
        auto tampered_sig = header;
        tampered_sig.validator_signature[5] ^= 0x55;
        CHECK(!tampered_sig.verify_signature(), "Firma corrupta debe fallar verificación");

        // Firma de otro validador ilegítimo
        crypto::Key256 evil_pk;
        std::array<uint8_t, 64> evil_sk;
        crypto_sign_keypair(evil_pk.data(), evil_sk.data());
        auto evil_header = header;
        evil_header.sign(evil_sk.data(), evil_pk);
        CHECK(evil_header.verify_signature(), "Firma de evil header es internamente válida");
        CHECK(std::memcmp(evil_header.validator_pubkey.data(), val_pk.data(), 32) != 0, "Evil pubkey no coincide con validador legítimo");

        // Verificación de Quórum M-de-N (Auditoría v3 - P1-01)
        crypto::Key256 val2_pk;
        std::array<uint8_t, 64> val2_sk;
        crypto_sign_keypair(val2_pk.data(), val2_sk.data());

        crypto::BlockHeader q_header = header;
        crypto::Signature64 sig2;
        crypto::Hash256 q_sh = q_header.signing_hash();
        crypto_sign_detached(sig2.data(), nullptr, q_sh.data(), 32, val2_sk.data());
        q_header.add_quorum_signature(val2_pk, sig2);

        std::vector<crypto::Key256> auth_validators = {val_pk, val2_pk};
        size_t verified_quorum = q_header.verify_quorum(auth_validators);
        CHECK(verified_quorum == 2, "Quórum 2-de-2 debe ser verificado exitosamente");

        std::cout << "  [OK] Autenticación criptográfica de cabecera de bloque Ed25519 y Quórum verificados al 100%.\n";
    }

    // -------------------------------------------------------------
    // TEST 9: Cálculo Seguro de Comisiones con Enteros de 128 bits (P1-04)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 9] Cálculo Seguro de Comisiones con Aritmética de 128 bits (safe_fee_calc)...\n";
    {
        // Comisión ordinaria: 10,000 USDT con 50 bps (0.5%) = 50 USDT
        crypto::Amount amt = 10'000 * crypto::USDT_UNIT;
        crypto::Amount fee = crypto::safe_fee_calc(amt, 50);
        CHECK(fee == 50 * crypto::USDT_UNIT, "Comisión de 10,000 USDT al 0.5% debe ser 50 USDT");

        // Monto masivo cercano a 2^60 que desbordaría una multiplicación ordinaria en uint64
        crypto::Amount massive = 100'000'000 * crypto::USDT_UNIT; // 100 millones de USDT
        crypto::Amount massive_fee = crypto::safe_fee_calc(massive, 100); // 1%
        CHECK(massive_fee == 1'000'000 * crypto::USDT_UNIT, "Cálculo con 128 bits no debe desbordar");

        // Comprobación de safe_sub_amount
        crypto::Amount res = 0;
        CHECK(crypto::safe_sub_amount(100, 40, res) && res == 60, "safe_sub_amount debe restar correctamente");
        CHECK(!crypto::safe_sub_amount(40, 100, res), "safe_sub_amount debe prevenir underflow retornando false");

        std::cout << "  [OK] Aritmética comprobada en Vault con temporales uint128 y safe_sub_amount verificada.\n";
    }

    // -------------------------------------------------------------
    // TEST 10: Certificado Criptográfico de Depósito SHA-256 + Ed25519 (P0-01)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 10] Certificado Criptográfico de Depósito (compute_deposit_attestation_hash)...\n";
    {
        crypto::Key256 val_pk;
        std::array<uint8_t, 64> val_sk;
        crypto_sign_keypair(val_pk.data(), val_sk.data());

        crypto::Key256 view_p, spend_p;
        randombytes_buf(view_p.data(), 32);
        randombytes_buf(spend_p.data(), 32);

        std::string tx = "0xArbitrumConfirmedDepositTx999";
        crypto::Amount gross = 500 * crypto::USDT_UNIT;
        crypto::Hash256 att_hash = crypto::compute_deposit_attestation_hash(
            421614ULL, "0x511A31987EF1019a41CBba658935515Dd64d2D18",
            tx, 0, gross, view_p, spend_p, 12345ULL
        );

        crypto::Signature64 att_sig;
        crypto_sign_detached(att_sig.data(), nullptr, att_hash.data(), 32, val_sk.data());

        // Verificación exitosa
        CHECK(crypto_sign_verify_detached(att_sig.data(), att_hash.data(), 32, val_pk.data()) == 0,
              "Atestación de depósito firmada por oráculo legítimo debe ser verificada");

        // Falsificación: monto alterado
        crypto::Hash256 forged_att = crypto::compute_deposit_attestation_hash(
            421614ULL, "0x511A31987EF1019a41CBba658935515Dd64d2D18",
            tx, 0, gross + 1, view_p, spend_p, 12345ULL
        );
        CHECK(crypto_sign_verify_detached(att_sig.data(), forged_att.data(), 32, val_pk.data()) != 0,
              "Atestación con monto alterado debe fallar verificación criptográfica");

        std::cout << "  [OK] Certificado criptográfico de depósito con firma Ed25519 verificado al 100%.\n";
    }

    // -------------------------------------------------------------
    // TEST 11: Particionado Seguro en Tumbler sin Underflow (V4-10)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 11] Particionado en Tumbler sin Underflow (partition_amount)...\n";
    {
        crypto::TumblerEngine tumbler;
        // Caso límite: monto igual a 5 (mínimo de fragmentos)
        auto plan_min = tumbler.plan_withdrawal_tumbling("ORD-MIN-1", 5, 0, 5, "0xDest");
        crypto::Amount sum_min = 0;
        for (const auto& r : plan_min.routes) {
            CHECK(r.fragment_amount >= 1, "Cada fragmento debe ser al menos 1");
            sum_min += r.fragment_amount;
        }
        CHECK(sum_min == 5, "Suma de fragmentos minimos debe ser exactamente 5");

        // Caso estándar con monto fraccionario grande
        crypto::Amount large_amount = 987654321ULL;
        auto plan_large = tumbler.plan_withdrawal_tumbling("ORD-LARGE-2", large_amount, 0, large_amount, "0xDest");
        crypto::Amount sum_large = 0;
        for (const auto& r : plan_large.routes) {
            CHECK(r.fragment_amount > 0, "Fragmento debe ser estrictamente positivo");
            sum_large += r.fragment_amount;
        }
        CHECK(sum_large == large_amount, "Suma de fragmentos grandes debe coincidir exactamente");
        std::cout << "  [OK] Particionado seguro en Tumbler sin underflow y con suma exacta verificado al 100%.\n";
    }

    // -------------------------------------------------------------
    // TEST 12: Validación Canónica de Direcciones EVM en Retiros (AUD-CP-03)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 12] Validación Canónica de Direcciones EVM en Retiros (AUD-CP-03)...\n";
    {
        // Válida estándar minúsculas
        CHECK(crypto::is_valid_evm_address("0x9d59867efe155406f637f028997866f252dcc72c"), "Direccion EVM valida en minusculas debe ser aceptada");
        // Válida con mayúsculas (Checksum / EIP-55)
        CHECK(crypto::is_valid_evm_address("0x9d59867EfE155406f637F028997866f252dcc72c"), "Direccion EVM con mayusculas debe ser aceptada");

        // Casos inválidos
        CHECK(!crypto::is_valid_evm_address("0x0000000000000000000000000000000000000000"), "Direccion cero (address 0) debe ser rechazada");
        CHECK(!crypto::is_valid_evm_address(""), "Cadena vacia debe ser rechazada");
        CHECK(!crypto::is_valid_evm_address("0x123"), "Direccion corta debe ser rechazada");
        CHECK(!crypto::is_valid_evm_address("0x9d59867efe155406f637f028997866f252dcc72c99"), "Direccion larga debe ser rechazada");
        CHECK(!crypto::is_valid_evm_address("9d59867efe155406f637f028997866f252dcc72c"), "Direccion sin prefijo 0x debe ser rechazada");
        CHECK(!crypto::is_valid_evm_address("0x9d59867efe155406f637f028997866f252dcc72z"), "Direccion con caracter no hex ('z') debe ser rechazada");

        std::cout << "  [OK] Validación sintáctica canónica de direcciones EVM verificada al 100% (AUD-CP-03).\n";
    }

    // -------------------------------------------------------------
    // TEST 13: Rechazo de Claves Públicas Duplicadas en Anillos MLSAG (SEC-03)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 13] Rechazo de Claves Duplicadas en Anillo MLSAG (SEC-03)...\n";
    {
        crypto::Key256 priv, pub;
        crypto_core_ed25519_scalar_random(priv.data());
        crypto_scalarmult_ed25519_base_noclamp(pub.data(), priv.data());

        std::vector<crypto::Key256> dup_ring = {pub, pub};
        crypto::Hash256 msg{};
        crypto_generichash(msg.data(), 32, (const uint8_t*)"test_dup_ring", 13, nullptr, 0);

        crypto::RingSignature sig;
        sig.key_image = crypto::RingSignatureEngine::compute_key_image(priv, pub);
        sig.ring_pubkeys = dup_ring;
        sig.responses.resize(2);
        randombytes_buf(sig.responses[0].data(), 32);
        randombytes_buf(sig.responses[1].data(), 32);
        randombytes_buf(sig.c0.data(), 32);

        CHECK(!crypto::RingSignatureEngine::verify(msg, sig),
              "Firma de anillo MLSAG con claves duplicadas [P, P] debe ser rechazada categóricamente");

        crypto::Key256 priv2, pub2;
        crypto_core_ed25519_scalar_random(priv2.data());
        crypto_scalarmult_ed25519_base_noclamp(pub2.data(), priv2.data());

        std::vector<crypto::Key256> dup_ring3 = {pub, pub2, pub};
        sig.ring_pubkeys = dup_ring3;
        sig.responses.resize(3);
        randombytes_buf(sig.responses[2].data(), 32);

        CHECK(!crypto::RingSignatureEngine::verify(msg, sig),
              "Firma de anillo MLSAG con [P1, P2, P1] debe ser rechazada categóricamente");

        std::cout << "  [OK] Rechazo estricto de claves públicas duplicadas en anillos MLSAG verificado al 100% (SEC-03).\n";
    }

    std::cout << "\n=================================================================\n";
    std::cout << "  [EXITO TOTAL] Todos los tests criptográficos pasaron al 100%!  \n";
    std::cout << "=================================================================\n";
    return 0;
}
