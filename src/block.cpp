#include "block.hpp"
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <sodium.h>

namespace crypto {

namespace {
constexpr uint32_t MAX_BLOCK_TRANSACTIONS = 10000;
constexpr uint32_t MAX_BLOCK_DEPOSITS = 10000;
constexpr uint32_t MAX_BLOCK_WITHDRAWALS = 10000;
constexpr uint32_t MAX_BLOCK_OUTPUTS = 20000;
constexpr uint32_t MAX_TX_OUTPUTS = 256;
}

// 1. OneTimeOutput
void serialize_output(ByteWriter& w, const OneTimeOutput& out) {
    w.write_array(out.ephemeral_public_key);
    w.write_array(out.destination_one_time);
    w.write_u64(out.amount);
}

OneTimeOutput deserialize_output(ByteReader& r) {
    OneTimeOutput out;
    out.ephemeral_public_key = r.read_array<32>();
    out.destination_one_time = r.read_array<32>();
    out.amount = r.read_u64();

    // Validación de puntos canónicos en la curva Ed25519 (AUD-INFO-01)
    if (crypto_core_ed25519_is_valid_point(out.ephemeral_public_key.data()) == 0) {
        throw std::runtime_error("Punto ephemeral_public_key invalido en salida deserializada.");
    }
    if (crypto_core_ed25519_is_valid_point(out.destination_one_time.data()) == 0) {
        throw std::runtime_error("Punto destination_one_time invalido en salida deserializada.");
    }

    return out;
}

// 2. RingSignature
void serialize_ring_sig(ByteWriter& w, const RingSignature& sig) {
    w.write_array(sig.key_image);
    w.write_array(sig.c0);
    w.write_u32(static_cast<uint32_t>(sig.responses.size()));
    for (const auto& resp : sig.responses) {
        w.write_array(resp);
    }
    w.write_u32(static_cast<uint32_t>(sig.ring_pubkeys.size()));
    for (const auto& pk : sig.ring_pubkeys) {
        w.write_array(pk);
    }
}

RingSignature deserialize_ring_sig(ByteReader& r) {
    RingSignature sig;
    sig.key_image = r.read_array<32>();
    sig.c0 = r.read_array<32>();

    // 1. Validar punto de imagen de clave (AUD-INFO-01)
    if (crypto_core_ed25519_is_valid_point(sig.key_image.data()) == 0) {
        throw std::runtime_error("Punto key_image invalido en firma de anillo deserializada.");
    }

    uint32_t resp_count = r.read_u32();
    if (resp_count < 1 || resp_count > 64) {
        throw std::runtime_error("Numero de respuestas de anillo fuera de limites de seguridad (1-64).");
    }
    sig.responses.reserve(resp_count);
    for (uint32_t i = 0; i < resp_count; ++i) {
        sig.responses.push_back(r.read_array<32>());
    }

    uint32_t pk_count = r.read_u32();
    if (pk_count != resp_count) {
        throw std::runtime_error("Discrepancia entre respuestas y claves publicas en anillo.");
    }
    sig.ring_pubkeys.reserve(pk_count);
    for (uint32_t i = 0; i < pk_count; ++i) {
        auto pk = r.read_array<32>();
        // 2. Validar que cada clave pública del anillo sea un punto válido en Ed25519 (AUD-INFO-01)
        if (crypto_core_ed25519_is_valid_point(pk.data()) == 0) {
            throw std::runtime_error("Clave publica del anillo no representa un punto valido en Ed25519.");
        }
        sig.ring_pubkeys.push_back(pk);
    }
    return sig;
}

// 3. ShieldedTransaction
void serialize_tx(ByteWriter& w, const ShieldedTransaction& tx) {
    w.write_array(tx.tx_hash);
    serialize_ring_sig(w, tx.ring_sig);
    w.write_u32(static_cast<uint32_t>(tx.outputs.size()));
    for (const auto& out : tx.outputs) {
        serialize_output(w, out);
    }
    w.write_u64(tx.public_fee);
    w.write_string(tx.timestamp);
}

ShieldedTransaction deserialize_tx(ByteReader& r) {
    ShieldedTransaction tx;
    tx.tx_hash = r.read_array<32>();
    tx.ring_sig = deserialize_ring_sig(r);
    uint32_t out_count = r.read_u32();
    if (out_count > MAX_TX_OUTPUTS) {
        throw std::runtime_error("Numero de outputs de transaccion excede el limite protocolario.");
    }
    tx.outputs.reserve(out_count);
    for (uint32_t i = 0; i < out_count; ++i) {
        tx.outputs.push_back(deserialize_output(r));
    }
    tx.public_fee = r.read_u64();
    tx.timestamp = r.read_string();
    return tx;
}

// 4. DepositReceipt
void serialize_deposit(ByteWriter& w, const DepositReceipt& dep) {
    w.write_u64(dep.gross_usdt_deposited);
    w.write_u64(dep.fee_to_pool);
    w.write_u64(dep.net_shielded_tokens_minted);
    w.write_string(dep.tx_hash);
    w.write_array(dep.recipient_view_pub);
    w.write_array(dep.recipient_spend_pub);
}

DepositReceipt deserialize_deposit(ByteReader& r) {
    DepositReceipt dep;
    dep.gross_usdt_deposited = r.read_u64();
    dep.fee_to_pool = r.read_u64();
    dep.net_shielded_tokens_minted = r.read_u64();
    dep.tx_hash = r.read_string();
    if (r.remaining() >= 64) {
        dep.recipient_view_pub = r.read_array<32>();
        dep.recipient_spend_pub = r.read_array<32>();
    } else {
        dep.recipient_view_pub.fill(0);
        dep.recipient_spend_pub.fill(0);
    }
    return dep;
}

// 5. WithdrawalReceipt
void serialize_withdrawal(ByteWriter& w, const WithdrawalReceipt& wdr) {
    w.write_u64(wdr.gross_tokens_burned);
    w.write_u64(wdr.fee_to_pool);
    w.write_u64(wdr.net_usdt_to_tumble);
    w.write_string(wdr.order_id);
    w.write_string(wdr.destination_address);
    w.write_array(wdr.key_image);
    w.write_array(wdr.burned_utxo_pubkey);
    w.write_array(wdr.burn_signature_c0);
    w.write_array(wdr.burn_signature_s);
    w.write_array(wdr.change_output_pubkey);
}

WithdrawalReceipt deserialize_withdrawal(ByteReader& r) {
    WithdrawalReceipt wdr;
    wdr.gross_tokens_burned = r.read_u64();
    wdr.fee_to_pool = r.read_u64();
    wdr.net_usdt_to_tumble = r.read_u64();
    wdr.order_id = r.read_string();
    if (r.remaining() > 4) {
        wdr.destination_address = r.read_string();
    } else {
        wdr.destination_address = "";
    }
    if (r.remaining() >= 32) {
        wdr.key_image = r.read_array<32>();
    } else {
        wdr.key_image.fill(0);
    }
    if (r.remaining() >= 96) {
        wdr.burned_utxo_pubkey = r.read_array<32>();
        wdr.burn_signature_c0 = r.read_array<32>();
        wdr.burn_signature_s = r.read_array<32>();
    } else {
        wdr.burned_utxo_pubkey.fill(0);
        wdr.burn_signature_c0.fill(0);
        wdr.burn_signature_s.fill(0);
    }
    if (r.remaining() >= 32) {
        wdr.change_output_pubkey = r.read_array<32>();
    } else {
        wdr.change_output_pubkey.fill(0);
    }
    return wdr;
}

// 6. BlockHeader (Auditoría v3 - P1-01 y P1-03: Versionado explícito y Quórum M-de-N)
void serialize_header(ByteWriter& w, const BlockHeader& h) {
    w.write_u32(h.version);
    w.write_u64(h.height);
    w.write_array(h.prev_block_hash);
    w.write_array(h.merkle_root);
    w.write_u64(h.timestamp);
    w.write_u64(h.nonce);
    w.write_array(h.validator_pubkey);
    w.write_array(h.validator_signature);
    w.write_u32(static_cast<uint32_t>(h.quorum_pubkeys.size()));
    for (size_t i = 0; i < h.quorum_pubkeys.size(); ++i) {
        w.write_array(h.quorum_pubkeys[i]);
        if (i < h.quorum_signatures.size()) {
            w.write_array(h.quorum_signatures[i]);
        } else {
            Signature64 empty_sig{};
            w.write_array(empty_sig);
        }
    }
}

BlockHeader deserialize_header(ByteReader& r) {
    BlockHeader h;
    h.version = r.read_u32();
    h.height = r.read_u64();
    h.prev_block_hash = r.read_array<32>();
    h.merkle_root = r.read_array<32>();
    h.timestamp = r.read_u64();
    h.nonce = r.read_u64();
    h.validator_pubkey = r.read_array<32>();
    h.validator_signature = r.read_array<64>();
    if (h.version >= 3 && r.remaining() >= 4) {
        uint32_t q_count = r.read_u32();
        if (q_count > 64) {
            throw std::runtime_error("Número de firmas de quórum excede el límite de seguridad (64).");
        }
        for (uint32_t i = 0; i < q_count; ++i) {
            h.quorum_pubkeys.push_back(r.read_array<32>());
            h.quorum_signatures.push_back(r.read_array<64>());
        }
    }
    return h;
}

Hash256 BlockHeader::signing_hash() const {
    ByteWriter w;
    w.write_u32(version);
    w.write_u64(height);
    w.write_array(prev_block_hash);
    w.write_array(merkle_root);
    w.write_u64(timestamp);
    w.write_u64(nonce);
    Hash256 h;
    crypto_generichash(h.data(), 32, w.get_bytes().data(), w.get_bytes().size(), nullptr, 0);
    return h;
}

void BlockHeader::sign(const uint8_t* secret_key_64, const Key256& pub_key_32) {
    validator_pubkey = pub_key_32;
    Hash256 sh = signing_hash();
    crypto_sign_detached(
        validator_signature.data(), nullptr,
        sh.data(), 32,
        secret_key_64
    );
    add_quorum_signature(pub_key_32, validator_signature);
}

void BlockHeader::add_quorum_signature(const Key256& pub_key_32, const Signature64& sig) {
    for (const auto& pk : quorum_pubkeys) {
        if (sodium_memcmp(pk.data(), pub_key_32.data(), 32) == 0) return;
    }

    // Canonicalizar por clave publica para que el mismo conjunto M-de-N produzca
    // exactamente el mismo header/hash independientemente del orden de llegada.
    size_t pos = 0;
    while (pos < quorum_pubkeys.size() &&
           std::lexicographical_compare(
               quorum_pubkeys[pos].begin(), quorum_pubkeys[pos].end(),
               pub_key_32.begin(), pub_key_32.end())) {
        ++pos;
    }
    quorum_pubkeys.insert(quorum_pubkeys.begin() + static_cast<std::ptrdiff_t>(pos), pub_key_32);
    quorum_signatures.insert(quorum_signatures.begin() + static_cast<std::ptrdiff_t>(pos), sig);
}

bool BlockHeader::verify_signature() const {
    bool is_zero = true;
    for (uint8_t b : validator_pubkey) {
        if (b != 0) { is_zero = false; break; }
    }
    if (is_zero) return false;

    Hash256 sh = signing_hash();
    return (crypto_sign_verify_detached(
        validator_signature.data(),
        sh.data(), 32,
        validator_pubkey.data()
    ) == 0);
}

size_t BlockHeader::verify_quorum(const std::vector<Key256>& authorized_set) const {
    if (authorized_set.empty()) return 0;
    Hash256 sh = signing_hash();
    std::vector<Key256> verified_keys;

    // 1. Verificar firma de cabecera primaria
    if (verify_signature()) {
        for (const auto& auth_pk : authorized_set) {
            if (sodium_memcmp(auth_pk.data(), validator_pubkey.data(), 32) == 0) {
                verified_keys.push_back(validator_pubkey);
                break;
            }
        }
    }

    // 2. Verificar firmas adicionales del vector de quórum
    size_t count = std::min(quorum_pubkeys.size(), quorum_signatures.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& pk = quorum_pubkeys[i];
        const auto& sig = quorum_signatures[i];

        bool is_auth = false;
        for (const auto& auth_pk : authorized_set) {
            if (sodium_memcmp(auth_pk.data(), pk.data(), 32) == 0) {
                is_auth = true;
                break;
            }
        }
        if (!is_auth) continue;

        bool already_counted = false;
        for (const auto& vk : verified_keys) {
            if (sodium_memcmp(vk.data(), pk.data(), 32) == 0) {
                already_counted = true;
                break;
            }
        }
        if (already_counted) continue;

        if (crypto_sign_verify_detached(sig.data(), sh.data(), 32, pk.data()) == 0) {
            verified_keys.push_back(pk);
        }
    }

    return verified_keys.size();
}

Hash256 BlockHeader::hash() const {
    ByteWriter w;
    serialize_header(w, *this);
    Hash256 h;
    crypto_generichash(h.data(), 32, w.get_bytes().data(), w.get_bytes().size(), nullptr, 0);
    return h;
}

// Merkle Root calculation (Canonical Economic Commitment AUD-CRIT-04 / P0-04)
Hash256 Block::compute_merkle_root() const {
    std::vector<Hash256> leaves;

    // 1. Transacciones protegidas (ya contienen hash canónico de salidas, comisiones, anillo e imagen de clave)
    for (const auto& tx : txs) {
        leaves.push_back(tx.tx_hash);
    }

    // 2. Depósitos canónicos (compromiso total de montos, comisiones, tx_hash, recipient keys y outputs furtivos)
    for (size_t i = 0; i < deposits.size(); ++i) {
        const auto& dep = deposits[i];
        ByteWriter dw;
        dw.write_string(dep.tx_hash);
        dw.write_u64(dep.gross_usdt_deposited);
        dw.write_u64(dep.fee_to_pool);
        dw.write_u64(dep.net_shielded_tokens_minted);
        dw.write_array(dep.recipient_view_pub);
        dw.write_array(dep.recipient_spend_pub);
        if (i < deposit_outputs.size()) {
            dw.write_array(deposit_outputs[i].ephemeral_public_key);
            dw.write_array(deposit_outputs[i].destination_one_time);
            dw.write_u64(deposit_outputs[i].amount);
        }
        Hash256 dh;
        auto db = dw.take_bytes();
        crypto_generichash(dh.data(), 32, db.data(), db.size(), nullptr, 0);
        leaves.push_back(dh);
    }

    // 3. Retiros canónicos (compromiso total de montos quemados, fees, destino, orden, key image y prueba de quema)
    for (const auto& wdr : withdrawals) {
        ByteWriter ww;
        ww.write_string(wdr.order_id);
        ww.write_u64(wdr.gross_tokens_burned);
        ww.write_u64(wdr.fee_to_pool);
        ww.write_u64(wdr.net_usdt_to_tumble);
        ww.write_string(wdr.destination_address);
        ww.write_array(wdr.key_image);
        ww.write_array(wdr.burned_utxo_pubkey);
        ww.write_array(wdr.burn_signature_c0);
        ww.write_array(wdr.burn_signature_s);
        Hash256 wh;
        auto wb = ww.take_bytes();
        crypto_generichash(wh.data(), 32, wb.data(), wb.size(), nullptr, 0);
        leaves.push_back(wh);
    }

    // 4. Salidas de cambio de retiros (AUD-H0-04)
    for (const auto& out : withdrawal_outputs) {
        ByteWriter wow;
        wow.write_array(out.ephemeral_public_key);
        wow.write_array(out.destination_one_time);
        wow.write_u64(out.amount);
        Hash256 woh;
        auto wob = wow.take_bytes();
        crypto_generichash(woh.data(), 32, wob.data(), wob.size(), nullptr, 0);
        leaves.push_back(woh);
    }

    if (leaves.empty()) {
        Hash256 empty_root{};
        empty_root.fill(0);
        return empty_root;
    }

    std::vector<Hash256> current = leaves;
    while (current.size() > 1) {
        std::vector<Hash256> next;
        for (size_t i = 0; i < current.size(); i += 2) {
            Hash256 combined;
            uint8_t buf[64];
            std::memcpy(buf, current[i].data(), 32);
            if (i + 1 < current.size()) {
                std::memcpy(buf + 32, current[i + 1].data(), 32);
            } else {
                // Si es impar, duplicar el último hash para el árbol de Merkle
                std::memcpy(buf + 32, current[i].data(), 32);
            }
            crypto_generichash(combined.data(), 32, buf, 64, nullptr, 0);
            next.push_back(combined);
        }
        current = std::move(next);
    }

    return current[0];
}

std::vector<uint8_t> Block::serialize() const {
    ByteWriter w;
    serialize_header(w, header);

    // TXs
    w.write_u32(static_cast<uint32_t>(txs.size()));
    for (const auto& tx : txs) {
        serialize_tx(w, tx);
    }

    // Deposits
    w.write_u32(static_cast<uint32_t>(deposits.size()));
    for (const auto& dep : deposits) {
        serialize_deposit(w, dep);
    }

    // Withdrawals
    w.write_u32(static_cast<uint32_t>(withdrawals.size()));
    for (const auto& wdr : withdrawals) {
        serialize_withdrawal(w, wdr);
    }

    // Deposit outputs
    w.write_u32(static_cast<uint32_t>(deposit_outputs.size()));
    for (const auto& out : deposit_outputs) {
        serialize_output(w, out);
    }

    // Withdrawal change outputs (AUD-H0-04)
    w.write_u32(static_cast<uint32_t>(withdrawal_outputs.size()));
    for (const auto& out : withdrawal_outputs) {
        serialize_output(w, out);
    }

    return w.take_bytes();
}

Block Block::deserialize(const uint8_t* data, size_t len) {
    ByteReader r(data, len);
    Block block;
    block.header = deserialize_header(r);

    uint32_t tx_count = r.read_u32();
    if (tx_count > MAX_BLOCK_TRANSACTIONS) {
        throw std::runtime_error("Cantidad de transacciones excede el limite protocolario del bloque.");
    }
    block.txs.reserve(tx_count);
    for (uint32_t i = 0; i < tx_count; ++i) {
        block.txs.push_back(deserialize_tx(r));
    }

    uint32_t dep_count = r.read_u32();
    if (dep_count > MAX_BLOCK_DEPOSITS) {
        throw std::runtime_error("Cantidad de depositos excede el limite protocolario del bloque.");
    }
    block.deposits.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        block.deposits.push_back(deserialize_deposit(r));
    }

    uint32_t wdr_count = r.read_u32();
    if (wdr_count > MAX_BLOCK_WITHDRAWALS) {
        throw std::runtime_error("Cantidad de retiros excede el limite protocolario del bloque.");
    }
    block.withdrawals.reserve(wdr_count);
    for (uint32_t i = 0; i < wdr_count; ++i) {
        block.withdrawals.push_back(deserialize_withdrawal(r));
    }

    if (r.remaining() >= sizeof(uint32_t)) {
        uint32_t out_count = r.read_u32();
        if (out_count > MAX_BLOCK_OUTPUTS) {
            throw std::runtime_error("Cantidad de outputs de deposito excede el limite protocolario del bloque.");
        }
        block.deposit_outputs.reserve(out_count);
        for (uint32_t i = 0; i < out_count; ++i) {
            block.deposit_outputs.push_back(deserialize_output(r));
        }
    }

    if (r.remaining() >= sizeof(uint32_t)) {
        uint32_t out_count = r.read_u32();
        if (out_count > MAX_BLOCK_OUTPUTS) {
            throw std::runtime_error("Cantidad de outputs de retiro excede el limite protocolario del bloque.");
        }
        block.withdrawal_outputs.reserve(out_count);
        for (uint32_t i = 0; i < out_count; ++i) {
            block.withdrawal_outputs.push_back(deserialize_output(r));
        }
    }

    return block;
}

Block Block::create_genesis() {
    Block genesis;
    genesis.header.height = 0;
    genesis.header.prev_block_hash.fill(0);
    genesis.header.timestamp = 1773273600ULL; // Bloque Génesis Peg USDT Privado
    genesis.header.nonce = 0x47454E45534953ULL; // "GENESIS"
    genesis.header.merkle_root = genesis.compute_merkle_root();
    return genesis;
}

} // namespace crypto
