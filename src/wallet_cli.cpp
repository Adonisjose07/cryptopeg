#include "wallet_manager.hpp"
#include "types.hpp"
#include <httplib.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

// Colores y formato ANSI
namespace color {
    const std::string RESET   = "\033[0m";
    const std::string BOLD    = "\033[1m";
    const std::string DIM     = "\033[2m";
    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string WHITE   = "\033[37m";
    const std::string GRAY    = "\033[90m";
}

void print_banner() {
    std::cout << color::CYAN << color::BOLD;
    std::cout << "  =================================================================\n";
    std::cout << "   CRYPTPEG USDT -- BILLETERA CLI INDEPENDIENTE (RINGCT / DKSAP)   \n";
    std::cout << "   Privacidad Grado Monero | Mnemónico BIP-39 24 Palabras | 1:1    \n";
    std::cout << "  =================================================================\n";
    std::cout << color::RESET << "\n";
}

void print_help() {
    std::cout << color::BOLD << "\nComandos disponibles en CryptoPeg Wallet CLI:\n" << color::RESET;
    std::cout << color::DIM << "--------------------------------------------------------------------------------\n" << color::RESET;
    std::cout << color::CYAN << "  create <nombre>" << color::RESET 
              << "               Generar nueva billetera con frase semilla de 24 palabras\n";
    std::cout << color::CYAN << "  restore <nombre>" << color::RESET 
              << "              Restaurar billetera a partir de las 24 palabras mnemónicas\n";
    std::cout << color::CYAN << "  open <archivo.wallet>" << color::RESET 
              << "         Abrir archivo de billetera guardado previamente\n";
    std::cout << color::CYAN << "  save [archivo.wallet]" << color::RESET 
              << "         Guardar billetera activa en disco (por defecto: <nombre>.wallet)\n";
    std::cout << color::CYAN << "  close" << color::RESET 
              << "                        Cerrar y descargar la billetera activa\n";
    std::cout << color::CYAN << "  address" << color::RESET 
              << "                      Mostrar dirección pública stealth (DKSAP)\n";
    std::cout << color::CYAN << "  seed" << color::RESET 
              << "                         Mostrar las 24 palabras mnemónicas de respaldo\n";
    std::cout << color::CYAN << "  balance" << color::RESET 
              << "                      Consultar saldo confirmado y lista de UTXOs propios\n";
    std::cout << color::CYAN << "  sync" << color::RESET 
              << "                         Escanear la blockchain mediante View-Key privada\n";
    std::cout << color::CYAN << "  deposit <monto_usdt>" << color::RESET 
              << "         Acuñar tokens 1:1 depositando USDT colateral en la bóveda\n";
    std::cout << color::CYAN << "  transfer <dir_stealth> <monto>" << color::RESET 
              << " Transferencia confidencial con anillo RingCT y señuelos\n";
    std::cout << color::CYAN << "  withdraw <monto> <dir_publica>" << color::RESET 
              << " Canjear 1:1 USDT público vía micro-tumbler asíncrono\n";
    std::cout << color::CYAN << "  status" << color::RESET 
              << "                       Consultar estado de la red, altura y solvencia del nodo\n";
    std::cout << color::CYAN << "  peers" << color::RESET 
              << "                        Listar pares conectados en la red P2P descentralizada\n";
    std::cout << color::CYAN << "  addpeer <url>" << color::RESET 
              << "                Conectar manualmente con otro nodo validador P2P\n";
    std::cout << color::CYAN << "  daemon [host:puerto]" << color::RESET 
              << "         Ver o cambiar la dirección del nodo daemon\n";
    std::cout << color::CYAN << "  clear" << color::RESET 
              << "                        Limpiar pantalla del terminal\n";
    std::cout << color::CYAN << "  help" << color::RESET 
              << "                         Mostrar esta lista de ayuda\n";
    std::cout << color::CYAN << "  exit / quit" << color::RESET 
              << "                  Salir de la billetera de forma segura\n";
    std::cout << color::DIM << "--------------------------------------------------------------------------------\n\n" << color::RESET;
}

void display_mnemonic_box(const std::string& mnemonic) {
    auto words = crypto::MnemonicEngine::split_words(mnemonic);
    std::cout << color::YELLOW << color::BOLD;
    std::cout << "\n  +-------------------------------------------------------------------------+\n";
    std::cout << "  |             TU FRASE SEMILLA MNEMÓNICA BIP-39 (24 PALABRAS)             |\n";
    std::cout << "  +-------------------------------------------------------------------------+\n" << color::RESET;

    for (size_t i = 0; i < 24; i += 4) {
        std::cout << "   ";
        for (size_t col = 0; col < 4 && (i + col) < 24; ++col) {
            size_t idx = i + col;
            std::ostringstream oss;
            oss << "[" << std::setw(2) << std::setfill('0') << (idx + 1) << "] " << words[idx];
            std::string item = oss.str();
            std::cout << color::GREEN << std::left << std::setw(18) << item << color::RESET;
        }
        std::cout << "\n";
    }

    std::cout << color::YELLOW << color::BOLD;
    std::cout << "  +-------------------------------------------------------------------------+\n";
    std::cout << "  | [!] ADVERTENCIA: Anota estas 24 palabras en papel y guárdalas fuera de    |\n";
    std::cout << "  |     línea. Son la ÚNICA manera de recuperar tus fondos en cualquier pc.  |\n";
    std::cout << "  +-------------------------------------------------------------------------+\n\n" << color::RESET;
}

std::vector<std::string> parse_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string t;
    while (iss >> t) {
        tokens.push_back(t);
    }
    return tokens;
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    std::string daemon_host = "127.0.0.1";
    int daemon_port = 8080;
    std::string initial_wallet_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--daemon" && i + 1 < argc) {
            std::string endpoint = argv[++i];
            auto colon = endpoint.find(':');
            if (colon != std::string::npos) {
                daemon_host = endpoint.substr(0, colon);
                daemon_port = std::stoi(endpoint.substr(colon + 1));
            } else {
                daemon_host = endpoint;
            }
        } else if (arg == "--wallet" && i + 1 < argc) {
            initial_wallet_path = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Uso: crypto_wallet_cli [OPCIONES]\n\n"
                      << "Opciones:\n"
                      << "  --daemon <host:puerto>   Dirección del nodo daemon (por defecto: 127.0.0.1:8080)\n"
                      << "  --wallet <ruta.wallet>   Cargar automáticamente archivo de billetera al iniciar\n"
                      << "  --help, -h               Mostrar esta ayuda\n";
            return 0;
        }
    }

    print_banner();

    crypto::WalletManager manager(daemon_host, daemon_port);
    std::cout << color::DIM << "[INFO] Conectando a nodo daemon en " << manager.get_daemon_url() << "...\n" << color::RESET;

    // Verificar conectividad inicial
    std::string status_raw, err_msg;
    if (manager.get_node_status(status_raw, err_msg)) {
        try {
            auto j = json::parse(status_raw);
            std::cout << color::GREEN << "[OK] Nodo conectado."
                      << " Altura: " << j["blockchain_height"]
                      << " | Solvente: " << (j["vault"]["is_solvent_1_to_1"].get<bool>() ? "SÍ" : "NO")
                      << " | Colateral: " << j["vault"]["total_collateral_usdt"].get<std::string>()
                      << color::RESET << "\n\n";
        } catch (...) {
            std::cout << color::YELLOW << "[!] Nodo respondió con datos no reconocidos.\n\n" << color::RESET;
        }
    } else {
        std::cout << color::YELLOW << "[!] Advertencia: Nodo daemon no disponible en " << manager.get_daemon_url()
                  << " (" << err_msg << "). Puedes usar comandos fuera de línea o configurar 'daemon <host:puerto>'.\n\n"
                  << color::RESET;
    }

    if (!initial_wallet_path.empty()) {
        try {
            manager.load_wallet(initial_wallet_path);
            std::cout << color::GREEN << "[OK] Billetera '" << manager.get_name() 
                      << "' cargada desde " << initial_wallet_path << color::RESET << "\n";
            std::string sync_err;
            if (manager.sync(sync_err)) {
                std::cout << color::GREEN << "[OK] Sincronización exitosa. Saldo: " 
                          << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << color::RED << "[ERROR] No se pudo cargar la billetera: " << e.what() << color::RESET << "\n";
        }
    }

    std::cout << color::DIM << "Escribe 'help' para ver la lista de comandos.\n\n" << color::RESET;

    std::string line;
    while (true) {
        // Renderizar Prompt
        if (manager.has_wallet()) {
            std::cout << color::BOLD << color::BLUE << "[wallet: " 
                      << color::CYAN << manager.get_name() << color::BLUE << " | "
                      << color::GREEN << crypto::format_usdt(manager.get_balance()) << color::BLUE << "] > " 
                      << color::RESET;
        } else {
            std::cout << color::BOLD << color::BLUE << "[crypto-wallet] > " << color::RESET;
        }

        if (!std::getline(std::cin, line)) {
            break; // EOF o Ctrl+D
        }

        auto tokens = parse_tokens(line);
        if (tokens.empty()) continue;

        const std::string& cmd = tokens[0];

        if (cmd == "exit" || cmd == "quit") {
            std::cout << color::CYAN << "Cerrando CryptoPeg Wallet CLI. ¡Hasta luego!\n" << color::RESET;
            break;
        } else if (cmd == "help") {
            print_help();
        } else if (cmd == "clear") {
            std::cout << "\033[2J\033[1;1H";
        } else if (cmd == "daemon") {
            if (tokens.size() > 1) {
                std::string ep = tokens[1];
                auto colon = ep.find(':');
                std::string h = ep;
                int p = 8080;
                if (colon != std::string::npos) {
                    h = ep.substr(0, colon);
                    p = std::stoi(ep.substr(colon + 1));
                }
                manager.set_daemon(h, p);
                std::cout << color::GREEN << "[OK] Nodo daemon actualizado a: " << manager.get_daemon_url() << color::RESET << "\n";
            } else {
                std::cout << "Nodo daemon configurado actualmente: " << color::CYAN << manager.get_daemon_url() << color::RESET << "\n";
            }
        } else if (cmd == "status") {
            std::string raw, err;
            if (!manager.get_node_status(raw, err)) {
                std::cout << color::RED << "[ERROR] No se pudo obtener estado del nodo: " << err << color::RESET << "\n";
            } else {
                try {
                    auto j = json::parse(raw);
                    std::cout << color::BOLD << "\n--- ESTADO DEL NODO DAEMON ---\n" << color::RESET;
                    std::cout << "  Endpoint:               " << manager.get_daemon_url() << "\n";
                    std::cout << "  Altura de Blockchain:   " << j["blockchain_height"] << " bloques\n";
                    std::cout << "  Ruta de Base de Datos:  " << j["database_path"] << "\n";
                    std::cout << "  UTXOs en Pool:          " << j["utxo_pool_count"] << "\n";
                    std::cout << "  Colateral Bóveda:       " << color::GREEN << j["vault"]["total_collateral_usdt"].get<std::string>() << color::RESET << "\n";
                    std::cout << "  Oferta Circulante:      " << color::CYAN << j["vault"]["circulating_shielded_supply"].get<std::string>() << color::RESET << "\n";
                    std::cout << "  Reserva de Comisiones:  " << j["vault"]["fee_pool_reserve_usdt"].get<std::string>() << "\n";
                    std::cout << "  Solvencia 1:1 Auditada: " << (j["vault"]["is_solvent_1_to_1"].get<bool>() ? (color::GREEN + "100% RESPALDADO (SOLVENTE)") : (color::RED + "ALERTA: INSOLVENTE")) << color::RESET << "\n\n";
                } catch (const std::exception& e) {
                    std::cout << color::RED << "[ERROR] Parseando estado: " << e.what() << color::RESET << "\n";
                }
            }
        } else if (cmd == "peers") {
            try {
                httplib::Client cli(manager.get_daemon_url().c_str());
                cli.set_connection_timeout(3, 0);
                cli.set_read_timeout(5, 0);
                auto res = cli.Get("/api/v1/p2p/status");
                if (!res || res->status != 200) {
                    std::cout << color::RED << "[ERROR] No se pudo consultar estado P2P del nodo.\n" << color::RESET;
                } else {
                    auto j = json::parse(res->body);
                    std::cout << color::BOLD << "\n--- ESTADO DE LA RED P2P (PEER-TO-PEER) ---\n" << color::RESET;
                    std::cout << "  Identidad Local:        " << color::CYAN << j.value("node_id", "desconocido") << color::RESET << "\n";
                    std::cout << "  URL de Escucha P2P:     " << j.value("listen_url", "") << "\n";
                    std::cout << "  Pares Conectados:       " << color::GREEN << j.value("peer_count", 0) << color::RESET << "\n";
                    if (j.contains("peers") && j["peers"].is_array() && !j["peers"].empty()) {
                        std::cout << color::DIM << "  ------------------------------------------------------------------------------------\n" << color::RESET;
                        std::cout << "  #   Dirección Endpoint              Altura   Latencia   Estado       ID Remoto\n";
                        std::cout << color::DIM << "  ------------------------------------------------------------------------------------\n" << color::RESET;
                        size_t idx = 1;
                        for (const auto& p : j["peers"]) {
                            std::string st = p.value("is_connected", false) ? (color::GREEN + "ONLINE" + color::RESET) : (color::RED + "OFFLINE" + color::RESET);
                            std::cout << "  " << std::setw(2) << idx++ << "  "
                                      << std::left << std::setw(32) << p.value("address", "") << "  "
                                      << std::setw(8) << p.value("height", 0ULL) << " "
                                      << std::setw(8) << (std::to_string(p.value("latency_ms", 0)) + "ms") << " "
                                      << std::setw(16) << st << " "
                                      << color::GRAY << p.value("node_id", "").substr(0, 16) << "..." << color::RESET << "\n";
                        }
                        std::cout << color::DIM << "  ------------------------------------------------------------------------------------\n\n" << color::RESET;
                    } else {
                        std::cout << color::YELLOW << "  (No hay pares P2P conectados actualmente. Usa 'addpeer <url>' para conectar).\n\n" << color::RESET;
                    }
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "addpeer") {
            if (tokens.size() < 2) {
                std::cout << color::YELLOW << "Uso: addpeer <http://host:puerto>\n" << color::RESET;
                continue;
            }
            std::string peer_url = tokens[1];
            try {
                httplib::Client cli(manager.get_daemon_url().c_str());
                cli.set_connection_timeout(3, 0);
                cli.set_read_timeout(5, 0);
                json body = {{"peer_url", peer_url}};
                auto res = cli.Post("/api/v1/p2p/peers", body.dump(), "application/json");
                if (res && res->status == 200) {
                    std::cout << color::GREEN << "[OK] Par P2P registrado e intento de handshake enviado a: " << peer_url << color::RESET << "\n";
                } else {
                    std::cout << color::RED << "[ERROR] Fallo al agregar par P2P: " << (res ? res->body : "Sin respuesta") << color::RESET << "\n";
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "create") {
            if (tokens.size() < 2) {
                std::cout << color::YELLOW << "Uso: create <nombre_billetera>\n" << color::RESET;
                continue;
            }
            std::string name = tokens[1];
            try {
                std::string mnemonic = manager.create_wallet(name);
                display_mnemonic_box(mnemonic);
                std::cout << color::GREEN << "[OK] Billetera '" << name << "' creada exitosamente.\n" << color::RESET;
                std::cout << "Dirección Stealth (DKSAP): " << color::CYAN << manager.get_address().encode() << color::RESET << "\n\n";

                // Guardar por defecto en archivo
                std::string default_file = name + ".wallet";
                manager.save_wallet(default_file);
                std::cout << color::DIM << "[INFO] Copia de claves guardada automáticamente en: " << default_file << "\n\n" << color::RESET;

                // Intentar sync
                std::string sync_err;
                manager.sync(sync_err);
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "restore") {
            if (tokens.size() < 2) {
                std::cout << color::YELLOW << "Uso: restore <nombre_billetera>\n" << color::RESET;
                continue;
            }
            std::string name = tokens[1];
            std::cout << color::BOLD << "Introduce las 24 palabras separadas por espacios:\n> " << color::RESET;
            std::string input_phrase;
            std::getline(std::cin, input_phrase);

            try {
                manager.restore_wallet(name, input_phrase);
                std::cout << color::GREEN << "[OK] Billetera '" << name << "' restaurada correctamente.\n" << color::RESET;
                std::cout << "Dirección Stealth: " << color::CYAN << manager.get_address().encode() << color::RESET << "\n";

                std::string sync_err;
                std::cout << color::DIM << "[INFO] Escaneando blockchain con View-Key...\n" << color::RESET;
                if (manager.sync(sync_err)) {
                    std::cout << color::GREEN << "[OK] Sincronización completada. Saldo recuperado: " 
                              << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n\n";
                } else {
                    std::cout << color::YELLOW << "[!] Aviso al sincronizar: " << sync_err << "\n\n" << color::RESET;
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] Fallo al restaurar: " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "open") {
            if (tokens.size() < 2) {
                std::cout << color::YELLOW << "Uso: open <ruta.wallet>\n" << color::RESET;
                continue;
            }
            std::string path = tokens[1];
            try {
                manager.load_wallet(path);
                std::cout << color::GREEN << "[OK] Billetera '" << manager.get_name() << "' cargada desde " << path << color::RESET << "\n";
                std::cout << "Dirección Stealth: " << color::CYAN << manager.get_address().encode() << color::RESET << "\n";

                std::string sync_err;
                if (manager.sync(sync_err)) {
                    std::cout << color::GREEN << "[OK] Sincronizado. Saldo: " << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n";
                } else {
                    std::cout << color::YELLOW << "[!] Aviso de sincronización: " << sync_err << color::RESET << "\n";
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] No se pudo abrir archivo: " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "save") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay ninguna billetera activa para guardar.\n" << color::RESET;
                continue;
            }
            std::string path = (tokens.size() > 1) ? tokens[1] : (manager.get_name() + ".wallet");
            try {
                manager.save_wallet(path);
                std::cout << color::GREEN << "[OK] Billetera guardada en: " << path << color::RESET << "\n";
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] Fallo al guardar: " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "close") {
            if (!manager.has_wallet()) {
                std::cout << color::YELLOW << "No hay ninguna billetera abierta.\n" << color::RESET;
            } else {
                std::string name = manager.get_name();
                manager.close_wallet();
                std::cout << color::GREEN << "[OK] Billetera '" << name << "' cerrada y descargada de memoria.\n" << color::RESET;
            }
        } else if (cmd == "address") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa. Usa 'create', 'restore' o 'open'.\n" << color::RESET;
                continue;
            }
            auto addr = manager.get_address();
            std::cout << color::BOLD << "\n--- DIRECCIÓN Y CLAVES PÚBLICAS ---\n" << color::RESET;
            std::cout << "  Dirección Stealth (Base58):  " << color::CYAN << color::BOLD << addr.encode() << color::RESET << "\n";
            std::cout << "  Spend Public Key (A):        " << crypto::to_hex(addr.spend_public_key) << "\n";
            std::cout << "  View Public Key (B):         " << crypto::to_hex(addr.view_public_key) << "\n\n";
        } else if (cmd == "seed") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            display_mnemonic_box(manager.get_mnemonic());
        } else if (cmd == "balance") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            std::string sync_err;
            manager.sync(sync_err);

            std::cout << color::BOLD << "\n--- SALDO Y SALIDAS NO GASTADAS (UTXOS) ---\n" << color::RESET;
            std::cout << "  Saldo Confirmado: " << color::GREEN << color::BOLD << crypto::format_usdt(manager.get_balance()) << color::RESET
                      << " (" << manager.get_balance() << " micro-unidades)\n";
            const auto& utxos = manager.get_utxos();
            std::cout << "  Cantidad de UTXOs propios: " << utxos.size() << "\n";

            if (!utxos.empty()) {
                std::cout << color::DIM << "  ------------------------------------------------------------------------------------------------\n" << color::RESET;
                std::cout << "  #   Monto                 Dirección Efímera (Stealth Pubkey)                Clave Efímera (R)\n";
                std::cout << color::DIM << "  ------------------------------------------------------------------------------------------------\n" << color::RESET;
                for (size_t i = 0; i < utxos.size(); ++i) {
                    std::cout << "  " << std::setw(2) << (i + 1) << "  " 
                              << color::GREEN << std::left << std::setw(20) << crypto::format_usdt(utxos[i].amount) << color::RESET << "  "
                              << color::CYAN << crypto::to_hex(utxos[i].destination_one_time).substr(0, 32) << "..." << color::RESET << "  "
                              << color::GRAY << crypto::to_hex(utxos[i].ephemeral_public_key).substr(0, 32) << "..." << color::RESET << "\n";
                }
                std::cout << color::DIM << "  ------------------------------------------------------------------------------------------------\n\n" << color::RESET;
            } else {
                std::cout << color::YELLOW << "  (No hay salidas disponibles. Puedes acuñar fondos usando 'deposit <monto>').\n\n" << color::RESET;
            }
        } else if (cmd == "sync") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            std::cout << color::DIM << "[INFO] Contactando nodo en " << manager.get_daemon_url() << " con View-Key privada...\n" << color::RESET;
            std::string err;
            if (manager.sync(err)) {
                std::cout << color::GREEN << "[OK] Sincronización exitosa." << color::RESET << "\n";
                std::cout << "  Saldo actual: " << color::GREEN << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n";
                std::cout << "  UTXOs detectados: " << manager.get_utxos().size() << "\n\n";
            } else {
                std::cout << color::RED << "[ERROR] Sincronización falló: " << err << color::RESET << "\n\n";
            }
        } else if (cmd == "deposit") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            if (tokens.size() < 2) {
                std::cout << color::YELLOW << "Uso: deposit <monto_usdt>  (Ejemplo: deposit 100.0)\n" << color::RESET;
                continue;
            }
            try {
                double val = std::stod(tokens[1]);
                crypto::Amount amount = crypto::parse_usdt(val);
                std::cout << color::DIM << "[INFO] Enviando solicitud de depósito de " << crypto::format_usdt(amount) 
                          << " a la bóveda colateral...\n" << color::RESET;

                std::string tx_hash, err;
                if (manager.deposit(amount, tx_hash, err)) {
                    std::cout << color::GREEN << "[OK] Depósito procesado exitosamente.\n" << color::RESET;
                    std::cout << "  Hash de Transacción: " << color::CYAN << tx_hash << color::RESET << "\n";
                    std::cout << "  Nuevo Saldo:         " << color::GREEN << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n\n";
                } else {
                    std::cout << color::RED << "[ERROR] Depósito falló: " << err << color::RESET << "\n\n";
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] Formato de monto inválido: " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "transfer") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            if (tokens.size() < 3) {
                std::cout << color::YELLOW << "Uso: transfer <direccion_stealth_destinatario> <monto_usdt>\n" << color::RESET;
                continue;
            }
            std::string recipient = tokens[1];
            try {
                double val = std::stod(tokens[2]);
                crypto::Amount amount = crypto::parse_usdt(val);

                std::cout << color::DIM << "[INFO] Construyendo transacción RingCT con firmas de anillo MLSAG y señuelos...\n" << color::RESET;
                std::string tx_hash, err;
                if (manager.transfer(recipient, amount, tx_hash, err)) {
                    std::cout << color::GREEN << "[OK] Transferencia privada transmitida con éxito a la blockchain.\n" << color::RESET;
                    std::cout << "  TX Hash:     " << color::CYAN << tx_hash << color::RESET << "\n";
                    std::cout << "  Monto:       " << color::GREEN << crypto::format_usdt(amount) << color::RESET << "\n";
                    std::cout << "  Saldo Restante: " << color::YELLOW << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n\n";
                } else {
                    std::cout << color::RED << "[ERROR] Transferencia falló: " << err << color::RESET << "\n\n";
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] Datos de transferencia inválidos: " << e.what() << color::RESET << "\n";
            }
        } else if (cmd == "withdraw") {
            if (!manager.has_wallet()) {
                std::cout << color::RED << "[ERROR] No hay billetera activa.\n" << color::RESET;
                continue;
            }
            if (tokens.size() < 3) {
                std::cout << color::YELLOW << "Uso: withdraw <monto_usdt> <direccion_publica_usdt>\n"
                          << "Ejemplo: withdraw 50.0 0x71C86576135593833950275819777174bE5b248a\n" << color::RESET;
                continue;
            }
            try {
                double val = std::stod(tokens[1]);
                crypto::Amount amount = crypto::parse_usdt(val);
                std::string public_dest = tokens[2];

                std::cout << color::DIM << "[INFO] Quemando tokens protegidos e iniciando mezclador Micro-Tumbler...\n" << color::RESET;
                std::string order_id, err;
                size_t fragments = 0;
                if (manager.withdraw(amount, public_dest, order_id, fragments, err)) {
                    std::cout << color::GREEN << "[OK] Orden de canje y retiro 1:1 programada exitosamente.\n" << color::RESET;
                    std::cout << "  Orden ID:            " << color::CYAN << order_id << color::RESET << "\n";
                    std::cout << "  Micro-fragmentos:    " << color::YELLOW << fragments << " envíos desvinculados" << color::RESET << "\n";
                    std::cout << "  Destino Público:     " << public_dest << "\n";
                    std::cout << "  Retardo Temporal:    Distribución de Poisson (Time-Jitter anti-análisis)\n";
                    std::cout << "  Saldo Restante:      " << color::GREEN << crypto::format_usdt(manager.get_balance()) << color::RESET << "\n\n";
                } else {
                    std::cout << color::RED << "[ERROR] Retiro falló: " << err << color::RESET << "\n\n";
                }
            } catch (const std::exception& e) {
                std::cout << color::RED << "[ERROR] " << e.what() << color::RESET << "\n";
            }
        } else {
            std::cout << color::YELLOW << "Comando no reconocido: '" << cmd << "'. Escribe 'help' para ver la lista de comandos.\n" << color::RESET;
        }
    }

    return 0;
}
