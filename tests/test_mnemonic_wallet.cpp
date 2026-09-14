#include "mnemonic.hpp"
#include "wallet_manager.hpp"
#include "node.hpp"
#include "rpc_server.hpp"
#include <iostream>
#include <cassert>
#include <filesystem>

void cleanup_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  TEST SUITE: BIP-39 MNEMONIC ENGINE & CLI WALLET MANAGER       \n";
    std::cout << "=================================================================\n\n";

    // -------------------------------------------------------------
    // TEST 1: Generación y Validación de 24 palabras BIP-39
    // -------------------------------------------------------------
    std::cout << "[TEST 1] Generación de mnemónico de 24 palabras y checksum SHA-256...\n";
    std::string phrase1 = crypto::MnemonicEngine::generate_24_words();
    auto words1 = crypto::MnemonicEngine::split_words(phrase1);
    assert(words1.size() == 24);
    assert(crypto::MnemonicEngine::validate_mnemonic(phrase1));
    std::cout << "  [OK] Frase válida generada: " << words1[0] << " ... " << words1[23] << "\n";

    // Probar detección de palabras corruptas
    std::string corrupt_phrase = phrase1;
    corrupt_phrase.replace(0, words1[0].size(), "palabrainvalida");
    assert(!crypto::MnemonicEngine::validate_mnemonic(corrupt_phrase));
    std::cout << "  [OK] Palabra ajena al diccionario BIP-39 rechazada.\n";

    // Probar detección de checksum corrupto al permutar dos palabras
    auto words_permuted = words1;
    std::swap(words_permuted[0], words_permuted[1]);
    std::string permuted_phrase;
    for (size_t i = 0; i < words_permuted.size(); ++i) {
        if (i > 0) permuted_phrase += " ";
        permuted_phrase += words_permuted[i];
    }
    assert(!crypto::MnemonicEngine::validate_mnemonic(permuted_phrase));
    std::cout << "  [OK] Checksum SHA-256 inválido detectado y rechazado ante permutación.\n";

    // -------------------------------------------------------------
    // TEST 2: Derivación determinista Ed25519 (Spend Key + View Key)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 2] Derivación determinista de claves criptográficas...\n";
    auto wallet_a1 = crypto::MnemonicEngine::mnemonic_to_wallet(phrase1);
    auto wallet_a2 = crypto::MnemonicEngine::mnemonic_to_wallet(phrase1);

    assert(wallet_a1.spend_private_key == wallet_a2.spend_private_key);
    assert(wallet_a1.spend_public_key == wallet_a2.spend_public_key);
    assert(wallet_a1.view_private_key == wallet_a2.view_private_key);
    assert(wallet_a1.view_public_key == wallet_a2.view_public_key);
    assert(wallet_a1.get_public_address().encode() == wallet_a2.get_public_address().encode());
    std::cout << "  [OK] Derivación 100% determinista y reproducible.\n";
    std::cout << "  Dirección Stealth: " << wallet_a1.get_public_address().encode() << "\n";

    // -------------------------------------------------------------
    // TEST 3: Persistencia en Archivo .wallet (Save & Load)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 3] Persistencia en disco (.wallet)...\n";
    const std::string wallet_file = "./test_storage.wallet";
    std::filesystem::remove(wallet_file);

    crypto::WalletManager mgr_writer("127.0.0.1", 8082);
    mgr_writer.restore_wallet("test_user", phrase1);
    mgr_writer.save_wallet(wallet_file);
    assert(std::filesystem::exists(wallet_file));

    crypto::WalletManager mgr_reader("127.0.0.1", 8082);
    mgr_reader.load_wallet(wallet_file);
    assert(mgr_reader.get_name() == "test_user");
    assert(mgr_reader.get_mnemonic() == phrase1);
    assert(mgr_reader.get_address().encode() == mgr_writer.get_address().encode());
    std::filesystem::remove(wallet_file);
    std::cout << "  [OK] Guardado y recarga de billetera verificados.\n";

    // -------------------------------------------------------------
    // TEST 4: Ciclo Completo End-to-End con Nodo Local (Port 8082)
    // -------------------------------------------------------------
    std::cout << "\n[TEST 4] Integración E2E: Depósito -> Escaneo View-Key -> Transferencia -> Retiro...\n";
    const std::string test_db = "./test_wallet_lmdb";
    cleanup_dir(test_db);

    crypto::Node node(50, 50, test_db); // 0.5% comisiones
    crypto::RpcServer rpc(node, "127.0.0.1", 8082);
    rpc.start_async();

    crypto::WalletManager alice("127.0.0.1", 8082);
    alice.create_wallet("Alice");
    std::cout << "  [Alice] Creada con dirección: " << alice.get_address().encode() << "\n";

    crypto::WalletManager bob("127.0.0.1", 8082);
    bob.create_wallet("Bob");
    std::cout << "  [Bob]   Creada con dirección: " << bob.get_address().encode() << "\n";

    // 4.1 Alice recibe depósito colateral de 100 USDT respaldado 1:1 en L2
    std::cout << "\n  -> Alice deposita 100 USDT colaterales en Arbitrum L2...\n";
    std::string tx_hash = "0xL2DepositConfirmedAlice01", err;
    node.buy_shielded(100 * crypto::USDT_UNIT, alice.get_address(), tx_hash, 0);
    bool alice_sync = alice.sync(err);
    assert(alice_sync);
    // 100 USDT bruto - 0.5% fee = 99.5 USDT neto
    assert(alice.get_balance() == 99'500'000ULL);
    assert(alice.get_utxos().size() == 1);
    std::cout << "  [OK] Depósito procesado. Saldo de Alice: " << crypto::format_usdt(alice.get_balance()) << "\n";

    // 4.2 Alice envía 25 USDT a Bob mediante RingCT
    std::cout << "\n  -> Alice envía 25 USDT privados a Bob mediante RingCT...\n";
    bool tx_ok = alice.transfer(bob.get_address().encode(), 25 * crypto::USDT_UNIT, tx_hash, err);
    assert(tx_ok);
    assert(!tx_hash.empty());

    // 4.3 Bob sincroniza mediante su View-Key privada
    std::cout << "\n  -> Bob escanea la blockchain con su View-Key privada...\n";
    bool bob_sync = bob.sync(err);
    assert(bob_sync);
    assert(bob.get_balance() == 25 * crypto::USDT_UNIT);
    assert(bob.get_utxos().size() == 1);
    std::cout << "  [OK] Bob descubrió su UTXO confidencial. Saldo de Bob: " << crypto::format_usdt(bob.get_balance()) << "\n";

    // 4.4 Bob solicita retiro de 10 USDT hacia dirección pública externa
    std::cout << "\n  -> Bob solicita retiro de 10 USDT hacia dirección pública...\n";
    std::string order_id;
    size_t fragments = 0;
    bool wdr_ok = bob.withdraw(10 * crypto::USDT_UNIT, "0xb0b0000000000000000000000000000000000001", order_id, fragments, err);
    assert(wdr_ok);
    assert(!order_id.empty());
    assert(fragments > 0);
    std::cout << "  [OK] Retiro 1:1 encolado en micro-tumbler (" << fragments << " fragmentos generados).\n";
    std::cout << "  Saldo restante de Bob: " << crypto::format_usdt(bob.get_balance()) << "\n";

    // Limpieza
    rpc.stop();
    cleanup_dir(test_db);

    std::cout << "\n=================================================================\n";
    std::cout << "  >>> TODOS LOS TESTS DE BILLETERA Y MNEMÓNICO PASARON EXITOSAMENTE! <<<\n";
    std::cout << "=================================================================\n";

    return 0;
}
