#pragma once

#include "types.hpp"
#include "block.hpp"
#include "vault.hpp"
#include <lmdb.h>
#include <string>
#include <vector>
#include <mutex>
#include <memory>

namespace crypto {

class BlockchainDB {
public:
    BlockchainDB();
    ~BlockchainDB();

    BlockchainDB(const BlockchainDB&) = delete;
    BlockchainDB& operator=(const BlockchainDB&) = delete;

    void open(const std::string& path, size_t map_size_bytes = 10ULL * 1024 * 1024 * 1024);
    void close();
    bool is_open() const { return env_ != nullptr; }

    uint64_t get_top_height() const;
    Hash256 get_top_block_hash() const;

    bool get_block_by_height(uint64_t height, Block& out_block) const;
    bool get_block_by_hash(const Hash256& hash, Block& out_block) const;

    bool is_key_image_spent(const KeyImage& image) const;
    std::vector<KeyImage> load_all_key_images() const;
    std::vector<OneTimeOutput> load_all_utxos() const;

    bool load_vault_state(Vault& vault) const;
    bool save_vault_state(const Vault& vault);

    bool is_deposit_tx_processed(const std::string& tx_hash) const;

    void commit_block(
        const Block& block,
        const Vault& vault,
        const std::vector<OneTimeOutput>& new_utxos = {},
        const std::vector<KeyImage>& spent_images = {}
    );

    bool rollback_top_block();

private:
    void init_top_height();

    mutable std::mutex db_mutex_;
    MDB_env* env_{nullptr};
    MDB_dbi dbi_blocks_{0};
    MDB_dbi dbi_block_index_{0};
    MDB_dbi dbi_key_images_{0};
    MDB_dbi dbi_utxos_{0};
    MDB_dbi dbi_metadata_{0};
    MDB_dbi dbi_deposit_txs_{0};

    std::string db_path_;
    uint64_t top_height_{0};
    Hash256 top_hash_{};
};

} // namespace crypto
