#include "node.hpp"
#include <iostream>
#include <random>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>

namespace crypto {

Node::Node(uint32_t deposit_fee_bps, uint32_t withdraw_fee_bps, const std::string& db_path)
    : db_path_(db_path),
      vault_(deposit_fee_bps, withdraw_fee_bps),
      tumbler_(5, 10, 2, 3, 100) {
    if (sodium_init() < 0) {
        throw std::runtime_error("Fallo al inicializar libsodium.");
    }

    // Inicializar clave de validador local (P0-03)
    const char* env_val_priv = std::getenv("VALIDATOR_PRIVATE_KEY");
    if (env_val_priv) {
        try {
            auto priv_bytes = from_hex(env_val_priv);
            if (priv_bytes.size() == 32) {
                crypto_sign_seed_keypair(
                    validator_pubkey_.data(),
                    validator_secret_key_.data(),
                    priv_bytes.data()
                );
                has_validator_key_ = true;
            } else if (priv_bytes.size() == 64) {
                std::memcpy(validator_secret_key_.data(), priv_bytes.data(), 64);
                std::memcpy(validator_pubkey_.data(), priv_bytes.data() + 32, 32);
                has_validator_key_ = true;
            }
            sodium_memzero(priv_bytes.data(), priv_bytes.size());
        } catch (...) {}
    }
    if (!has_validator_key_) {
        crypto_sign_keypair(validator_pubkey_.data(), validator_secret_key_.data());
        has_validator_key_ = true;
    }

    // Cargar validadores autorizados desde entorno si están configurados
    const char* env_auth = std::getenv("AUTHORIZED_VALIDATOR_KEYS");
    if (env_auth) {
        std::istringstream iss(env_auth);
        std::string pk_hex;
        while (std::getline(iss, pk_hex, ',')) {
            if (!pk_hex.empty()) {
                try {
                    auto b = from_hex(pk_hex);
                    if (b.size() == 32) {
                        Key256 k;
                        std::memcpy(k.data(), b.data(), 32);
                        authorized_validators_.push_back(k);
                    }
                } catch (...) {}
            }
        }
    }

    init_or_recover_database();
}

Node::~Node() {
    sodium_memzero(validator_secret_key_.data(), validator_secret_key_.size());
}

void Node::set_validator_key(const uint8_t* secret_key_64, const Key256& pub_key_32) {
    std::lock_guard<std::mutex> lock(node_mutex_);
    std::memcpy(validator_secret_key_.data(), secret_key_64, 64);
    validator_pubkey_ = pub_key_32;
    has_validator_key_ = true;
}

void Node::add_authorized_validator(const Key256& pub_key_32) {
    std::lock_guard<std::mutex> lock(node_mutex_);
    for (const auto& k : authorized_validators_) {
        if (sodium_memcmp(k.data(), pub_key_32.data(), 32) == 0) return;
    }
    authorized_validators_.push_back(pub_key_32);
}

void Node::recover_state_from_db() {
    // Presupone que node_mutex_ ya está adquirido por el llamador
    db_.load_vault_state(vault_);
    utxo_pool_ = db_.load_all_utxos();

    key_image_ledger_.clear();
    auto key_images = db_.load_all_key_images();
    for (const auto& ki : key_images) {
        key_image_ledger_.restore_key_image(ki);
    }

    // Reconstruir historial de transacciones en memoria
    tx_history_.clear();
    for (uint64_t h = 1; h <= db_.get_top_height(); ++h) {
        Block blk;
        if (db_.get_block_by_height(h, blk)) {
            for (const auto& tx : blk.txs) {
                tx_history_.push_back(tx);
            }
        }
    }
}

void Node::init_or_recover_database() {
    std::lock_guard<std::mutex> lock(node_mutex_);
    db_.open(db_path_);

    uint64_t height = db_.get_top_height();
    Block b0;
    bool has_b0 = db_.get_block_by_height(0, b0);

    if (height == 0 && !has_b0) {
        // Inicializar con Bloque Génesis
        Block genesis = Block::create_genesis();
        db_.commit_block(genesis, vault_, {}, {});
    } else {
        recover_state_from_db();
    }
}

DepositReceipt Node::buy_shielded(
    Amount usdt_gross,
    const StealthAddress& recipient_address,
    const std::string& custom_tx_hash,
    uint64_t custom_timestamp,
    const std::vector<std::array<uint8_t, 64>>& cosigner_secret_keys
) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // Guardar estado contable previo de vault_ en caso de que falle el quórum
    Amount prev_col = vault_.get_total_collateral();
    Amount prev_circ = vault_.get_circulating_shielded_supply();
    Amount prev_fee = vault_.get_fee_pool_reserve();

    // 1. Asentar el depósito en la bóveda
    DepositReceipt receipt = vault_.deposit(usdt_gross, custom_tx_hash);
    receipt.recipient_view_pub = recipient_address.view_public_key;
    receipt.recipient_spend_pub = recipient_address.spend_public_key;

    // 2. Generar el output furtivo (one-time stealth output) para el receptor.
    // Se utiliza siempre el hash de la transacción (Arbitrum Sepolia custom_tx_hash o el hash de recibo)
    // como semilla determinista para garantizar consenso absoluto entre oráculos y evitar bifurcaciones.
    Hash256 seed;
    crypto_generichash(
        seed.data(), 32,
        reinterpret_cast<const uint8_t*>(receipt.tx_hash.data()),
        receipt.tx_hash.size(),
        nullptr, 0
    );

    OneTimeOutput utxo = StealthProtocol::create_one_time_output(
        recipient_address,
        receipt.net_shielded_tokens_minted,
        &seed
    );

    utxo_pool_.push_back(utxo);

    // 3. Empaquetar y asentar en Bloque LMDB
    uint64_t next_height = db_.get_top_height() + 1;
    Block block;
    block.header.height = next_height;
    block.header.prev_block_hash = db_.get_top_block_hash();
    if (custom_timestamp > 0) {
        block.header.timestamp = custom_timestamp;
    } else {
        block.header.timestamp = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );
    }
    block.deposits.push_back(receipt);
    block.deposit_outputs.push_back(utxo);
    block.header.merkle_root = block.compute_merkle_root();

    if (has_validator_key_) {
        block.header.sign(validator_secret_key_.data(), validator_pubkey_);
    }
    for (const auto& c_sk : cosigner_secret_keys) {
        Key256 c_pk;
        crypto_sign_ed25519_sk_to_pk(c_pk.data(), c_sk.data());
        Signature64 c_sig;
        crypto_sign_detached(c_sig.data(), nullptr, block.header.signing_hash().data(), 32, c_sk.data());
        block.header.add_quorum_signature(c_pk, c_sig);
    }

    // Verificar quórum M-de-N antes de hacer commit local si la lista de oráculos autorizados no está vacía (P1-01)
    if (!authorized_validators_.empty()) {
        size_t valid_quorum = block.header.verify_quorum(authorized_validators_);
        uint32_t required_quorum = std::max(1U, validator_quorum_threshold_);
        if (valid_quorum < required_quorum) {
            utxo_pool_.pop_back();
            vault_.restore_state(prev_col, prev_circ, prev_fee);
            throw std::runtime_error("Quórum insuficiente de validadores para minar depósito: se requieren " +
                                     std::to_string(required_quorum) + " firmas válidas, se obtuvieron " +
                                     std::to_string(valid_quorum) + ".");
        }
    }

    db_.commit_block(block, vault_, {utxo}, {});

    if (on_block_mined_) {
        on_block_mined_(block);
    }

    return receipt;
}

std::vector<Key256> Node::select_decoys(size_t ring_size, const Key256& real_pubkey, Amount target_amount) {
    std::vector<Key256> candidates;
    for (const auto& out : utxo_pool_) {
        if (target_amount > 0 && out.amount != target_amount) {
            continue; // Homogeneidad estricta de denominación (AUD-H0-P0-02)
        }
        if (sodium_memcmp(out.destination_one_time.data(), real_pubkey.data(), 32) != 0) {
            bool already = false;
            for (const auto& c : candidates) {
                if (sodium_memcmp(c.data(), out.destination_one_time.data(), 32) == 0) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                candidates.push_back(out.destination_one_time);
            }
        }
    }

    // Barajado criptográficamente seguro (Fisher-Yates con CSPRNG de libsodium AUD-HIGH-03)
    if (!candidates.empty()) {
        for (size_t i = candidates.size() - 1; i > 0; --i) {
            size_t j = randombytes_uniform(static_cast<uint32_t>(i + 1));
            std::swap(candidates[i], candidates[j]);
        }
    }

    std::vector<Key256> selected;
    // Remediación P1: NUNCA generar señuelos sintéticos fuera de cadena.
    // Solo seleccionar salidas históricas reales presentes en el libro mayor.
    size_t target_decoys = (ring_size > 0) ? (ring_size - 1) : 0;
    size_t num_to_take = std::min(target_decoys, candidates.size());
    for (size_t i = 0; i < num_to_take; ++i) {
        selected.push_back(candidates[i]);
    }

    return selected;
}

ShieldedTransaction Node::transfer_shielded(
    const StealthWallet& sender_wallet,
    const OneTimeOutput& input_utxo,
    const StealthAddress& recipient_address,
    Amount send_amount,
    Amount tx_fee
) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // 1. Verificar que el emisor realmente es dueño del output
    if (!StealthProtocol::scan_output(sender_wallet, input_utxo)) {
        throw std::runtime_error("El emisor no es el propietario del output seleccionado.");
    }

    Amount total_needed = 0;
    if (!safe_add_amount(send_amount, tx_fee, total_needed)) {
        throw std::runtime_error("Desbordamiento aritmético en monto a transferir más comisión (P0-02).");
    }
    if (input_utxo.amount < total_needed) {
        throw std::runtime_error("Fondos insuficientes en el output para cubrir monto + comisión.");
    }

    // 2. Derivar la clave privada de un solo uso x para este output
    Key256 one_time_priv = StealthProtocol::derive_one_time_private_key(sender_wallet, input_utxo);

    // 3. Calcular la imagen de clave I = x * H_p(P) y verificar doble-gasto
    KeyImage img = RingSignatureEngine::compute_key_image(one_time_priv, input_utxo.destination_one_time);
    if (key_image_ledger_.is_spent(img) || db_.is_key_image_spent(img)) {
        secure_wipe(one_time_priv);
        throw std::runtime_error("Intento de DOBLE GASTO detectado: la imagen de clave ya fue utilizada.");
    }

    // 4. Construir el anillo de señuelos (tamaño típico: 5 a 11 con denominación homogénea AUD-H0-P0-02)
    size_t ring_size = 5;
    auto decoys = select_decoys(ring_size, input_utxo.destination_one_time, input_utxo.amount);

    // Insertar la clave real en una posición aleatoria del anillo usando CSPRNG (AUD-HIGH-03)
    size_t actual_ring_size = decoys.size() + 1;
    size_t real_index = randombytes_uniform(static_cast<uint32_t>(actual_ring_size));

    std::vector<Key256> ring = decoys;
    ring.insert(ring.begin() + real_index, input_utxo.destination_one_time);

    // 5. Crear los nuevos outputs (Receptor + Cambio al emisor)
    std::vector<OneTimeOutput> new_outputs;
    OneTimeOutput recipient_out = StealthProtocol::create_one_time_output(recipient_address, send_amount);
    new_outputs.push_back(recipient_out);

    // Output de cambio (change) de regreso al emisor si sobró saldo
    Amount change = input_utxo.amount - send_amount - tx_fee;
    if (change > 0) {
        OneTimeOutput change_out = StealthProtocol::create_one_time_output(
            sender_wallet.get_public_address(),
            change
        );
        new_outputs.push_back(change_out);
    }

    // 6. Generar hash canónico completo de la transacción (AUD-CRIT-03)
    Hash256 tx_hash = RingSignatureEngine::compute_canonical_tx_hash(
        new_outputs,
        tx_fee,
        ring,
        img
    );

    // 7. Firmar con el anillo (Ring Signature MLSAG)
    RingSignature sig = RingSignatureEngine::sign(tx_hash, ring, real_index, one_time_priv);

    // 8. Verificar la firma del anillo inmediatamente antes de asentar
    if (!RingSignatureEngine::verify(tx_hash, sig)) {
        secure_wipe(one_time_priv);
        throw std::runtime_error("Fallo crítico en validación matemática de firma de anillo.");
    }

    // 9. Registrar la imagen de clave para impedir que este output se vuelva a gastar jamás
    key_image_ledger_.register_key_image(sig.key_image);

    // 10. Actualizar libro mayor con las nuevas salidas
    for (const auto& out : new_outputs) {
        utxo_pool_.push_back(out);
    }

    secure_wipe(one_time_priv);

    ShieldedTransaction tx;
    tx.tx_hash = tx_hash;
    tx.ring_sig = sig;
    tx.outputs = new_outputs;
    tx.public_fee = tx_fee;
    tx.timestamp = "2026-09-11 20:00:00 UTC";

    tx_history_.push_back(tx);

    // 11. Empaquetar y asentar en Bloque LMDB atómico
    uint64_t next_height = db_.get_top_height() + 1;
    Block block;
    block.header.height = next_height;
    block.header.prev_block_hash = db_.get_top_block_hash();
    block.header.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    block.txs.push_back(tx);
    block.header.merkle_root = block.compute_merkle_root();

    if (has_validator_key_) {
        block.header.sign(validator_secret_key_.data(), validator_pubkey_);
    }

    db_.commit_block(block, vault_, new_outputs, {sig.key_image});

    if (on_block_mined_) {
        on_block_mined_(block);
    }

    return tx;
}

TumblingPlan Node::withdraw_shielded(
    const StealthWallet& burner_wallet,
    const OneTimeOutput& input_utxo,
    Amount tokens_to_withdraw,
    const std::string& destination_public_usdt,
    bool execute_immediately
) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // 1. Validar propiedad del output
    if (!StealthProtocol::scan_output(burner_wallet, input_utxo)) {
        throw std::runtime_error("La billetera no es dueña del output a retirar.");
    }

    if (input_utxo.amount < tokens_to_withdraw) {
        throw std::runtime_error("Monto en output insuficiente para el retiro solicitado.");
    }

    // 2. Derivar clave de un solo uso y marcar imagen de clave como gastada
    Key256 one_time_priv = StealthProtocol::derive_one_time_private_key(burner_wallet, input_utxo);
    KeyImage img = RingSignatureEngine::compute_key_image(one_time_priv, input_utxo.destination_one_time);

    if (key_image_ledger_.is_spent(img) || db_.is_key_image_spent(img)) {
        secure_wipe(one_time_priv);
        throw std::runtime_error("Intento de retiro con output ya gastado.");
    }

    // 3. Procesar quema y comisión en la bóveda
    WithdrawalReceipt receipt = vault_.request_withdrawal(tokens_to_withdraw);
    receipt.destination_address = destination_public_usdt;
    receipt.key_image = img;
    receipt.burned_utxo_pubkey = input_utxo.destination_one_time;

    // 4. Si hubo cambio, re-emitir output privado para el usuario y ligarlo criptográficamente (Auditoría v3 - P1-02)
    std::vector<OneTimeOutput> new_outs;
    Key256 change_pubkey{};
    Amount change = input_utxo.amount - tokens_to_withdraw;
    if (change > 0) {
        OneTimeOutput change_out = StealthProtocol::create_one_time_output(
            burner_wallet.get_public_address(),
            change
        );
        change_pubkey = change_out.destination_one_time;
        utxo_pool_.push_back(change_out);
        new_outs.push_back(change_out);
    }
    receipt.change_output_pubkey = change_pubkey;

    // Generar prueba criptográfica de quema DLEQ ligando destino de cambio (Auditoría v3 - P1-02)
    Hash256 burn_msg = RingSignatureEngine::compute_burn_message_hash(
        receipt.order_id,
        receipt.gross_tokens_burned,
        receipt.destination_address,
        receipt.change_output_pubkey
    );
    RingSignatureEngine::sign_burn_proof(
        burn_msg,
        input_utxo.destination_one_time,
        one_time_priv,
        receipt.key_image,
        receipt.burn_signature_c0,
        receipt.burn_signature_s
    );

    // Registrar imagen de clave
    key_image_ledger_.register_key_image(img);
    secure_wipe(one_time_priv);

    // 5. Empaquetar y asentar en Bloque LMDB atómico
    uint64_t next_height = db_.get_top_height() + 1;
    Block block;
    block.header.height = next_height;
    block.header.prev_block_hash = db_.get_top_block_hash();
    block.header.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    block.withdrawals.push_back(receipt);
    for (const auto& co : new_outs) {
        block.withdrawal_outputs.push_back(co);
    }
    block.header.merkle_root = block.compute_merkle_root();

    if (has_validator_key_) {
        block.header.sign(validator_secret_key_.data(), validator_pubkey_);
    }

    db_.commit_block(block, vault_, new_outs, {img});

    if (on_block_mined_) {
        on_block_mined_(block);
    }

    // 6. Planificar el mezclador (Tumbler) en microtransacciones
    TumblingPlan plan = tumbler_.plan_withdrawal_tumbling(
        receipt.order_id,
        receipt.gross_tokens_burned,
        receipt.fee_to_pool,
        receipt.net_usdt_to_tumble,
        destination_public_usdt
    );

    // 7. Ejecutar si se solicitó inmediato
    if (execute_immediately) {
        tumbler_.execute_tumbling_plan(plan, true);
    }

    return plan;
}

ClaimReceipt Node::claim_treasury_fees(Amount amount_to_claim, const std::string& destination_address) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // 1. Deducir comisiones de la reserva de la bóveda
    ClaimReceipt receipt = vault_.claim_fees(amount_to_claim, destination_address);

    // 2. Persistir el nuevo estado de la bóveda en la base de datos LMDB
    db_.save_vault_state(vault_);

    std::cout << "[NODE] Comisiones de tesorería reclamadas exitosamente: " 
              << format_usdt(receipt.amount_claimed) 
              << " hacia " << receipt.destination_address << "\n";

    return receipt;
}

bool Node::audit_system() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return vault_.audit_solvency();
}

uint64_t Node::get_blockchain_height() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return db_.get_top_height();
}

Hash256 Node::get_top_block_hash() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return db_.get_top_block_hash();
}

bool Node::get_block(uint64_t height, Block& block) const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return db_.get_block_by_height(height, block);
}

bool Node::is_key_image_spent(const KeyImage& image) const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return key_image_ledger_.is_spent(image) || db_.is_key_image_spent(image);
}

bool Node::apply_remote_block(const Block& block, std::string& error_msg) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    uint64_t current_height = db_.get_top_height();

    // Regla determinista de Fork-Choice y resolución canónica de bifurcaciones (P1-05):
    bool is_reorg = false;
    Block local_top;
    if (block.header.height == current_height && current_height > 0) {
        if (!db_.get_block_by_height(current_height, local_top)) {
            error_msg = "Error interno: no se pudo recuperar el bloque local en la punta.";
            return false;
        }

        // Si es exactamente el mismo bloque, ya está procesado y asentado
        if (std::memcmp(block.header.merkle_root.data(), local_top.header.merkle_root.data(), 32) == 0 &&
            std::memcmp(block.header.prev_block_hash.data(), local_top.header.prev_block_hash.data(), 32) == 0) {
            return true;
        }

        // Ambos bloques competidores deben descender del mismo bloque previo
        if (std::memcmp(block.header.prev_block_hash.data(), local_top.header.prev_block_hash.data(), 32) != 0) {
            error_msg = "Bifurcación no soportada: bloques competidores en altura " + std::to_string(current_height) +
                        " no comparten el mismo ancestro inmediato.";
            return false;
        }

        size_t remote_quorum = block.header.verify_quorum(authorized_validators_);
        size_t local_quorum = local_top.header.verify_quorum(authorized_validators_);

        bool remote_preferred = false;
        if (remote_quorum > local_quorum) {
            remote_preferred = true;
        } else if (remote_quorum == local_quorum) {
            // Desempate lexicográfico determinista por hash de bloque
            if (to_hex(block.hash()) < to_hex(local_top.hash())) {
                remote_preferred = true;
            }
        }

        if (!remote_preferred) {
            error_msg = "Bloque competidor rechazado por Fork-Choice: el bloque local posee mayor peso de quórum o prioridad léxica.";
            return false;
        }

        // Ejecutar reorg atómico en LMDB: rollback del bloque local
        if (!db_.rollback_top_block()) {
            error_msg = "Fallo al ejecutar rollback del bloque local para resolución Fork-Choice.";
            return false;
        }

        recover_state_from_db();
        current_height = db_.get_top_height();
        is_reorg = true;
    }

    bool success = false;
    struct ReorgGuard {
        bool& is_reorg;
        bool& success;
        Block& local_top;
        BlockchainDB& db;
        Vault& vault;
        Node& node;
        ~ReorgGuard() {
            if (is_reorg && !success) {
                // Restaurar atómicamente el bloque previo con todas sus imágenes de clave y UTXOs (P1-05)
                std::vector<KeyImage> spent_images;
                for (const auto& tx : local_top.txs) {
                    spent_images.push_back(tx.ring_sig.key_image);
                }
                for (const auto& wdr : local_top.withdrawals) {
                    spent_images.push_back(wdr.key_image);
                }

                std::vector<OneTimeOutput> new_utxos;
                for (const auto& out : local_top.deposit_outputs) {
                    new_utxos.push_back(out);
                }
                for (const auto& tx : local_top.txs) {
                    for (const auto& out : tx.outputs) {
                        new_utxos.push_back(out);
                    }
                }
                for (const auto& out : local_top.withdrawal_outputs) {
                    new_utxos.push_back(out);
                }

                db.commit_block(local_top, vault, new_utxos, spent_images);
            }
            if (!success) {
                // Restaurar deterministamente el estado en memoria desde disco ante cualquier fallo (V4-09)
                node.recover_state_from_db();
            }
        }
    } reorg_guard{is_reorg, success, local_top, db_, vault_, *this};

    auto fail = [&](const std::string& msg) -> bool {
        error_msg = msg;
        return false;
    };

    if (block.header.height != current_height + 1) {
        return fail("Altura de bloque inválida: se esperaba " + std::to_string(current_height + 1) +
                    ", recibido " + std::to_string(block.header.height));
    }

    Hash256 current_top_hash = db_.get_top_block_hash();
    if (std::memcmp(block.header.prev_block_hash.data(), current_top_hash.data(), 32) != 0) {
        return fail("Hash de bloque previo no coincide con la punta local de la cadena.");
    }

    Hash256 calculated_root = block.compute_merkle_root();
    if (std::memcmp(block.header.merkle_root.data(), calculated_root.data(), 32) != 0) {
        return fail("Raiz de Merkle invalida en cabecera del bloque.");
    }

    // 0. Autenticación de depósitos y consenso federado P2P (AUD-H0-P0-03 & Auditoría v3: P0-02 Fail-closed y P1-01 Quórum M-de-N)
    if (!block.deposits.empty()) {
        // En modo fail-closed, no se aceptan depósitos si la lista de oráculos autorizados está vacía (P0-02)
        if (authorized_validators_.empty()) {
            error_msg = "Bloque remoto rechazado: la lista de validadores autorizados está vacía (modo fail-closed requerido para depósitos P0-02).";
            return false;
        }

        // Verificar quórum M-de-N de firmas autorizadas (P1-01)
        size_t valid_quorum = block.header.verify_quorum(authorized_validators_);
        uint32_t required_quorum = std::max(1U, validator_quorum_threshold_);
        if (valid_quorum < required_quorum) {
            error_msg = "Bloque remoto rechazado: quórum insuficiente de oráculos autorizados (P1-01). Se requieren " +
                        std::to_string(required_quorum) + " firmas válidas, pero se verificaron " +
                        std::to_string(valid_quorum);
            return false;
        }
    } else if (!authorized_validators_.empty()) {
        if (!block.header.verify_signature()) {
            error_msg = "Bloque remoto rechazado: firma de validador/oráculo inválida o ausente en el encabezado (P0-03).";
            return false;
        }
        bool authorized = false;
        for (const auto& auth_pk : authorized_validators_) {
            if (sodium_memcmp(auth_pk.data(), block.header.validator_pubkey.data(), 32) == 0) {
                authorized = true;
                break;
            }
        }
        if (!authorized) {
            error_msg = "Bloque remoto rechazado: la clave del validador firmante no está en el conjunto autorizado de oráculos (P0-03).";
            return false;
        }
    }

    Vault temp_vault(vault_.get_deposit_fee_bps(), vault_.get_withdraw_fee_bps());
    temp_vault.restore_state(
        vault_.get_total_collateral(),
        vault_.get_circulating_shielded_supply(),
        vault_.get_fee_pool_reserve()
    );

    // 1. Validar depósitos (Hito 0: AUD-CRIT-01 & AUD-CRIT-02 / P0-01)
    if (block.deposits.size() != block.deposit_outputs.size()) {
        error_msg = "Desajuste entre cantidad de recibos de deposito y salidas generadas.";
        return false;
    }

    std::vector<std::string> block_deposit_txs;
    for (size_t i = 0; i < block.deposits.size(); ++i) {
        const auto& dep = block.deposits[i];
        const auto& out = block.deposit_outputs[i];

        if (dep.tx_hash.empty()) {
            error_msg = "Deposito rechazado: tx_hash vacio.";
            return false;
        }

        // 1.1 Idempotencia: Verificar que no haya sido procesado en LMDB
        if (db_.is_deposit_tx_processed(dep.tx_hash)) {
            error_msg = "Intento de reproduccion (replay): deposito ya procesado en ledger: " + dep.tx_hash;
            return false;
        }

        // 1.2 Unicidad intra-bloque
        for (const auto& existing_tx : block_deposit_txs) {
            if (existing_tx == dep.tx_hash) {
                error_msg = "Deposito duplicado dentro del mismo bloque: " + dep.tx_hash;
                return false;
            }
        }
        block_deposit_txs.push_back(dep.tx_hash);

        // 1.3 Conservación económica y comisiones exactas
        if (dep.gross_usdt_deposited == 0) {
            error_msg = "Deposito invalido: monto bruto debe ser mayor a cero.";
            return false;
        }
        Amount expected_fee = (dep.gross_usdt_deposited * vault_.get_deposit_fee_bps()) / 10000;
        Amount expected_net = dep.gross_usdt_deposited - expected_fee;
        if (dep.fee_to_pool != expected_fee || dep.net_shielded_tokens_minted != expected_net) {
            error_msg = "Comision o acuniacion neta de deposito no coincide con los parametros de la boveda.";
            return false;
        }
        if (out.amount != expected_net) {
            error_msg = "El monto de la salida de deposito no coincide con el valor neto acuniado.";
            return false;
        }

        // 1.4 Verificación Criptográfica de Derivación Determinista (Multi-Oráculo)
        // seed = H(dep.tx_hash), r = scalar_reduce(H_64(seed)), R_expected = r*G
        Hash256 seed;
        crypto_generichash(
            seed.data(), 32,
            reinterpret_cast<const uint8_t*>(dep.tx_hash.data()),
            dep.tx_hash.size(),
            nullptr, 0
        );
        uint8_t hash_r[64];
        crypto_generichash(hash_r, 64, seed.data(), 32, nullptr, 0);
        Key256 expected_r;
        crypto_core_ed25519_scalar_reduce(expected_r.data(), hash_r);
        Key256 expected_R;
        if (crypto_scalarmult_ed25519_base_noclamp(expected_R.data(), expected_r.data()) != 0) {
            sodium_memzero(expected_r.data(), sizeof(expected_r));
            sodium_memzero(hash_r, sizeof(hash_r));
            error_msg = "Error al derivar clave publica efimera esperada.";
            return false;
        }

        if (sodium_memcmp(out.ephemeral_public_key.data(), expected_R.data(), 32) != 0) {
            sodium_memzero(expected_r.data(), sizeof(expected_r));
            sodium_memzero(hash_r, sizeof(hash_r));
            error_msg = "Prueba de derivacion determinista fallida: ephemeral_pubkey no coincide con la semilla de Arbitrum tx_hash.";
            return false;
        }

        // 1.5 Blindaje Criptográfico contra Secuestro de Depósitos (AUD-H0-P1-01)
        // Reconstrucción algebraica: rB = r * B, s = scalar_reduce(H_64(rB)), P_expected = s*G + A
        bool has_recipient_keys = false;
        for (auto b : dep.recipient_view_pub) { if (b != 0) { has_recipient_keys = true; break; } }
        if (!has_recipient_keys) {
            sodium_memzero(expected_r.data(), sizeof(expected_r));
            sodium_memzero(hash_r, sizeof(hash_r));
            error_msg = "Rechazado: recibo de deposito carece de claves recipient_view_pub y recipient_spend_pub validas.";
            return false;
        }

        if (crypto_core_ed25519_is_valid_point(dep.recipient_view_pub.data()) == 0 ||
            crypto_core_ed25519_is_valid_point(dep.recipient_spend_pub.data()) == 0) {
                sodium_memzero(expected_r.data(), sizeof(expected_r));
                sodium_memzero(hash_r, sizeof(hash_r));
                error_msg = "Claves de destinatario en recibo de deposito no son puntos validos en Ed25519.";
                return false;
            }
            Key256 rB;
            if (crypto_scalarmult_ed25519_noclamp(rB.data(), expected_r.data(), dep.recipient_view_pub.data()) != 0) {
                sodium_memzero(expected_r.data(), sizeof(expected_r));
                sodium_memzero(hash_r, sizeof(hash_r));
                error_msg = "Multiplicacion escalar rB fallida en verificacion de deposito.";
                return false;
            }
            uint8_t hash_s[64];
            crypto_generichash(hash_s, 64, rB.data(), 32, nullptr, 0);
            Key256 s_scalar;
            crypto_core_ed25519_scalar_reduce(s_scalar.data(), hash_s);
            Key256 hG;
            if (crypto_scalarmult_ed25519_base_noclamp(hG.data(), s_scalar.data()) != 0) {
                sodium_memzero(expected_r.data(), sizeof(expected_r));
                sodium_memzero(hash_r, sizeof(hash_r));
                sodium_memzero(rB.data(), sizeof(rB));
                sodium_memzero(hash_s, sizeof(hash_s));
                sodium_memzero(s_scalar.data(), sizeof(s_scalar));
                error_msg = "Multiplicacion escalar hG fallida en verificacion de deposito.";
                return false;
            }
            Key256 expected_P;
            if (crypto_core_ed25519_add(expected_P.data(), hG.data(), dep.recipient_spend_pub.data()) != 0) {
                sodium_memzero(expected_r.data(), sizeof(expected_r));
                sodium_memzero(hash_r, sizeof(hash_r));
                sodium_memzero(rB.data(), sizeof(rB));
                sodium_memzero(hash_s, sizeof(hash_s));
                sodium_memzero(s_scalar.data(), sizeof(s_scalar));
                error_msg = "Suma escalar hG + A fallida en verificacion de deposito.";
                return false;
            }
            sodium_memzero(rB.data(), sizeof(rB));
            sodium_memzero(hash_s, sizeof(hash_s));
            sodium_memzero(s_scalar.data(), sizeof(s_scalar));

            if (sodium_memcmp(out.destination_one_time.data(), expected_P.data(), 32) != 0) {
                sodium_memzero(expected_r.data(), sizeof(expected_r));
                sodium_memzero(hash_r, sizeof(hash_r));
                error_msg = "Violacion de integridad DKSAP: destination_one_time no coincide con la direccion del beneficiario.";
                return false;
            }
        sodium_memzero(expected_r.data(), sizeof(expected_r));
        sodium_memzero(hash_r, sizeof(hash_r));

        // 1.6 Validación de punto sobre curva Ed25519
        if (crypto_core_ed25519_is_valid_point(out.destination_one_time.data()) == 0) {
            error_msg = "Punto de destino en salida de deposito no es valido en la curva Ed25519.";
            return false;
        }

        try {
            temp_vault.deposit(dep.gross_usdt_deposited, dep.tx_hash);
        } catch (const std::exception& e) {
            error_msg = "Deposito invalido en boveda: " + std::string(e.what());
            return false;
        }
    }

    // 2. Validar transacciones RingCT
    std::vector<KeyImage> spent_images;
    std::vector<OneTimeOutput> new_utxos;

    for (const auto& tx : block.txs) {
        // 2.1. Validar que el tx_hash corresponda al hash canónico de la transacción (AUD-CRIT-03)
        Hash256 expected_canonical = RingSignatureEngine::compute_canonical_tx_hash(
            tx.outputs,
            tx.public_fee,
            tx.ring_sig.ring_pubkeys,
            tx.ring_sig.key_image
        );
        bool hash_valid = (sodium_memcmp(tx.tx_hash.data(), expected_canonical.data(), 32) == 0);
        if (!hash_valid && !tx.outputs.empty() && block.header.height == 0) {
            // Compatibilidad hacia atrás exclusivamente para bloque génesis histórico (AUD-RES-02)
            Hash256 legacy_hash;
            crypto_generichash(legacy_hash.data(), 32, tx.outputs[0].destination_one_time.data(), 32, nullptr, 0);
            if (sodium_memcmp(tx.tx_hash.data(), legacy_hash.data(), 32) == 0) {
                hash_valid = true;
            }
        }
        if (!hash_valid) {
            error_msg = "Hash de transaccion invalido (no coincide con el hash canonico BLAKE2b).";
            return false;
        }

        if (!RingSignatureEngine::verify(tx.tx_hash, tx.ring_sig)) {
            error_msg = "Firma de anillo MLSAG invalida en transaccion remota.";
            return false;
        }

        // 2.2. Prevención de doble gasto intra-bloque e inter-bloque (AUD-H0-01, AUD-CRIT-01)
        KeyImage tx_ki_canonical = canonical_key_image(tx.ring_sig.key_image);
        for (const auto& ki : spent_images) {
            if (sodium_memcmp(canonical_key_image(ki).data(), tx_ki_canonical.data(), 32) == 0) {
                error_msg = "Intento de doble gasto intra-bloque: imagen de clave duplicada en transacciones o retiros del mismo bloque.";
                return false;
            }
        }
        if (key_image_ledger_.is_spent(tx.ring_sig.key_image) || db_.is_key_image_spent(tx.ring_sig.key_image)) {
            error_msg = "Intento de doble gasto: imagen de clave ya utilizada.";
            return false;
        }

        // 2.3. Validar que TODOS los participantes del anillo pertenezcan al ledger y tengan denominación homogénea (AUD-H0-02, AUD-RES-01)
        Amount tx_total_spent = tx.public_fee;
        for (const auto& out : tx.outputs) {
            if (out.amount == 0) {
                error_msg = "Monto de salida en transaccion debe ser mayor a cero.";
                return false;
            }
            if (!safe_add_amount(tx_total_spent, out.amount, tx_total_spent)) {
                error_msg = "Transaccion rechazada: desbordamiento aritmetico en la suma de salidas (Amount overflow P0-02).";
                return false;
            }
        }

        Amount ring_denomination = 0;
        bool denomination_set = false;
        for (const auto& r_pk : tx.ring_sig.ring_pubkeys) {
            bool found = false;
            for (const auto& u : utxo_pool_) {
                if (sodium_memcmp(r_pk.data(), u.destination_one_time.data(), 32) == 0) {
                    found = true;
                    if (!denomination_set) {
                        ring_denomination = u.amount;
                        denomination_set = true;
                    } else if (u.amount != ring_denomination) {
                        error_msg = "Violacion de homogeneidad en anillo: miembros con denominaciones dispares en libro mayor.";
                        return false;
                    }
                    break;
                }
            }
            if (!found) {
                error_msg = "Transaccion rechazada: el participante del anillo no existe en el libro mayor.";
                return false;
            }
        }

        if (!denomination_set || tx_total_spent > ring_denomination) {
            error_msg = "Violacion de conservacion de balance en transaccion remota: salidas superan la denominacion de los inputs del anillo.";
            return false;
        }

        spent_images.push_back(tx.ring_sig.key_image);
        for (const auto& out : tx.outputs) {
            new_utxos.push_back(out);
        }
    }

    // 3. Validar retiros (Hito 0: AUD-H0-P0-01 Prueba Criptográfica de Quema)
    for (const auto& co : block.withdrawal_outputs) {
        if (co.amount == 0) {
            error_msg = "Salida de cambio en retiro con monto cero.";
            return false;
        }
        if (crypto_core_ed25519_is_valid_point(co.destination_one_time.data()) == 0 ||
            crypto_core_ed25519_is_valid_point(co.ephemeral_public_key.data()) == 0) {
            error_msg = "Punto geométrico inválido en salida de cambio de retiro.";
            return false;
        }
    }

    std::vector<bool> output_claimed(block.withdrawal_outputs.size(), false);
    size_t expected_change_count = 0;

    for (const auto& wdr : block.withdrawals) {
        if (wdr.gross_tokens_burned == 0) {
            error_msg = "Retiro rechazado: monto quemado debe ser mayor a cero.";
            return false;
        }

        // 3.1 Conservación económica y comisiones de retiro
        Amount expected_fee = (wdr.gross_tokens_burned * vault_.get_withdraw_fee_bps()) / 10000;
        Amount expected_net = wdr.gross_tokens_burned - expected_fee;
        if (wdr.fee_to_pool != expected_fee || wdr.net_usdt_to_tumble != expected_net) {
            error_msg = "Comision o monto neto de retiro no coincide con los parametros de la boveda.";
            return false;
        }

        // 3.2 Validación de dirección destino pública
        if (wdr.destination_address.empty()) {
            error_msg = "Direccion publica de destino para retiro no puede estar vacia.";
            return false;
        }

        // 3.3 Verificación del UTXO a quemar en el ledger
        bool utxo_found = false;
        Amount utxo_amount = 0;
        for (const auto& u : utxo_pool_) {
            if (sodium_memcmp(u.destination_one_time.data(), wdr.burned_utxo_pubkey.data(), 32) == 0) {
                utxo_found = true;
                utxo_amount = u.amount;
                break;
            }
        }
        if (!utxo_found) {
            error_msg = "Retiro rechazado: el UTXO a quemar no existe en el libro mayor.";
            return false;
        }
        if (utxo_amount < wdr.gross_tokens_burned) {
            error_msg = "Retiro rechazado: fondos insuficientes en el UTXO para el retiro solicitado.";
            return false;
        }

        // 3.3b Si hay cambio, verificar correspondencia biyectiva 1:1 estricta y ligadura criptográfica (Auditoría v3 - P1-02)
        Amount expected_change = utxo_amount - wdr.gross_tokens_burned;
        if (expected_change > 0) {
            expected_change_count++;
            bool change_found = false;
            for (size_t oi = 0; oi < block.withdrawal_outputs.size(); ++oi) {
                if (!output_claimed[oi] && block.withdrawal_outputs[oi].amount == expected_change) {
                    // Verificar que el destino coincida exactamente con la clave de cambio autorizada por el propietario
                    if (sodium_memcmp(block.withdrawal_outputs[oi].destination_one_time.data(), wdr.change_output_pubkey.data(), 32) == 0) {
                        output_claimed[oi] = true;
                        change_found = true;
                        break;
                    }
                }
            }
            if (!change_found) {
                error_msg = "Retiro rechazado: falta salida de cambio, monto discordante o clave pública no coincide con la prueba DLEQ (P1-02).";
                return false;
            }
        } else {
            Key256 empty_key{};
            if (sodium_memcmp(wdr.change_output_pubkey.data(), empty_key.data(), 32) != 0) {
                error_msg = "Retiro rechazado: retiro sin cambio contiene clave de cambio no nula en el recibo (P1-02).";
                return false;
            }
        }

        // 3.4 Verificación de prueba matemática de quema (DLEQ) ligando el cambio
        Hash256 burn_msg = RingSignatureEngine::compute_burn_message_hash(
            wdr.order_id,
            wdr.gross_tokens_burned,
            wdr.destination_address,
            wdr.change_output_pubkey
        );
        if (!RingSignatureEngine::verify_burn_proof(
                burn_msg,
                wdr.burned_utxo_pubkey,
                wdr.key_image,
                wdr.burn_signature_c0,
                wdr.burn_signature_s)) {
            error_msg = "Firma criptografica de quema invalida en retiro.";
            return false;
        }

        // 3.5 Protección anti-doble gasto de quema
        if (key_image_ledger_.is_spent(wdr.key_image) || db_.is_key_image_spent(wdr.key_image)) {
            error_msg = "Intento de doble gasto / doble quema en retiro: imagen de clave ya utilizada.";
            return false;
        }
        KeyImage wdr_ki_canonical = canonical_key_image(wdr.key_image);
        for (const auto& ki : spent_images) {
            if (sodium_memcmp(canonical_key_image(ki).data(), wdr_ki_canonical.data(), 32) == 0) {
                error_msg = "Imagen de clave duplicada en transacciones o retiros del mismo bloque.";
                return false;
            }
        }
        spent_images.push_back(wdr.key_image);

        try {
            temp_vault.request_withdrawal(wdr.gross_tokens_burned);
        } catch (const std::exception& e) {
            error_msg = "Retiro invalido en boveda: " + std::string(e.what());
            return false;
        }
    }

    // 3.6 Rechazar terminantemente cualquier salida de cambio huérfana o no respaldada (P0-01)
    if (block.withdrawal_outputs.size() != expected_change_count) {
        error_msg = "Bloque rechazado: salidas de cambio huérfanas o no respaldadas detectadas (P0-01). Se esperaban " +
                    std::to_string(expected_change_count) + " salidas, pero el bloque contiene " +
                    std::to_string(block.withdrawal_outputs.size());
        return false;
    }

    // 4. Incorporar salidas de depósitos y de cambio de retiros (AUD-H0-04)
    for (const auto& out : block.deposit_outputs) {
        new_utxos.push_back(out);
    }
    for (const auto& out : block.withdrawal_outputs) {
        new_utxos.push_back(out);
    }

    if (!temp_vault.audit_solvency()) {
        error_msg = "Fallo de solvencia 1:1 post-bloque.";
        return false;
    }

    // Aplicar al estado en memoria
    vault_.restore_state(
        temp_vault.get_total_collateral(),
        temp_vault.get_circulating_shielded_supply(),
        temp_vault.get_fee_pool_reserve()
    );

    for (const auto& ki : spent_images) {
        key_image_ledger_.register_key_image(ki);
    }

    for (const auto& out : new_utxos) {
        utxo_pool_.push_back(out);
    }

    for (const auto& tx : block.txs) {
        tx_history_.push_back(tx);
    }

    // Persistir atómicamente en LMDB (V4-09)
    try {
        db_.commit_block(block, vault_, new_utxos, spent_images);
    } catch (const std::exception& e) {
        error_msg = "Fallo en persistencia LMDB commit_block: " + std::string(e.what());
        return false;
    }

    success = true;
    return true;
}

std::vector<Key256> Node::get_random_decoys(size_t count, const Key256& exclude_pubkey, Amount target_amount) {
    std::lock_guard<std::mutex> lock(node_mutex_);
    return select_decoys(count + 1, exclude_pubkey, target_amount);
}

ShieldedTransaction Node::submit_pre_signed_transaction(const ShieldedTransaction& tx) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // 1. Validar que la firma de anillo contenga una imagen de clave válida en curva Ed25519
    if (crypto_core_ed25519_is_valid_point(tx.ring_sig.key_image.data()) == 0) {
        throw std::runtime_error("Imagen de clave no es un punto valido en Ed25519.");
    }

    // 2. Mitigación de cofactor 8 en key image (8 * I != Identidad)
    unsigned char ki_8[32];
    static const unsigned char eight_scalar[32] = {8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                                   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (crypto_scalarmult_ed25519_noclamp(ki_8, eight_scalar, tx.ring_sig.key_image.data()) != 0) {
        throw std::runtime_error("Fallo escalar en comprobacion de torsion de imagen de clave.");
    }
    static const unsigned char ed25519_identity[32] = {
        1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    if (sodium_memcmp(ki_8, ed25519_identity, 32) == 0) {
        throw std::runtime_error("Rechazado: imagen de clave en subgrupo de torsion pequenia de cofactor 8.");
    }

    // 3. Verificar doble gasto
    if (key_image_ledger_.is_spent(tx.ring_sig.key_image) || db_.is_key_image_spent(tx.ring_sig.key_image)) {
        throw std::runtime_error("Intento de DOBLE GASTO detectado: la imagen de clave ya fue utilizada.");
    }

    // 4. Verificar validez de puntos en los participantes del anillo MLSAG
    for (const auto& r_pk : tx.ring_sig.ring_pubkeys) {
        if (crypto_core_ed25519_is_valid_point(r_pk.data()) == 0) {
            throw std::runtime_error("Punto invalido en participantes del anillo MLSAG.");
        }
    }

    // 5. Verificar pertenencia y homogeneidad estricta de todos los miembros del anillo en el ledger (AUD-H0-02, AUD-H0-P0-02)
    Amount ring_denomination = 0;
    bool denomination_set = false;
    for (const auto& r_pk : tx.ring_sig.ring_pubkeys) {
        bool found = false;
        for (const auto& u : utxo_pool_) {
            if (sodium_memcmp(r_pk.data(), u.destination_one_time.data(), 32) == 0) {
                found = true;
                if (!denomination_set) {
                    ring_denomination = u.amount;
                    denomination_set = true;
                } else if (u.amount != ring_denomination) {
                    throw std::runtime_error("Violacion de homogeneidad en anillo: los miembros del anillo tienen diferentes denominaciones.");
                }
                break;
            }
        }
        if (!found) {
            throw std::runtime_error("Transaccion rechazada: participante del anillo no existe en el libro mayor.");
        }
    }

    // 5.1. Conservación estricta de balance anti-inflación
    Amount total_spent = tx.public_fee;
    for (const auto& out : tx.outputs) {
        if (out.amount == 0) {
            throw std::runtime_error("Salida de transaccion invalida: monto debe ser mayor a cero.");
        }
        if (!safe_add_amount(total_spent, out.amount, total_spent)) {
            throw std::runtime_error("Violacion de conservacion de balance: desbordamiento aritmetico en salidas (Amount overflow P0-02).");
        }
    }

    if (!denomination_set || total_spent > ring_denomination) {
        throw std::runtime_error("Violacion de conservacion de balance: la suma de salidas mas comision (" +
                                 format_usdt(total_spent) +
                                 ") excede la denominacion del input del anillo (" +
                                 format_usdt(ring_denomination) + ").");
    }

    // 6. Verificar hash canónico
    Hash256 canonical_hash = RingSignatureEngine::compute_canonical_tx_hash(
        tx.outputs,
        tx.public_fee,
        tx.ring_sig.ring_pubkeys,
        tx.ring_sig.key_image
    );
    if (sodium_memcmp(canonical_hash.data(), tx.tx_hash.data(), 32) != 0) {
        throw std::runtime_error("Hash de transaccion no coincide con el hash canonico BLAKE2b.");
    }

    // 7. Verificar firma MLSAG
    if (!RingSignatureEngine::verify(tx.tx_hash, tx.ring_sig)) {
        throw std::runtime_error("Firma de anillo MLSAG matematicamente invalida.");
    }

    // 8. Registrar imagen de clave
    key_image_ledger_.register_key_image(tx.ring_sig.key_image);

    // 9. Actualizar libro mayor con las nuevas salidas
    for (const auto& out : tx.outputs) {
        utxo_pool_.push_back(out);
    }

    tx_history_.push_back(tx);

    // 10. Empaquetar y asentar en Bloque LMDB atómico
    uint64_t next_height = db_.get_top_height() + 1;
    Block block;
    block.header.height = next_height;
    block.header.prev_block_hash = db_.get_top_block_hash();
    block.header.timestamp = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    block.txs.push_back(tx);
    block.header.merkle_root = block.compute_merkle_root();

    if (has_validator_key_) {
        block.header.sign(validator_secret_key_.data(), validator_pubkey_);
    }

    db_.commit_block(block, vault_, tx.outputs, {tx.ring_sig.key_image});

    if (on_block_mined_) {
        on_block_mined_(block);
    }

    return tx;
}

void Node::print_status() const {
    std::lock_guard<std::mutex> lock(node_mutex_);
    std::cout << "\n================ [ESTADO DEL NODO CRIPTO (LMDB)] ================\n";
    std::cout << " Altura Actual de Blockchain     : " << db_.get_top_height() << " bloques\n";
    std::cout << " Hash del Ultimo Bloque          : " << to_hex(db_.get_top_block_hash()) << "\n";
    std::cout << " Base de Datos LMDB              : " << db_path_ << "\n";
    std::cout << " Colateral Total Bloqueado (USDT): " << format_usdt(vault_.get_total_collateral()) << "\n";
    std::cout << " Suministro Privado Circulante   : " << format_usdt(vault_.get_circulating_shielded_supply()) << "\n";
    std::cout << " Reserva en Pool de Comisiones   : " << format_usdt(vault_.get_fee_pool_reserve()) << "\n";
    std::cout << " Solvencia Invariante 1:1        : " << (vault_.audit_solvency() ? "VALIDA (100% RESPALDADO)" : "ERROR DE SOLVENCIA") << "\n";
    std::cout << " Salidas en Ledger Oculto (UTXOs): " << utxo_pool_.size() << "\n";
    std::cout << " Imagenes de Clave Gastadas      : " << key_image_ledger_.size() << "\n";
    std::cout << " Comision Deposito               : " << vault_.get_deposit_fee_bps() << " bps (" << (vault_.get_deposit_fee_bps() / 100.0) << "%)\n";
    std::cout << " Comision Retiro                 : " << vault_.get_withdraw_fee_bps() << " bps (" << (vault_.get_withdraw_fee_bps() / 100.0) << "%)\n";
    std::cout << "===================================================================\n\n";
}

} // namespace crypto
