#include <asr/backend/websocket.hpp>
#include <asr/engines/prefix_engine.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 6) {
        std::cerr << "Usage: asr-prefix-server MODEL_DIR PORT [MAX_CALLS=2] [PREVIEW_MS=4000] [THREADS=4]\n";
        return 2;
    }
    try {
        const int port = std::stoi(argv[2]);
        if (port < 0 || port > 65535) throw std::invalid_argument("port must be 0..65535");
        const int max_calls = argc > 3 ? std::stoi(argv[3]) : 2;
        const int preview_ms = argc > 4 ? std::stoi(argv[4]) : 4000;
        const int threads = argc > 5 ? std::stoi(argv[5]) : 4;
        asr::PrefixMultiplexEngine engine(argv[1], max_calls, preview_ms, threads);
        asr::WebSocketServer server(engine, static_cast<std::uint16_t>(port));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::cout << "{\"host\":\"127.0.0.1\",\"port\":" << server.port()
                  << ",\"path\":\"/v1/asr\",\"experimental\":true"
                  << ",\"max_calls\":" << max_calls
                  << ",\"preview_ms\":" << preview_ms
                  << ",\"threads\":" << threads << "}" << std::endl;
        while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
