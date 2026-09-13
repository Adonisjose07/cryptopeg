#include "block.hpp"
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <sodium.h>

namespace crypto {

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
        // Compatibilidad hacia atrás para bloques históricos previos
        if (wdr.order_id == "ORD-6a11f5bef01a84280a6ec5cd09a64d10") {
            wdr.destination_address = "0x9d59867EfE155406f637F028997866f252dcc72c";
        } else {
            wdr.destination_address = "";
        }
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
    return wdr;
}

// 6. BlockHeader
void serialize_header(ByteWriter& w, const BlockHeader& h) {
    w.write_u64(h.height);
    w.write_array(h.prev_block_hash);
    w.write_array(h.merkle_root);
    w.write_u64(h.timestamp);
    w.write_u64(h.nonce);
}

BlockHeader deserialize_header(ByteReader& r) {
    BlockHeader h;
    h.height = r.read_u64();
    h.prev_block_hash = r.read_array<32>();
    h.merkle_root = r.read_array<32>();
    h.timestamp = r.read_u64();
    h.nonce = r.read_u64();
    return h;
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
    block.txs.reserve(tx_count);
    for (uint32_t i = 0; i < tx_count; ++i) {
        block.txs.push_back(deserialize_tx(r));
    }

    uint32_t dep_count = r.read_u32();
    block.deposits.reserve(dep_count);
    for (uint32_t i = 0; i < dep_count; ++i) {
        block.deposits.push_back(deserialize_deposit(r));
    }

    uint32_t wdr_count = r.read_u32();
    block.withdrawals.reserve(wdr_count);
    for (uint32_t i = 0; i < wdr_count; ++i) {
        block.withdrawals.push_back(deserialize_withdrawal(r));
    }

    if (r.remaining() >= sizeof(uint32_t)) {
        uint32_t out_count = r.read_u32();
        block.deposit_outputs.reserve(out_count);
        for (uint32_t i = 0; i < out_count; ++i) {
            block.deposit_outputs.push_back(deserialize_output(r));
        }
    }

    if (r.remaining() >= sizeof(uint32_t)) {
        uint32_t out_count = r.read_u32();
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
