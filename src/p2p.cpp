#include "p2p.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;

namespace crypto {

static uint64_t current_timestamp() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

static std::string normalize_url(const std::string& url) {
    if (url.empty()) return "";
    std::string clean = url;
    if (clean.back() == '/') {
        clean.pop_back();
    }
    return clean;
}

P2PManager::P2PManager(Node& node, const std::string& local_listen_url, const std::string& node_id)
    : node_(node), local_listen_url_(normalize_url(local_listen_url)), node_id_(node_id) {
    if (node_id_.empty()) {
        std::array<uint8_t, 8> rand_id;
        randombytes_buf(rand_id.data(), 8);
        node_id_ = "node-" + to_hex(rand_id);
    }
}

P2PManager::~P2PManager() {
    stop();
}

void P2PManager::start() {
    if (is_running_) return;
    is_running_ = true;
    sync_thread_ = std::thread([this]() {
        background_sync_loop();
    });
    std::cout << "[P2P] Servicio de red iniciado. Node ID: " << node_id_ 
              << " | URL de escucha: " << local_listen_url_ << "\n";
}

void P2PManager::stop() {
    if (!is_running_) return;
    is_running_ = false;
    if (sync_thread_.joinable()) {
        sync_thread_.join();
    }
    std::cout << "[P2P] Servicio de red detenido.\n";
}

bool P2PManager::add_peer(const std::string& peer_url) {
    std::string clean_url = normalize_url(peer_url);
    if (clean_url.empty() || clean_url == local_listen_url_) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (peers_.find(clean_url) != peers_.end()) {
            return false; // Ya registrado
        }

        PeerInfo info;
        info.address = clean_url;
        info.is_connected = false;
        info.last_seen_timestamp = 0;
        peers_[clean_url] = info;
    }

    // Intentar negociación inmediata
    return handshake_with_peer(clean_url);
}

void P2PManager::remove_peer(const std::string& peer_url) {
    std::string clean_url = normalize_url(peer_url);
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_.erase(clean_url);
}

std::vector<PeerInfo> P2PManager::get_active_peers() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::vector<PeerInfo> list;
    list.reserve(peers_.size());
    for (const auto& [_, p] : peers_) {
        list.push_back(p);
    }
    return list;
}

std::vector<std::string> P2PManager::get_peer_urls() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    std::vector<std::string> list;
    list.reserve(peers_.size());
    for (const auto& [url, _] : peers_) {
        list.push_back(url);
    }
    return list;
}

size_t P2PManager::get_peer_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peers_.size();
}

bool P2PManager::handshake_with_peer(const std::string& peer_url) {
    std::string clean_url = normalize_url(peer_url);
    if (clean_url.empty() || clean_url == local_listen_url_) return false;

    try {
        httplib::Client cli(clean_url.c_str());
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(5, 0);

        json req = {
            {"node_id", node_id_},
            {"listen_url", local_listen_url_},
            {"height", node_.get_blockchain_height()},
            {"top_hash", to_hex(node_.get_top_block_hash())},
            {"version", 1}
        };

        auto t0 = std::chrono::steady_clock::now();
        auto res = cli.Post("/api/v1/p2p/handshake", req.dump(), "application/json");
        auto t1 = std::chrono::steady_clock::now();

        if (!res || res->status != 200) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = peers_.find(clean_url);
            if (it != peers_.end()) {
                it->second.is_connected = false;
            }
            return false;
        }

        uint32_t latency = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
        );

        auto data = json::parse(res->body);
        std::string remote_id = data.value("node_id", "");
        uint64_t remote_height = data.value("height", 0ULL);
        std::string remote_top_hash_hex = data.value("top_hash", "");

        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto& p = peers_[clean_url];
            p.address = clean_url;
            p.node_id = remote_id;
            p.height = remote_height;
            if (!remote_top_hash_hex.empty()) {
                auto bytes = from_hex(remote_top_hash_hex);
                if (bytes.size() == 32) {
                    std::memcpy(p.top_hash.data(), bytes.data(), 32);
                }
            }
            p.last_seen_timestamp = current_timestamp();
            p.latency_ms = latency;
            p.is_connected = true;
        }

        // Descubrimiento PEX (Peer Exchange)
        if (data.contains("known_peers") && data["known_peers"].is_array()) {
            for (const auto& item : data["known_peers"]) {
                std::string candidate = item.get<std::string>();
                if (candidate != local_listen_url_ && candidate != clean_url) {
                    add_peer(candidate);
                }
            }
        }

        // Sincronización bidireccional inteligente:
        if (remote_height > node_.get_blockchain_height()) {
            // El par remoto tiene una cadena más larga -> Descargar (PULL)
            sync_from_peer(clean_url);
        } else if (remote_height < node_.get_blockchain_height()) {
            // El par remoto está desactualizado (ej. seednode reiniciado o tras caída) -> Empujar (PUSH)
            push_blocks_to_peer(clean_url, remote_height + 1);
        }

        return true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        auto it = peers_.find(clean_url);
        if (it != peers_.end()) {
            it->second.is_connected = false;
        }
        return false;
    }
}

void P2PManager::register_incoming_peer(
    const std::string& peer_url,
    const std::string& node_id,
    uint64_t height,
    const Hash256& top_hash
) {
    std::string clean_url = normalize_url(peer_url);
    if (clean_url.empty() || clean_url == local_listen_url_) return;

    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto& p = peers_[clean_url];
    p.address = clean_url;
    p.node_id = node_id;
    p.height = height;
    p.top_hash = top_hash;
    p.last_seen_timestamp = current_timestamp();
    p.is_connected = true;
}

void P2PManager::broadcast_block(const Block& block, const std::string& skip_peer_url) {
    std::vector<std::string> target_peers;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [url, info] : peers_) {
            if (url != skip_peer_url && info.is_connected) {
                target_peers.push_back(url);
            }
        }
    }

    if (target_peers.empty()) return;

    auto raw_bytes = block.serialize();
    std::string block_hex = to_hex(raw_bytes.data(), raw_bytes.size());
    std::string block_hash_hex = to_hex(block.hash());

    json payload = {
        {"sender_node_id", node_id_},
        {"sender_listen_url", local_listen_url_},
        {"height", block.header.height},
        {"block_hash", block_hash_hex},
        {"block_hex", block_hex}
    };
    std::string payload_str = payload.dump();

    for (const auto& peer_url : target_peers) {
        try {
            httplib::Client cli(peer_url.c_str());
            cli.set_connection_timeout(2, 0);
            cli.set_read_timeout(5, 0);
            auto res = cli.Post("/api/v1/p2p/block", payload_str, "application/json");
            if (res && res->status == 200) {
                std::lock_guard<std::mutex> lock(peers_mutex_);
                auto it = peers_.find(peer_url);
                if (it != peers_.end()) {
                    it->second.height = std::max(it->second.height, block.header.height);
                    it->second.top_hash = block.hash();
                    it->second.last_seen_timestamp = current_timestamp();
                }
            }
        } catch (...) {
            // El fallo de difusión hacia un par no detiene la propagación a otros
        }
    }
}

bool P2PManager::sync_from_peer(const std::string& peer_url) {
    std::string clean_url = normalize_url(peer_url);
    if (clean_url.empty()) return false;

    // Evitar múltiples sincronizaciones concurrentes
    bool expected = false;
    if (!is_syncing_.compare_exchange_strong(expected, true)) {
        return false;
    }

    struct SyncGuard {
        std::atomic<bool>& flag;
        ~SyncGuard() { flag.store(false); }
    } guard{is_syncing_};

    try {
        httplib::Client cli(clean_url.c_str());
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(10, 0);

        while (true) {
            uint64_t current_h = node_.get_blockchain_height();
            std::string query = "/api/v1/p2p/sync?from_height=" + std::to_string(current_h + 1) + "&limit=50";

            auto res = cli.Get(query.c_str());
            if (!res || res->status != 200) {
                return false;
            }

            auto data = json::parse(res->body);
            if (!data.contains("blocks") || !data["blocks"].is_array()) {
                return false;
            }

            auto blocks_array = data["blocks"];
            if (blocks_array.empty()) {
                break; // Sincronizado completamente
            }

            size_t applied_count = 0;
            for (const auto& b_item : blocks_array) {
                std::string hex_str = b_item.at("block_hex").get<std::string>();
                auto bytes = from_hex(hex_str);
                Block blk = Block::deserialize(bytes.data(), bytes.size());

                std::string err;
                if (!node_.apply_remote_block(blk, err)) {
                    std::cerr << "[P2P SYNC ERROR] Rechazo de bloque #" << blk.header.height
                              << " desde " << clean_url << ": " << err << "\n";
                    return false;
                }
                applied_count++;
            }

            std::cout << "[P2P SYNC] " << applied_count << " bloques asimilados desde " 
                      << clean_url << ". Altura actual: " << node_.get_blockchain_height() << "\n";

            if (blocks_array.size() < 50) {
                break; // Último lote procesado
            }
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[P2P SYNC EXCEPTION] " << e.what() << "\n";
        return false;
    }
}

bool P2PManager::push_blocks_to_peer(const std::string& peer_url, uint64_t from_height) {
    std::string clean_url = normalize_url(peer_url);
    if (clean_url.empty()) return false;

    // Evitar múltiples sincronizaciones concurrentes
    bool expected = false;
    if (!is_syncing_.compare_exchange_strong(expected, true)) {
        return false;
    }

    struct SyncGuard {
        std::atomic<bool>& flag;
        ~SyncGuard() { flag.store(false); }
    } guard{is_syncing_};

    try {
        httplib::Client cli(clean_url.c_str());
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(10, 0);

        uint64_t my_height = node_.get_blockchain_height();
        if (from_height > my_height) return true;

        std::cout << "[P2P PUSH] Iniciando envío de bloques #" << from_height 
                  << " -> #" << my_height << " hacia par desactualizado " << clean_url << "...\n";

        size_t pushed_count = 0;
        for (uint64_t h = from_height; h <= my_height; ++h) {
            Block blk;
            if (!node_.get_block(h, blk)) {
                std::cerr << "[P2P PUSH ERROR] No se pudo leer el bloque local #" << h << "\n";
                return false;
            }

            auto raw_bytes = blk.serialize();
            std::string block_hex = to_hex(raw_bytes.data(), raw_bytes.size());
            std::string block_hash_hex = to_hex(blk.hash());

            json payload = {
                {"sender_node_id", node_id_},
                {"sender_listen_url", local_listen_url_},
                {"height", blk.header.height},
                {"block_hash", block_hash_hex},
                {"block_hex", block_hex}
            };

            auto res = cli.Post("/api/v1/p2p/block", payload.dump(), "application/json");
            if (!res || res->status != 200) {
                std::string err_body = res ? res->body : "Timeout / Sin respuesta";
                std::cerr << "[P2P PUSH ERROR] Rechazo de bloque #" << h 
                          << " en " << clean_url << ": " << err_body << "\n";
                return false;
            }
            pushed_count++;
        }

        std::cout << "[P2P PUSH] " << pushed_count << " bloques empujados y asimilados con éxito en " 
                  << clean_url << ".\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[P2P PUSH EXCEPTION] " << e.what() << "\n";
        return false;
    }
}

void P2PManager::background_sync_loop() {
    while (is_running_) {
        // Intervalo de latido: cada 4 segundos
        for (int i = 0; i < 40 && is_running_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!is_running_) break;

        std::vector<std::string> current_peers = get_peer_urls();
        for (const auto& peer_url : current_peers) {
            if (!is_running_) break;
            handshake_with_peer(peer_url);
        }
    }
}

} // namespace crypto
