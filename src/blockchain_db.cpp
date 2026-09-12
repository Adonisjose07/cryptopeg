#include "blockchain_db.hpp"
#include <filesystem>
#include <stdexcept>
#include <iostream>
#include <cstring>

namespace crypto {

BlockchainDB::BlockchainDB() = default;

BlockchainDB::~BlockchainDB() {
    close();
}

void BlockchainDB::open(const std::string& path, size_t map_size_bytes) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (env_) {
        return; // Ya está abierto
    }

    db_path_ = path;
    std::filesystem::create_directories(path);

    int rc = mdb_env_create(&env_);
    if (rc != 0) {
        throw std::runtime_error("Fallo al crear entorno LMDB: " + std::string(mdb_strerror(rc)));
    }

    rc = mdb_env_set_maxdbs(env_, 10);
    if (rc != 0) {
        close();
        throw std::runtime_error("Fallo al configurar maxdbs en LMDB: " + std::string(mdb_strerror(rc)));
    }

    rc = mdb_env_set_mapsize(env_, map_size_bytes);
    if (rc != 0) {
        close();
        throw std::runtime_error("Fallo al configurar mapsize en LMDB: " + std::string(mdb_strerror(rc)));
    }

    rc = mdb_env_open(env_, path.c_str(), 0, 0664);
    if (rc != 0) {
        close();
        throw std::runtime_error("Fallo al abrir directorio LMDB (" + path + "): " + std::string(mdb_strerror(rc)));
    }

    // Inicializar / Abrir tablas (sub-bases de datos con nombre)
    MDB_txn* txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
        close();
        throw std::runtime_error("Fallo al iniciar transacción inicial de LMDB: " + std::string(mdb_strerror(rc)));
    }

    rc = mdb_dbi_open(txn, "blocks", MDB_CREATE, &dbi_blocks_);
    if (rc != 0) { mdb_txn_abort(txn); close(); throw std::runtime_error("Fallo al abrir tabla blocks: " + std::string(mdb_strerror(rc))); }

    rc = mdb_dbi_open(txn, "block_index", MDB_CREATE, &dbi_block_index_);
    if (rc != 0) { mdb_txn_abort(txn); close(); throw std::runtime_error("Fallo al abrir tabla block_index: " + std::string(mdb_strerror(rc))); }

    rc = mdb_dbi_open(txn, "key_images", MDB_CREATE, &dbi_key_images_);
    if (rc != 0) { mdb_txn_abort(txn); close(); throw std::runtime_error("Fallo al abrir tabla key_images: " + std::string(mdb_strerror(rc))); }

    rc = mdb_dbi_open(txn, "utxos", MDB_CREATE, &dbi_utxos_);
    if (rc != 0) { mdb_txn_abort(txn); close(); throw std::runtime_error("Fallo al abrir tabla utxos: " + std::string(mdb_strerror(rc))); }

    rc = mdb_dbi_open(txn, "metadata", MDB_CREATE, &dbi_metadata_);
    if (rc != 0) { mdb_txn_abort(txn); close(); throw std::runtime_error("Fallo al abrir tabla metadata: " + std::string(mdb_strerror(rc))); }

    rc = mdb_txn_commit(txn);
    if (rc != 0) {
        close();
        throw std::runtime_error("Fallo al confirmar tablas LMDB: " + std::string(mdb_strerror(rc)));
    }

    init_top_height();
}

void BlockchainDB::close() {
    if (env_) {
        mdb_env_close(env_);
        env_ = nullptr;
    }
}

void BlockchainDB::init_top_height() {
    top_height_ = 0;
    top_hash_.fill(0);

    if (!env_) return;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return;

    std::string meta_key = "top_height";
    MDB_val k, v;
    k.mv_size = meta_key.size();
    k.mv_data = const_cast<char*>(meta_key.data());

    rc = mdb_get(txn, dbi_metadata_, &k, &v);
    if (rc == 0 && v.mv_size == sizeof(uint64_t)) {
        uint64_t be_h = 0;
        std::memcpy(&be_h, v.mv_data, sizeof(uint64_t));
        top_height_ = from_big_endian_64(be_h);

        // Obtener el hash del bloque en top_height_
        uint64_t be_key = to_big_endian_64(top_height_);
        MDB_val bk, bv;
        bk.mv_size = sizeof(be_key);
        bk.mv_data = &be_key;
        if (mdb_get(txn, dbi_blocks_, &bk, &bv) == 0) {
            try {
                Block b = Block::deserialize(static_cast<const uint8_t*>(bv.mv_data), bv.mv_size);
                top_hash_ = b.hash();
            } catch (...) {}
        }
    }

    mdb_txn_abort(txn);
}

uint64_t BlockchainDB::get_top_height() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    return top_height_;
}

Hash256 BlockchainDB::get_top_block_hash() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    return top_hash_;
}

bool BlockchainDB::get_block_by_height(uint64_t height, Block& out_block) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return false;

    uint64_t be_h = to_big_endian_64(height);
    MDB_val k, v;
    k.mv_size = sizeof(be_h);
    k.mv_data = &be_h;

    rc = mdb_get(txn, dbi_blocks_, &k, &v);
    if (rc == 0) {
        try {
            out_block = Block::deserialize(static_cast<const uint8_t*>(v.mv_data), v.mv_size);
            mdb_txn_abort(txn);
            return true;
        } catch (...) {
            mdb_txn_abort(txn);
            return false;
        }
    }

    mdb_txn_abort(txn);
    return false;
}

bool BlockchainDB::get_block_by_hash(const Hash256& hash, Block& out_block) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return false;

    MDB_val k_hash, v_height;
    k_hash.mv_size = hash.size();
    k_hash.mv_data = const_cast<uint8_t*>(hash.data());

    rc = mdb_get(txn, dbi_block_index_, &k_hash, &v_height);
    if (rc != 0 || v_height.mv_size != sizeof(uint64_t)) {
        mdb_txn_abort(txn);
        return false;
    }

    uint64_t be_h = 0;
    std::memcpy(&be_h, v_height.mv_data, sizeof(uint64_t));

    MDB_val k_h, v_block;
    k_h.mv_size = sizeof(be_h);
    k_h.mv_data = &be_h;

    rc = mdb_get(txn, dbi_blocks_, &k_h, &v_block);
    if (rc == 0) {
        try {
            out_block = Block::deserialize(static_cast<const uint8_t*>(v_block.mv_data), v_block.mv_size);
            mdb_txn_abort(txn);
            return true;
        } catch (...) {}
    }

    mdb_txn_abort(txn);
    return false;
}

bool BlockchainDB::is_key_image_spent(const KeyImage& image) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return false;

    MDB_val k, v;
    k.mv_size = image.size();
    k.mv_data = const_cast<uint8_t*>(image.data());

    rc = mdb_get(txn, dbi_key_images_, &k, &v);
    mdb_txn_abort(txn);

    return (rc == 0);
}

std::vector<KeyImage> BlockchainDB::load_all_key_images() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<KeyImage> list;
    if (!env_) return list;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return list;

    MDB_cursor* cursor = nullptr;
    rc = mdb_cursor_open(txn, dbi_key_images_, &cursor);
    if (rc == 0) {
        MDB_val k, v;
        rc = mdb_cursor_get(cursor, &k, &v, MDB_FIRST);
        while (rc == 0) {
            if (k.mv_size == 32) {
                KeyImage ki;
                std::memcpy(ki.data(), k.mv_data, 32);
                list.push_back(ki);
            }
            rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
    }

    mdb_txn_abort(txn);
    return list;
}

std::vector<OneTimeOutput> BlockchainDB::load_all_utxos() const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    std::vector<OneTimeOutput> list;
    if (!env_) return list;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return list;

    MDB_cursor* cursor = nullptr;
    rc = mdb_cursor_open(txn, dbi_utxos_, &cursor);
    if (rc == 0) {
        MDB_val k, v;
        rc = mdb_cursor_get(cursor, &k, &v, MDB_FIRST);
        while (rc == 0) {
            try {
                ByteReader r(static_cast<const uint8_t*>(v.mv_data), v.mv_size);
                list.push_back(deserialize_output(r));
            } catch (...) {}
            rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
        }
        mdb_cursor_close(cursor);
    }

    mdb_txn_abort(txn);
    return list;
}

bool BlockchainDB::load_vault_state(Vault& vault) const {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) return false;

    std::string meta_vault_key = "vault_state";
    MDB_val k, v;
    k.mv_size = meta_vault_key.size();
    k.mv_data = const_cast<char*>(meta_vault_key.data());

    rc = mdb_get(txn, dbi_metadata_, &k, &v);
    if (rc == 0) {
        try {
            ByteReader r(static_cast<const uint8_t*>(v.mv_data), v.mv_size);
            Amount collateral = r.read_u64();
            Amount circulating = r.read_u64();
            Amount fee_pool = r.read_u64();
            vault.restore_state(collateral, circulating, fee_pool);
            mdb_txn_abort(txn);
            return true;
        } catch (...) {}
    }

    mdb_txn_abort(txn);
    return false;
}

bool BlockchainDB::save_vault_state(const Vault& vault) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) return false;

    try {
        std::string meta_vs = "vault_state";
        MDB_val k_mvs, v_mvs;
        k_mvs.mv_size = meta_vs.size();
        k_mvs.mv_data = const_cast<char*>(meta_vs.data());

        ByteWriter vw;
        vw.write_u64(vault.get_total_collateral());
        vw.write_u64(vault.get_circulating_shielded_supply());
        vw.write_u64(vault.get_fee_pool_reserve());
        auto vb = vw.take_bytes();
        v_mvs.mv_size = vb.size();
        v_mvs.mv_data = vb.data();

        rc = mdb_put(txn, dbi_metadata_, &k_mvs, &v_mvs, 0);
        if (rc != 0) {
            mdb_txn_abort(txn);
            return false;
        }

        rc = mdb_txn_commit(txn);
        return (rc == 0);
    } catch (...) {
        mdb_txn_abort(txn);
        return false;
    }
}

void BlockchainDB::commit_block(
    const Block& block,
    const Vault& vault,
    const std::vector<OneTimeOutput>& new_utxos,
    const std::vector<KeyImage>& spent_images
) {
    std::lock_guard<std::mutex> lock(db_mutex_);
    if (!env_) {
        throw std::runtime_error("No se puede hacer commit: entorno LMDB cerrado.");
    }

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
        throw std::runtime_error("Fallo al iniciar transacción de escritura LMDB: " + std::string(mdb_strerror(rc)));
    }

    try {
        uint64_t be_height = to_big_endian_64(block.header.height);

        // 1. Guardar bloque serializado
        MDB_val k_height, v_block;
        k_height.mv_size = sizeof(be_height);
        k_height.mv_data = &be_height;

        auto block_bytes = block.serialize();
        v_block.mv_size = block_bytes.size();
        v_block.mv_data = block_bytes.data();

        rc = mdb_put(txn, dbi_blocks_, &k_height, &v_block, 0);
        if (rc != 0) {
            throw std::runtime_error("Fallo al insertar bloque en dbi_blocks: " + std::string(mdb_strerror(rc)));
        }

        // 2. Indexar hash del bloque -> altura
        Hash256 b_hash = block.hash();
        MDB_val k_hash, v_height;
        k_hash.mv_size = b_hash.size();
        k_hash.mv_data = b_hash.data();
        v_height.mv_size = sizeof(be_height);
        v_height.mv_data = &be_height;

        rc = mdb_put(txn, dbi_block_index_, &k_hash, &v_height, 0);
        if (rc != 0) {
            throw std::runtime_error("Fallo al indexar hash de bloque: " + std::string(mdb_strerror(rc)));
        }

        // 3. Registrar imágenes de clave gastadas
        for (const auto& ki : spent_images) {
            MDB_val k_ki, v_ki_h;
            k_ki.mv_size = ki.size();
            k_ki.mv_data = const_cast<uint8_t*>(ki.data());
            v_ki_h.mv_size = sizeof(be_height);
            v_ki_h.mv_data = &be_height;

            rc = mdb_put(txn, dbi_key_images_, &k_ki, &v_ki_h, 0);
            if (rc != 0) {
                throw std::runtime_error("Fallo al guardar imagen de clave gastada: " + std::string(mdb_strerror(rc)));
            }
        }

        // 4. Guardar nuevos UTXOs
        for (const auto& utxo : new_utxos) {
            MDB_val k_utxo, v_utxo;
            k_utxo.mv_size = utxo.destination_one_time.size();
            k_utxo.mv_data = const_cast<uint8_t*>(utxo.destination_one_time.data());

            ByteWriter bw;
            serialize_output(bw, utxo);
            auto ub = bw.take_bytes();
            v_utxo.mv_size = ub.size();
            v_utxo.mv_data = ub.data();

            rc = mdb_put(txn, dbi_utxos_, &k_utxo, &v_utxo, 0);
            if (rc != 0) {
                throw std::runtime_error("Fallo al guardar UTXO en dbi_utxos: " + std::string(mdb_strerror(rc)));
            }
        }

        // 5. Actualizar metadata: "top_height" y "vault_state"
        std::string meta_th = "top_height";
        MDB_val k_mth, v_mth;
        k_mth.mv_size = meta_th.size();
        k_mth.mv_data = const_cast<char*>(meta_th.data());
        v_mth.mv_size = sizeof(be_height);
        v_mth.mv_data = &be_height;

        rc = mdb_put(txn, dbi_metadata_, &k_mth, &v_mth, 0);
        if (rc != 0) {
            throw std::runtime_error("Fallo al actualizar metadata top_height: " + std::string(mdb_strerror(rc)));
        }

        std::string meta_vs = "vault_state";
        MDB_val k_mvs, v_mvs;
        k_mvs.mv_size = meta_vs.size();
        k_mvs.mv_data = const_cast<char*>(meta_vs.data());

        ByteWriter vw;
        vw.write_u64(vault.get_total_collateral());
        vw.write_u64(vault.get_circulating_shielded_supply());
        vw.write_u64(vault.get_fee_pool_reserve());
        auto vb = vw.take_bytes();
        v_mvs.mv_size = vb.size();
        v_mvs.mv_data = vb.data();

        rc = mdb_put(txn, dbi_metadata_, &k_mvs, &v_mvs, 0);
        if (rc != 0) {
            throw std::runtime_error("Fallo al actualizar metadata vault_state: " + std::string(mdb_strerror(rc)));
        }

        // Commit atómico final
        rc = mdb_txn_commit(txn);
        if (rc != 0) {
            throw std::runtime_error("Fallo en commit atómico LMDB: " + std::string(mdb_strerror(rc)));
        }

        top_height_ = block.header.height;
        top_hash_ = b_hash;
    } catch (...) {
        mdb_txn_abort(txn);
        throw;
    }
}

} // namespace crypto
