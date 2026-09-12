#pragma once

#include "types.hpp"
#include "vault.hpp"
#include "stealth.hpp"
#include "pedersen.hpp"
#include "ring_signature.hpp"
#include "tumbler.hpp"
#include "block.hpp"
#include "blockchain_db.hpp"
#include <vector>
#include <memory>
#include <mutex>
#include <string>
#include <functional>

namespace crypto {

class Node {
public:
    using BlockCallback = std::function<void(const Block&)>;

    explicit Node(
        uint32_t deposit_fee_bps = 50,
        uint32_t withdraw_fee_bps = 50,
        const std::string& db_path = "./data/lmdb"
    );

    // 1. Compra de cripto con USDT: acuñación 1:1 menos comisión que va al pool + Asentamiento en bloque LMDB
    DepositReceipt buy_shielded(Amount usdt_gross, const StealthAddress& recipient_address);

    // 2. Transferencia oculta estilo Monero (DKSAP + RingCT + Señuelos + Key Image) + Asentamiento en bloque LMDB
    ShieldedTransaction transfer_shielded(
        const StealthWallet& sender_wallet,
        const OneTimeOutput& input_utxo,
        const StealthAddress& recipient_address,
        Amount send_amount,
        Amount tx_fee = 0
    );

    // 3. Solicitud de retiro 1:1 menos comisión al pool + Mezclador de microtransacciones + Asentamiento en bloque LMDB
    TumblingPlan withdraw_shielded(
        const StealthWallet& burner_wallet,
        const OneTimeOutput& input_utxo,
        Amount tokens_to_withdraw,
        const std::string& destination_public_usdt,
        bool execute_immediately = true
    );

    // 4. Validación y asimilación de bloques provenientes de la red P2P
    bool apply_remote_block(const Block& block, std::string& error_msg);

    // Registro de callback cuando se mina/asienta un nuevo bloque localmente
    void set_on_block_mined(BlockCallback cb) {
        std::lock_guard<std::mutex> lock(node_mutex_);
        on_block_mined_ = std::move(cb);
    }

    // Consultas y auditoría de solvencia
    bool audit_system() const;
    void print_status() const;

    uint64_t get_blockchain_height() const;
    Hash256 get_top_block_hash() const;
    bool get_block(uint64_t height, Block& block) const;

    const Vault& get_vault() const { return vault_; }
    const std::vector<OneTimeOutput>& get_utxo_pool() const { return utxo_pool_; }
    const std::vector<ShieldedTransaction>& get_tx_history() const { return tx_history_; }
    const std::string& get_db_path() const { return db_path_; }

private:
    mutable std::mutex node_mutex_;
    std::string db_path_;
    BlockchainDB db_;
    Vault vault_;
    KeyImageLedger key_image_ledger_;
    TumblerEngine tumbler_;
    std::vector<OneTimeOutput> utxo_pool_; // Todas las salidas en cadena (para decoys)
    std::vector<ShieldedTransaction> tx_history_;
    BlockCallback on_block_mined_;

    // Seleccionar N-1 señuelos aleatorios del conjunto de outputs
    std::vector<Key256> select_decoys(size_t ring_size, const Key256& real_pubkey);

    // Inicialización y recuperación desde LMDB
    void init_or_recover_database();
};

} // namespace crypto
