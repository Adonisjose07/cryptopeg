#pragma once

#include "types.hpp"
#include "stealth.hpp"
#include "ring_signature.hpp"
#include "vault.hpp"
#include "serialization.hpp"
#include <vector>
#include <string>

namespace crypto {

struct ShieldedTransaction {
    Hash256 tx_hash;
    RingSignature ring_sig;
    std::vector<OneTimeOutput> outputs;
    Amount public_fee{0};
    std::string timestamp;
};

// Serialización de componentes
void serialize_output(ByteWriter& w, const OneTimeOutput& out);
OneTimeOutput deserialize_output(ByteReader& r);

void serialize_ring_sig(ByteWriter& w, const RingSignature& sig);
RingSignature deserialize_ring_sig(ByteReader& r);

void serialize_tx(ByteWriter& w, const ShieldedTransaction& tx);
ShieldedTransaction deserialize_tx(ByteReader& r);

void serialize_deposit(ByteWriter& w, const DepositReceipt& dep);
DepositReceipt deserialize_deposit(ByteReader& r);

void serialize_withdrawal(ByteWriter& w, const WithdrawalReceipt& wdr);
WithdrawalReceipt deserialize_withdrawal(ByteReader& r);

struct BlockHeader {
    uint64_t height{0};
    Hash256 prev_block_hash{};
    Hash256 merkle_root{};
    uint64_t timestamp{0};
    uint64_t nonce{0};

    Hash256 hash() const;
};

void serialize_header(ByteWriter& w, const BlockHeader& h);
BlockHeader deserialize_header(ByteReader& r);

struct Block {
    BlockHeader header;
    std::vector<ShieldedTransaction> txs;
    std::vector<DepositReceipt> deposits;
    std::vector<WithdrawalReceipt> withdrawals;
    std::vector<OneTimeOutput> deposit_outputs;

    Hash256 compute_merkle_root() const;
    Hash256 hash() const { return header.hash(); }

    std::vector<uint8_t> serialize() const;
    static Block deserialize(const uint8_t* data, size_t len);

    static Block create_genesis();
};

} // namespace crypto
