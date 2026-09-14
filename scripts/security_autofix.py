from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one match, got {count}\n--- OLD ---\n{old}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")
    print(f"patched {path}")


# -----------------------------------------------------------------------------
# src/block.cpp: canonical quorum ordering + protocol count bounds
# -----------------------------------------------------------------------------
replace_once(
    "src/block.cpp",
    '#include <stdexcept>\n#include <sodium.h>\n',
    '#include <stdexcept>\n#include <algorithm>\n#include <sodium.h>\n',
)

replace_once(
    "src/block.cpp",
    'namespace crypto {\n\n// 1. OneTimeOutput\n',
    'namespace crypto {\n\nnamespace {\nconstexpr uint32_t MAX_BLOCK_TRANSACTIONS = 10000;\nconstexpr uint32_t MAX_BLOCK_DEPOSITS = 10000;\nconstexpr uint32_t MAX_BLOCK_WITHDRAWALS = 10000;\nconstexpr uint32_t MAX_BLOCK_OUTPUTS = 20000;\nconstexpr uint32_t MAX_TX_OUTPUTS = 256;\n}\n\n// 1. OneTimeOutput\n',
)

replace_once(
    "src/block.cpp",
    '    uint32_t out_count = r.read_u32();\n    tx.outputs.reserve(out_count);\n',
    '    uint32_t out_count = r.read_u32();\n    if (out_count > MAX_TX_OUTPUTS) {\n        throw std::runtime_error("Numero de outputs de transaccion excede el limite protocolario.");\n    }\n    tx.outputs.reserve(out_count);\n',
)

replace_once(
    "src/block.cpp",
    '''void BlockHeader::add_quorum_signature(const Key256& pub_key_32, const Signature64& sig) {
    for (const auto& pk : quorum_pubkeys) {
        if (sodium_memcmp(pk.data(), pub_key_32.data(), 32) == 0) return;
    }
    quorum_pubkeys.push_back(pub_key_32);
    quorum_signatures.push_back(sig);
}
''',
    '''void BlockHeader::add_quorum_signature(const Key256& pub_key_32, const Signature64& sig) {
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
''',
)

replace_once(
    "src/block.cpp",
    '''    uint32_t tx_count = r.read_u32();
    block.txs.reserve(tx_count);
''',
    '''    uint32_t tx_count = r.read_u32();
    if (tx_count > MAX_BLOCK_TRANSACTIONS) {
        throw std::runtime_error("Cantidad de transacciones excede el limite protocolario del bloque.");
    }
    block.txs.reserve(tx_count);
''',
)
replace_once(
    "src/block.cpp",
    '''    uint32_t dep_count = r.read_u32();
    block.deposits.reserve(dep_count);
''',
    '''    uint32_t dep_count = r.read_u32();
    if (dep_count > MAX_BLOCK_DEPOSITS) {
        throw std::runtime_error("Cantidad de depositos excede el limite protocolario del bloque.");
    }
    block.deposits.reserve(dep_count);
''',
)
replace_once(
    "src/block.cpp",
    '''    uint32_t wdr_count = r.read_u32();
    block.withdrawals.reserve(wdr_count);
''',
    '''    uint32_t wdr_count = r.read_u32();
    if (wdr_count > MAX_BLOCK_WITHDRAWALS) {
        throw std::runtime_error("Cantidad de retiros excede el limite protocolario del bloque.");
    }
    block.withdrawals.reserve(wdr_count);
''',
)
# There are two block-level out_count snippets; patch each distinct reserve target.
replace_once(
    "src/block.cpp",
    '''        uint32_t out_count = r.read_u32();
        block.deposit_outputs.reserve(out_count);
''',
    '''        uint32_t out_count = r.read_u32();
        if (out_count > MAX_BLOCK_OUTPUTS) {
            throw std::runtime_error("Cantidad de outputs de deposito excede el limite protocolario del bloque.");
        }
        block.deposit_outputs.reserve(out_count);
''',
)
replace_once(
    "src/block.cpp",
    '''        uint32_t out_count = r.read_u32();
        block.withdrawal_outputs.reserve(out_count);
''',
    '''        uint32_t out_count = r.read_u32();
        if (out_count > MAX_BLOCK_OUTPUTS) {
            throw std::runtime_error("Cantidad de outputs de retiro excede el limite protocolario del bloque.");
        }
        block.withdrawal_outputs.reserve(out_count);
''',
)

# -----------------------------------------------------------------------------
# src/node.cpp: rollback in-memory state after LMDB failures + safe remote fees
# -----------------------------------------------------------------------------
replace_once(
    "src/node.cpp",
    '    db_.commit_block(block, vault_, {utxo}, {});\n\n    if (on_block_mined_) {\n',
    '''    try {
        db_.commit_block(block, vault_, {utxo}, {});
    } catch (...) {
        // LMDB is the source of truth: never leave a successful-looking mint only in RAM.
        try { recover_state_from_db(); } catch (...) {}
        throw;
    }

    if (on_block_mined_) {
''',
)
replace_once(
    "src/node.cpp",
    '    db_.commit_block(block, vault_, new_outputs, {sig.key_image});\n\n    if (on_block_mined_) {\n',
    '''    try {
        db_.commit_block(block, vault_, new_outputs, {sig.key_image});
    } catch (...) {
        try { recover_state_from_db(); } catch (...) {}
        throw;
    }

    if (on_block_mined_) {
''',
)
replace_once(
    "src/node.cpp",
    '    db_.commit_block(block, vault_, new_outs, {img});\n\n    if (on_block_mined_) {\n',
    '''    try {
        db_.commit_block(block, vault_, new_outs, {img});
    } catch (...) {
        try { recover_state_from_db(); } catch (...) {}
        throw;
    }

    if (on_block_mined_) {
''',
)
replace_once(
    "src/node.cpp",
    '    db_.commit_block(block, vault_, tx.outputs, {tx.ring_sig.key_image});\n\n    if (on_block_mined_) {\n',
    '''    try {
        db_.commit_block(block, vault_, tx.outputs, {tx.ring_sig.key_image});
    } catch (...) {
        try { recover_state_from_db(); } catch (...) {}
        throw;
    }

    if (on_block_mined_) {
''',
)
replace_once(
    "src/node.cpp",
    '        Amount expected_fee = (dep.gross_usdt_deposited * vault_.get_deposit_fee_bps()) / 10000;\n',
    '        Amount expected_fee = safe_fee_calc(dep.gross_usdt_deposited, vault_.get_deposit_fee_bps());\n',
)
replace_once(
    "src/node.cpp",
    '        Amount expected_fee = (wdr.gross_tokens_burned * vault_.get_withdraw_fee_bps()) / 10000;\n',
    '        Amount expected_fee = safe_fee_calc(wdr.gross_tokens_burned, vault_.get_withdraw_fee_bps());\n',
)

# -----------------------------------------------------------------------------
# src/rpc_server.cpp: server-derived canonical deposit event id + M-of-N threshold
# -----------------------------------------------------------------------------
replace_once(
    "src/rpc_server.cpp",
    '#include <filesystem>\n#include <cstring>\n',
    '#include <filesystem>\n#include <cstring>\n#include <algorithm>\n#include <cctype>\n',
)

replace_once(
    "src/rpc_server.cpp",
    '''            uint64_t chain_id = body.value("chain_id", 421614ULL);
            std::string contract_addr = body.value("contract_address", "");
            uint32_t log_index = body.value("log_index", 0U);
            uint64_t l2_block = body.value("l2_block_number", 0ULL);
            std::string event_id = body.value("event_id", "");
            if (event_id.empty()) {
                event_id = std::to_string(chain_id) + ":" + contract_addr + ":" + custom_tx + ":" + std::to_string(log_index);
            }

            if (node_.is_deposit_tx_processed(event_id) || node_.is_deposit_tx_processed(custom_tx)) {
                res.status = 409;
                res.set_content(json{{"error", "Transacción de depósito ya procesada previamente (idempotencia garantizada)."}}.dump(), "application/json");
                return;
            }
''',
    '''            uint64_t chain_id = body.value("chain_id", 421614ULL);
            std::string contract_addr = body.value("contract_address", "");
            uint32_t log_index = body.value("log_index", 0U);
            uint64_t l2_block = body.value("l2_block_number", 0ULL);

            if (contract_addr.empty()) {
                res.status = 400;
                res.set_content(json{{"error", "El campo contract_address es obligatorio para construir la identidad canónica del evento."}}.dump(), "application/json");
                return;
            }

            std::string normalized_contract = contract_addr;
            std::transform(normalized_contract.begin(), normalized_contract.end(), normalized_contract.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string canonical_event_id = std::to_string(chain_id) + ":" + normalized_contract + ":" +
                                                   custom_tx + ":" + std::to_string(log_index);
            const std::string supplied_event_id = body.value("event_id", "");
            if (!supplied_event_id.empty() && supplied_event_id != canonical_event_id) {
                res.status = 400;
                res.set_content(json{{"error", "event_id no coincide con la identidad canónica derivada de chainId/contract/txHash/logIndex."}}.dump(), "application/json");
                return;
            }
            const std::string& event_id = canonical_event_id;

            // Idempotencia por evento L2, no por txHash: una misma transacción puede emitir varios logs DepositInitiated.
            if (node_.is_deposit_tx_processed(event_id)) {
                res.status = 409;
                res.set_content(json{{"error", "Evento de depósito ya procesado previamente (idempotencia garantizada)."}}.dump(), "application/json");
                return;
            }
''',
)

replace_once(
    "src/rpc_server.cpp",
    '''            if (attested_signatures.empty()) {
                res.status = 401;
                res.set_content(json{{"error", "No autorizado: se requiere un certificado criptográfico de depósito válido firmado por un oráculo autorizado (P0-01)."}}.dump(), "application/json");
                return;
            }

            auto receipt = node_.buy_shielded(gross, recipient, event_id, custom_ts);
''',
    '''            const uint32_t required_attestations = std::max(1U, node_.get_quorum_threshold());
            if (attested_signatures.size() < required_attestations) {
                res.status = 401;
                res.set_content(json{{"error", "No autorizado: certificado de depósito no alcanza el quórum M-de-N requerido."},
                                     {"required", required_attestations},
                                     {"verified", attested_signatures.size()}}.dump(), "application/json");
                return;
            }

            auto receipt = node_.buy_shielded(gross, recipient, event_id, custom_ts);
''',
)

# -----------------------------------------------------------------------------
# contracts/scripts/oracle_listener.js: dedicated key, deterministic bootstrap, event-only idempotency
# -----------------------------------------------------------------------------
replace_once(
    "contracts/scripts/oracle_listener.js",
    'const L2_CONFIRMATION_BLOCKS = parseInt(process.env.L2_CONFIRMATION_BLOCKS || "12");\n',
    'const L2_CONFIRMATION_BLOCKS = parseInt(process.env.L2_CONFIRMATION_BLOCKS || "12");\nconst ORACLE_START_BLOCK = parseInt(process.env.ORACLE_START_BLOCK || "0");\n',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '  const seedHex = process.env.VALIDATOR_ED25519_PRIVKEY || process.env.VALIDATOR_PRIVATE_KEY || process.env.TESTNET_PRIVATE_KEY || "";\n',
    '  const seedHex = process.env.VALIDATOR_ED25519_PRIVKEY || "";\n',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '    throw new Error("[ORACLE SECURITY CRITICAL] Clave de validador no configurada en VALIDATOR_ED25519_PRIVKEY o VALIDATOR_PRIVATE_KEY. Abortando por seguridad (V4-08).");\n',
    '    throw new Error("[ORACLE SECURITY CRITICAL] VALIDATOR_ED25519_PRIVKEY dedicada no configurada. No se permite reutilizar claves ECDSA/L2 para atestaciones Ed25519.");\n',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '''    if (lastCheckedL2Block === 0 || lastCheckedL2Block > currentL2) {
      lastCheckedL2Block = currentL2;
    }
''',
    '''    if (lastCheckedL2Block === 0) {
      if (!Number.isInteger(ORACLE_START_BLOCK) || ORACLE_START_BLOCK <= 0) {
        throw new Error("ORACLE_START_BLOCK debe configurarse al bloque de despliegue/checkpoint del Vault para evitar omitir depósitos históricos.");
      }
      lastCheckedL2Block = ORACLE_START_BLOCK - 1;
    }
    if (lastCheckedL2Block > currentL2) {
      throw new Error(`Cursor L2 persistido/configurado (${lastCheckedL2Block}) está por encima de la punta actual (${currentL2}).`);
    }
''',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '        if (processedTxHashes.has(eventId) || processedTxHashes.has(txHash)) continue;\n',
    '        if (processedTxHashes.has(eventId)) continue;\n',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '''            processedTxHashes.add(eventId);
            processedTxHashes.add(txHash);
            processedSuccessfully = true;
''',
    '''            processedTxHashes.add(eventId);
            processedSuccessfully = true;
''',
)
replace_once(
    "contracts/scripts/oracle_listener.js",
    '              gross_usdt: grossUSDT,\n              gross_usdt_raw: grossRawStr,\n',
    '              gross_usdt_raw: grossRawStr,\n',
)

print("security autofix batch applied successfully")
