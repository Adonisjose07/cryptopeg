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
    init_or_recover_database();
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
        // Recuperar estado desde LMDB
        db_.load_vault_state(vault_);
        utxo_pool_ = db_.load_all_utxos();

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
}

DepositReceipt Node::buy_shielded(Amount usdt_gross, const StealthAddress& recipient_address, const std::string& custom_tx_hash, uint64_t custom_timestamp) {
    std::lock_guard<std::mutex> lock(node_mutex_);

    // 1. La bóveda recibe los USDT públicos, deduce la comisión al pool y emite el recibo 1:1
    DepositReceipt receipt = vault_.deposit(usdt_gross, custom_tx_hash);

    // 2. Generar el output furtivo (one-time stealth output) para el receptor
    // Si viene custom_tx_hash (depósito on-chain), se usa como semilla determinista
    Hash256 seed;
    const Hash256* seed_ptr = nullptr;
    if (!custom_tx_hash.empty()) {
        crypto_generichash(
            seed.data(), 32,
            reinterpret_cast<const uint8_t*>(custom_tx_hash.data()),
            custom_tx_hash.size(),
            nullptr, 0
        );
        seed_ptr = &seed;
    }

    OneTimeOutput utxo = StealthProtocol::create_one_time_output(
        recipient_address,
        receipt.net_shielded_tokens_minted,
        seed_ptr
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

    db_.commit_block(block, vault_, {utxo}, {});

    if (on_block_mined_) {
        on_block_mined_(block);
    }

    return receipt;
}

std::vector<Key256> Node::select_decoys(size_t ring_size, const Key256& real_pubkey) {
    std::vector<Key256> candidates;
    for (const auto& out : utxo_pool_) {
        if (std::memcmp(out.destination_one_time.data(), real_pubkey.data(), 32) != 0) {
            candidates.push_back(out.destination_one_time);
        }
    }

    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(candidates.begin(), candidates.end(), g);

    std::vector<Key256> selected;
    // Si no hay suficientes salidas históricas, generamos señuelos criptográficos sintéticos
    for (size_t i = 0; i < ring_size - 1; ++i) {
        if (i < candidates.size()) {
            selected.push_back(candidates[i]);
        } else {
            // Señuelo sintético válido sobre la curva Ed25519
            Key256 fake_priv, fake_pub;
            crypto_core_ed25519_scalar_random(fake_priv.data());
            crypto_scalarmult_ed25519_base_noclamp(fake_pub.data(), fake_priv.data());
            selected.push_back(fake_pub);
        }
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

    if (input_utxo.amount < (send_amount + tx_fee)) {
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

    // 4. Construir el anillo de señuelos (tamaño típico: 5 a 11)
    size_t ring_size = 5;
    auto decoys = select_decoys(ring_size, input_utxo.destination_one_time);

    // Insertar la clave real en una posición aleatoria del anillo
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> pos_dist(0, ring_size - 1);
    size_t real_index = pos_dist(gen);

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

    // 6. Generar hash de la transacción
    Hash256 tx_hash;
    crypto_generichash(tx_hash.data(), 32, recipient_out.destination_one_time.data(), 32, nullptr, 0);

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

    // 4. Si hubo cambio, re-emitir output privado para el usuario
    std::vector<OneTimeOutput> new_outs;
    Amount change = input_utxo.amount - tokens_to_withdraw;
    if (change > 0) {
        OneTimeOutput change_out = StealthProtocol::create_one_time_output(
            burner_wallet.get_public_address(),
            change
        );
        utxo_pool_.push_back(change_out);
        new_outs.push_back(change_out);
    }

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
    block.header.merkle_root = block.compute_merkle_root();

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
    if (block.header.height != current_height + 1) {
        error_msg = "Altura de bloque inválida: se esperaba " + std::to_string(current_height + 1) +
                    ", recibido " + std::to_string(block.header.height);
        return false;
    }

    Hash256 current_top_hash = db_.get_top_block_hash();
    if (std::memcmp(block.header.prev_block_hash.data(), current_top_hash.data(), 32) != 0) {
        error_msg = "Hash de bloque previo no coincide con la punta local de la cadena.";
        return false;
    }

    Hash256 calculated_root = block.compute_merkle_root();
    if (std::memcmp(block.header.merkle_root.data(), calculated_root.data(), 32) != 0) {
        error_msg = "Raiz de Merkle invalida en cabecera del bloque.";
        return false;
    }

    Vault temp_vault(vault_.get_deposit_fee_bps(), vault_.get_withdraw_fee_bps());
    temp_vault.restore_state(
        vault_.get_total_collateral(),
        vault_.get_circulating_shielded_supply(),
        vault_.get_fee_pool_reserve()
    );

    // 1. Validar depósitos
    for (const auto& dep : block.deposits) {
        try {
            temp_vault.deposit(dep.gross_usdt_deposited);
        } catch (const std::exception& e) {
            error_msg = "Deposito invalido en bloque: " + std::string(e.what());
            return false;
        }
    }

    // 2. Validar transacciones RingCT
    std::vector<KeyImage> spent_images;
    std::vector<OneTimeOutput> new_utxos;

    for (const auto& tx : block.txs) {
        if (!RingSignatureEngine::verify(tx.tx_hash, tx.ring_sig)) {
            error_msg = "Firma de anillo MLSAG invalida en transaccion remota.";
            return false;
        }

        if (key_image_ledger_.is_spent(tx.ring_sig.key_image) || db_.is_key_image_spent(tx.ring_sig.key_image)) {
            error_msg = "Intento de doble gasto: imagen de clave ya utilizada.";
            return false;
        }

        spent_images.push_back(tx.ring_sig.key_image);
        for (const auto& out : tx.outputs) {
            new_utxos.push_back(out);
        }
    }

    // 3. Validar retiros
    for (const auto& wdr : block.withdrawals) {
        try {
            temp_vault.request_withdrawal(wdr.gross_tokens_burned);
        } catch (const std::exception& e) {
            error_msg = "Retiro invalido en bloque: " + std::string(e.what());
            return false;
        }
    }

    // 4. Incorporar salidas de depósitos
    for (const auto& out : block.deposit_outputs) {
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

    // Persistir atómicamente en LMDB
    db_.commit_block(block, vault_, new_utxos, spent_images);

    return true;
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
