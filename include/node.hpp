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
    ~Node();

    // 1. Compra de cripto con USDT: acuñación 1:1 menos comisión que va al pool + Asentamiento en bloque LMDB
    DepositReceipt buy_shielded(
        Amount usdt_gross,
        const StealthAddress& recipient_address,
        const std::string& custom_tx_hash = "",
        uint64_t custom_timestamp = 0,
        const std::vector<std::array<uint8_t, 64>>& cosigner_secret_keys = {}
    );

    // 2. Transferencia confidencial RingCT (DKSAP + MLSAG + Señuelos + Key Image) + Asentamiento en bloque LMDB
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

    // 4. Cobro administrativo de comisiones acumuladas del pool hacia la tesorería
    ClaimReceipt claim_treasury_fees(Amount amount_to_claim, const std::string& destination_address);

    // 5. Validación y asimilación de bloques provenientes de la red P2P
    bool apply_remote_block(const Block& block, std::string& error_msg);

    // 6. Obtener señuelos aleatorios para composición no-custodial en cliente Wasm
    std::vector<Key256> get_random_decoys(size_t count, const Key256& exclude_pubkey, Amount target_amount = 0);

    // 7. Asimilación y minado de una transacción confidencial pre-firmada (Fase 3 No-Custodial)
    ShieldedTransaction submit_pre_signed_transaction(const ShieldedTransaction& tx);

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
    bool is_key_image_spent(const KeyImage& image) const;
    bool is_deposit_tx_processed(const std::string& tx_hash) const { return db_.is_deposit_tx_processed(tx_hash); }
    const BlockchainDB& get_db() const { return db_; }

    // Gestión de identidad de validador y oráculos autorizados (P0-03 y P1-01 Quórum)
    void set_validator_key(const uint8_t* secret_key_64, const Key256& pub_key_32);
    void add_authorized_validator(const Key256& pub_key_32);
    const Key256& get_validator_pubkey() const { return validator_pubkey_; }
    bool has_validator_key() const { return has_validator_key_; }
    const std::vector<Key256>& get_authorized_validators() const { return authorized_validators_; }
    uint32_t get_quorum_threshold() const { return validator_quorum_threshold_; }
    void set_quorum_threshold(uint32_t q) { validator_quorum_threshold_ = q; }
    bool is_authorized_validator(const Key256& pk) const {
        for (const auto& auth : authorized_validators_) {
            if (sodium_memcmp(auth.data(), pk.data(), 32) == 0) return true;
        }
        return false;
    }

    Vault get_vault() const {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return vault_;
    }
    std::vector<OneTimeOutput> get_utxo_pool() const {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return utxo_pool_;
    }
    size_t get_utxo_pool_size() const {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return utxo_pool_.size();
    }
    std::vector<ShieldedTransaction> get_tx_history() const {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return tx_history_;
    }
    std::string get_db_path() const {
        std::lock_guard<std::mutex> lock(node_mutex_);
        return db_path_;
    }

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

    // Claves y autorización de validador/oráculo (P0-03)
    Key256 validator_pubkey_{};
    std::array<uint8_t, 64> validator_secret_key_{};
    bool has_validator_key_{false};
    std::vector<Key256> authorized_validators_;
    uint32_t validator_quorum_threshold_{1};

    // Seleccionar N-1 señuelos aleatorios del conjunto de outputs con homogeneidad de denominación (AUD-H0-P0-02)
    std::vector<Key256> select_decoys(size_t ring_size, const Key256& real_pubkey, Amount target_amount = 0);

    // Inicialización y recuperación desde LMDB
    void init_or_recover_database();
    void recover_state_from_db();
};

} // namespace crypto
