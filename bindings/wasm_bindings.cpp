#include <emscripten/bind.h>
#include <emscripten/val.h>
#include "types.hpp"
#include "stealth.hpp"
#include "ring_signature.hpp"
#include "pedersen.hpp"

using namespace emscripten;
using namespace crypto;

EMSCRIPTEN_BINDINGS(cryptopeg_wasm) {
    register_vector<std::string>("StringVector");

    // =========================================================================
    // 1. Protocolo de Direcciones Furtivas DKSAP en Wasm
    // =========================================================================
    class_<StealthProtocol>("StealthProtocol")
        // Crear salida de un solo uso para un destinatario furtivo
        .class_function("createOneTimeOutput", optional_override([](std::string spendPubHex, std::string viewPubHex, double amountUsdt) {
            StealthAddress addr;
            auto s_bytes = from_hex(spendPubHex);
            auto v_bytes = from_hex(viewPubHex);
            if (s_bytes.size() != 32 || v_bytes.size() != 32) {
                throw std::invalid_argument("Las claves deben tener 32 bytes.");
            }
            std::memcpy(addr.spend_public_key.data(), s_bytes.data(), 32);
            std::memcpy(addr.view_public_key.data(), v_bytes.data(), 32);

            Amount amt = parse_usdt(amountUsdt);
            auto out = StealthProtocol::create_one_time_output(addr, amt);

            val res = val::object();
            res.set("ephemeral_pubkey", to_hex(out.ephemeral_public_key));
            res.set("destination_one_time", to_hex(out.destination_one_time));
            res.set("amount_units", static_cast<double>(out.amount));
            res.set("amount_usdt", amountUsdt);
            return res;
        }))

        // Escanear si una salida pertenece a la View Key del usuario
        .class_function("scanOutput", optional_override([](
            std::string viewPrivHex,
            std::string spendPubHex,
            std::string ephemeralPubHex,
            std::string destOneTimeHex
        ) {
            StealthWallet w;
            auto vp_bytes = from_hex(viewPrivHex);
            auto sp_bytes = from_hex(spendPubHex);
            if (vp_bytes.size() != 32 || sp_bytes.size() != 32) return false;
            std::memcpy(w.view_private_key.data(), vp_bytes.data(), 32);
            std::memcpy(w.spend_public_key.data(), sp_bytes.data(), 32);

            OneTimeOutput out;
            auto ep_bytes = from_hex(ephemeralPubHex);
            auto dst_bytes = from_hex(destOneTimeHex);
            if (ep_bytes.size() != 32 || dst_bytes.size() != 32) return false;
            std::memcpy(out.ephemeral_public_key.data(), ep_bytes.data(), 32);
            std::memcpy(out.destination_one_time.data(), dst_bytes.data(), 32);

            bool match = StealthProtocol::scan_output(w, out);
            secure_wipe(w.view_private_key);
            return match;
        }))

        // Derivar clave privada de gasto de un solo uso x = H_s(rA) + a
        .class_function("deriveOneTimePrivateKey", optional_override([](
            std::string viewPrivHex,
            std::string spendPrivHex,
            std::string ephemeralPubHex,
            std::string destOneTimeHex
        ) {
            StealthWallet w;
            auto vp_bytes = from_hex(viewPrivHex);
            auto sp_bytes = from_hex(spendPrivHex);
            if (vp_bytes.size() != 32 || sp_bytes.size() != 32) {
                throw std::invalid_argument("Claves de billetera inválidas.");
            }
            std::memcpy(w.view_private_key.data(), vp_bytes.data(), 32);
            std::memcpy(w.spend_private_key.data(), sp_bytes.data(), 32);

            OneTimeOutput out;
            auto ep_bytes = from_hex(ephemeralPubHex);
            auto dst_bytes = from_hex(destOneTimeHex);
            if (ep_bytes.size() != 32 || dst_bytes.size() != 32) {
                secure_wipe(w.view_private_key);
                secure_wipe(w.spend_private_key);
                throw std::invalid_argument("Claves de salida inválidas.");
            }
            std::memcpy(out.ephemeral_public_key.data(), ep_bytes.data(), 32);
            std::memcpy(out.destination_one_time.data(), dst_bytes.data(), 32);

            Key256 x = StealthProtocol::derive_one_time_private_key(w, out);
            secure_wipe(w.view_private_key);
            secure_wipe(w.spend_private_key);
            sodium_memzero(vp_bytes.data(), vp_bytes.size());
            sodium_memzero(sp_bytes.data(), sp_bytes.size());

            std::string x_hex = to_hex(x);
            secure_wipe(x);
            return x_hex;
        }));

    // =========================================================================
    // 2. Motor de Firmas de Anillo RingCT MLSAG en Wasm
    // =========================================================================
    class_<RingSignatureEngine>("RingSignatureEngine")
        // Calcular Imagen de Clave I = x * H_p(P)
        .class_function("computeKeyImage", optional_override([](std::string oneTimePrivHex, std::string destOneTimeHex) {
            Key256 priv;
            auto p_bytes = from_hex(oneTimePrivHex);
            if (p_bytes.size() != 32) throw std::invalid_argument("Clave privada debe tener 32 bytes.");
            std::memcpy(priv.data(), p_bytes.data(), 32);

            Key256 pub;
            auto pb_bytes = from_hex(destOneTimeHex);
            if (pb_bytes.size() != 32) {
                secure_wipe(priv);
                throw std::invalid_argument("Clave pública debe tener 32 bytes.");
            }
            std::memcpy(pub.data(), pb_bytes.data(), 32);

            KeyImage img = RingSignatureEngine::compute_key_image(priv, pub);
            secure_wipe(priv);
            sodium_memzero(p_bytes.data(), p_bytes.size());
            return to_hex(img);
        }))

        // Calcular Hash Canónico BLAKE2b
        .class_function("computeCanonicalTxHash", optional_override([](
            val outputsArray,
            double feeUsdt,
            val ringPubkeysArray,
            std::string keyImageHex
        ) {
            std::vector<OneTimeOutput> outs;
            size_t out_len = outputsArray["length"].as<size_t>();
            for (size_t i = 0; i < out_len; ++i) {
                val item = outputsArray[i];
                OneTimeOutput out;
                auto ep = from_hex(item["ephemeral_pubkey"].as<std::string>());
                auto dst = from_hex(item["destination_one_time"].as<std::string>());
                std::memcpy(out.ephemeral_public_key.data(), ep.data(), 32);
                std::memcpy(out.destination_one_time.data(), dst.data(), 32);
                out.amount = parse_usdt(item["amount_usdt"].as<double>());
                outs.push_back(out);
            }

            Amount fee = parse_usdt(feeUsdt);

            std::vector<Key256> ring;
            size_t ring_len = ringPubkeysArray["length"].as<size_t>();
            for (size_t i = 0; i < ring_len; ++i) {
                auto pk_bytes = from_hex(ringPubkeysArray[i].as<std::string>());
                Key256 pk;
                std::memcpy(pk.data(), pk_bytes.data(), 32);
                ring.push_back(pk);
            }

            KeyImage img;
            auto ki_bytes = from_hex(keyImageHex);
            std::memcpy(img.data(), ki_bytes.data(), 32);

            Hash256 h = RingSignatureEngine::compute_canonical_tx_hash(outs, fee, ring, img);
            return to_hex(h);
        }))

        // Firmar transacción en el navegador con RingCT MLSAG
        .class_function("signTransaction", optional_override([](
            std::string txHashHex,
            val ringPubkeysArray,
            size_t realIndex,
            std::string realPrivkeyHex
        ) {
            Hash256 tx_hash;
            auto th_bytes = from_hex(txHashHex);
            if (th_bytes.size() != 32) throw std::invalid_argument("txHash debe tener 32 bytes.");
            std::memcpy(tx_hash.data(), th_bytes.data(), 32);

            std::vector<Key256> ring;
            size_t len = ringPubkeysArray["length"].as<size_t>();
            for (size_t i = 0; i < len; ++i) {
                auto pk_bytes = from_hex(ringPubkeysArray[i].as<std::string>());
                if (pk_bytes.size() != 32) throw std::invalid_argument("Participante del anillo debe tener 32 bytes.");
                Key256 pk;
                std::memcpy(pk.data(), pk_bytes.data(), 32);
                ring.push_back(pk);
            }

            Key256 priv;
            auto priv_bytes = from_hex(realPrivkeyHex);
            if (priv_bytes.size() != 32) throw std::invalid_argument("Clave privada debe tener 32 bytes.");
            std::memcpy(priv.data(), priv_bytes.data(), 32);

            RingSignature sig = RingSignatureEngine::sign(tx_hash, ring, realIndex, priv);
            secure_wipe(priv);
            sodium_memzero(priv_bytes.data(), priv_bytes.size());

            val sigObj = val::object();
            sigObj.set("key_image", to_hex(sig.key_image));
            sigObj.set("c0", to_hex(sig.c0));

            val ringArr = val::array();
            for (size_t i = 0; i < sig.ring_pubkeys.size(); ++i) {
                ringArr.set(i, to_hex(sig.ring_pubkeys[i]));
            }
            sigObj.set("ring_pubkeys", ringArr);

            val responsesArr = val::array();
            for (size_t i = 0; i < sig.responses.size(); ++i) {
                responsesArr.set(i, to_hex(sig.responses[i]));
            }
            sigObj.set("responses", responsesArr);

            return sigObj;
        }))

        // Verificar firma de anillo MLSAG
        .class_function("verifySignature", optional_override([](
            std::string txHashHex,
            std::string keyImageHex,
            val ringPubkeysArray,
            std::string c0Hex,
            val responsesArray
        ) {
            Hash256 tx_hash;
            auto th_bytes = from_hex(txHashHex);
            if (th_bytes.size() != 32) return false;
            std::memcpy(tx_hash.data(), th_bytes.data(), 32);

            RingSignature sig;
            auto ki_bytes = from_hex(keyImageHex);
            auto c0_bytes = from_hex(c0Hex);
            if (ki_bytes.size() != 32 || c0_bytes.size() != 32) return false;
            std::memcpy(sig.key_image.data(), ki_bytes.data(), 32);
            std::memcpy(sig.c0.data(), c0_bytes.data(), 32);

            size_t ring_len = ringPubkeysArray["length"].as<size_t>();
            for (size_t i = 0; i < ring_len; ++i) {
                auto pk_bytes = from_hex(ringPubkeysArray[i].as<std::string>());
                if (pk_bytes.size() != 32) return false;
                Key256 pk;
                std::memcpy(pk.data(), pk_bytes.data(), 32);
                sig.ring_pubkeys.push_back(pk);
            }

            size_t resp_len = responsesArray["length"].as<size_t>();
            for (size_t i = 0; i < resp_len; ++i) {
                auto s_bytes = from_hex(responsesArray[i].as<std::string>());
                if (s_bytes.size() != 32) return false;
                Key256 s;
                std::memcpy(s.data(), s_bytes.data(), 32);
                sig.responses.push_back(s);
            }

            return RingSignatureEngine::verify(tx_hash, sig);
        }));

    // =========================================================================
    // 3. Inicializador de Libsodium en WebAssembly
    // =========================================================================
    function("initSodium", optional_override([]() {
        return sodium_init() >= 0;
    }));
}
