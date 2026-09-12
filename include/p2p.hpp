#pragma once

#include "types.hpp"
#include "block.hpp"
#include "node.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

namespace crypto {

struct PeerInfo {
    std::string node_id;
    std::string address; // e.g. "http://192.168.1.50:8080"
    uint64_t height{0};
    Hash256 top_hash{};
    uint64_t last_seen_timestamp{0};
    uint32_t latency_ms{0};
    bool is_connected{false};
};

class P2PManager {
public:
    P2PManager(Node& node, const std::string& local_listen_url, const std::string& node_id = "");
    ~P2PManager();

    // Iniciar y detener el hilo de mantenimiento y sincronización P2P
    void start();
    void stop();
    bool is_running() const { return is_running_; }

    // Gestión de pares
    bool add_peer(const std::string& peer_url);
    void remove_peer(const std::string& peer_url);
    std::vector<PeerInfo> get_active_peers() const;
    std::vector<std::string> get_peer_urls() const;
    size_t get_peer_count() const;

    // Negociación y Handshake
    bool handshake_with_peer(const std::string& peer_url);
    void register_incoming_peer(
        const std::string& peer_url,
        const std::string& node_id,
        uint64_t height,
        const Hash256& top_hash
    );

    // Difusión Gossip de bloques
    void broadcast_block(const Block& block, const std::string& skip_peer_url = "");

    // Sincronización rápida (Initial Block Download - IBD)
    bool sync_from_peer(const std::string& peer_url);

    // Sincronización proactiva hacia un par atrasado (Push Sync)
    bool push_blocks_to_peer(const std::string& peer_url, uint64_t from_height);

    // Consulta de identidad local
    const std::string& get_node_id() const { return node_id_; }
    const std::string& get_local_listen_url() const { return local_listen_url_; }
    void set_local_listen_url(const std::string& url) { local_listen_url_ = url; }

private:
    void background_sync_loop();
    bool ping_peer(const std::string& peer_url, PeerInfo& info);
    void peer_exchange(const std::string& peer_url);

    Node& node_;
    std::string local_listen_url_;
    std::string node_id_;

    mutable std::mutex peers_mutex_;
    std::unordered_map<std::string, PeerInfo> peers_;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_syncing_{false};
    std::thread sync_thread_;
};

} // namespace crypto
