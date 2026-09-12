#include "block.hpp"
#include <cstring>
#include <chrono>

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
    uint32_t resp_count = r.read_u32();
    sig.responses.reserve(resp_count);
    for (uint32_t i = 0; i < resp_count; ++i) {
        sig.responses.push_back(r.read_array<32>());
    }
    uint32_t pk_count = r.read_u32();
    sig.ring_pubkeys.reserve(pk_count);
    for (uint32_t i = 0; i < pk_count; ++i) {
        sig.ring_pubkeys.push_back(r.read_array<32>());
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
}

DepositReceipt deserialize_deposit(ByteReader& r) {
    DepositReceipt dep;
    dep.gross_usdt_deposited = r.read_u64();
    dep.fee_to_pool = r.read_u64();
    dep.net_shielded_tokens_minted = r.read_u64();
    dep.tx_hash = r.read_string();
    return dep;
}

// 5. WithdrawalReceipt
void serialize_withdrawal(ByteWriter& w, const WithdrawalReceipt& wdr) {
    w.write_u64(wdr.gross_tokens_burned);
    w.write_u64(wdr.fee_to_pool);
    w.write_u64(wdr.net_usdt_to_tumble);
    w.write_string(wdr.order_id);
}

WithdrawalReceipt deserialize_withdrawal(ByteReader& r) {
    WithdrawalReceipt wdr;
    wdr.gross_tokens_burned = r.read_u64();
    wdr.fee_to_pool = r.read_u64();
    wdr.net_usdt_to_tumble = r.read_u64();
    wdr.order_id = r.read_string();
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

// Merkle Root calculation
Hash256 Block::compute_merkle_root() const {
    std::vector<Hash256> leaves;

    // Tx hashes
    for (const auto& tx : txs) {
        leaves.push_back(tx.tx_hash);
    }

    // Deposit hashes
    for (const auto& dep : deposits) {
        Hash256 dh;
        crypto_generichash(
            dh.data(), 32,
            reinterpret_cast<const uint8_t*>(dep.tx_hash.data()),
            dep.tx_hash.size(),
            nullptr, 0
        );
        leaves.push_back(dh);
    }

    // Withdrawal hashes
    for (const auto& wdr : withdrawals) {
        Hash256 wh;
        crypto_generichash(
            wh.data(), 32,
            reinterpret_cast<const uint8_t*>(wdr.order_id.data()),
            wdr.order_id.size(),
            nullptr, 0
        );
        leaves.push_back(wh);
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
                // Si es impar, duplicar como en Bitcoin/Monero
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
