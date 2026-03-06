/*
    srtla_rec - SRT transport proxy with link aggregation
    Stats HTTP server

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <spdlog/spdlog.h>

#include "main.h"
#include "stats_server.h"

extern std::vector<srtla_conn_group_ptr> conn_groups;

static std::atomic<bool> stats_stop_flag{false};
static std::thread stats_thread;

static std::string build_stats_json() {
    uint64_t now_ms = 0;
    get_ms(&now_ms);

    time_t now_s = 0;
    get_seconds(&now_s);

    std::string json = "{\"groups\":[";

    bool first_group = true;
    for (auto &g : conn_groups) {
        if (!first_group) json += ",";
        first_group = false;

        // Format group id as hex of first 8 bytes
        char id_hex[17];
        for (int i = 0; i < 8; i++) {
            snprintf(id_hex + i * 2, 3, "%02x", (unsigned char)g->id[i]);
        }

        uint64_t total_bytes = g->stats_total_bytes.load(std::memory_order_relaxed);

        json += "{\"id\":\"0x";
        json += id_hex;
        json += "\",\"total_bytes\":";
        json += std::to_string(total_bytes);
        json += ",\"connections\":[";

        bool first_conn = true;
        for (auto &c : g->conns) {
            if (!first_conn) json += ",";
            first_conn = false;

            uint64_t bytes = c->stats_bytes.load(std::memory_order_relaxed);
            uint64_t pkts = c->stats_pkts.load(std::memory_order_relaxed);

            double share_pct = 0.0;
            if (total_bytes > 0) {
                share_pct = (double)bytes * 100.0 / (double)total_bytes;
            }

            // last_ms_ago: difference between now and last_rcvd in ms
            // last_rcvd is in seconds (time_t), convert to ms for comparison
            uint64_t last_ms_ago = 0;
            if (c->last_rcvd > 0 && now_ms > 0) {
                uint64_t last_rcvd_ms = (uint64_t)c->last_rcvd * 1000;
                if (now_ms > last_rcvd_ms) {
                    last_ms_ago = now_ms - last_rcvd_ms;
                }
            }

            long uptime_s = 0;
            if (now_s > c->registered_at) {
                uptime_s = (long)(now_s - c->registered_at);
            }

            const char *addr_str = print_addr(&c->addr);
            int port = port_no(&c->addr);

            char share_buf[16];
            snprintf(share_buf, sizeof(share_buf), "%.1f", share_pct);

            json += "{\"addr\":\"";
            json += addr_str;
            json += ":";
            json += std::to_string(port);
            json += "\",\"bytes\":";
            json += std::to_string(bytes);
            json += ",\"pkts\":";
            json += std::to_string(pkts);
            json += ",\"share_pct\":";
            json += share_buf;
            json += ",\"last_ms_ago\":";
            json += std::to_string(last_ms_ago);
            json += ",\"uptime_s\":";
            json += std::to_string(uptime_s);
            json += "}";
        }

        json += "]}";
    }

    json += "]}";
    return json;
}

static void stats_thread_fn(uint16_t port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("Stats server: failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        spdlog::error("Stats server: failed to bind to port {}", port);
        close(server_fd);
        return;
    }

    if (listen(server_fd, 4) < 0) {
        spdlog::error("Stats server: failed to listen");
        close(server_fd);
        return;
    }

    // Set accept timeout to 1 second so we can check the stop flag
    struct timeval to = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

    spdlog::debug("Stats server: listening on port {}", port);

    while (!stats_stop_flag.load(std::memory_order_relaxed)) {
        struct sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            // Timeout or interrupted — just loop
            continue;
        }

        // Read the HTTP request (we just need to consume it)
        char req_buf[2048];
        recv(client_fd, req_buf, sizeof(req_buf), 0);

        // Build the JSON response
        std::string body = build_stats_json();

        std::string response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Connection: close\r\n"
                               "Access-Control-Allow-Origin: *\r\n"
                               "Content-Length: " + std::to_string(body.size()) + "\r\n"
                               "\r\n" + body;

        send(client_fd, response.c_str(), response.size(), 0);
        close(client_fd);
    }

    close(server_fd);
    spdlog::debug("Stats server: stopped");
}

void stats_server_start(uint16_t port) {
    stats_stop_flag.store(false, std::memory_order_relaxed);
    stats_thread = std::thread(stats_thread_fn, port);
    stats_thread.detach();
}

void stats_server_stop() {
    stats_stop_flag.store(true, std::memory_order_relaxed);
}
