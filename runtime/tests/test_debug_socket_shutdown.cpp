#include "debug_socket_io.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include <cstdio>
#include <string>

using namespace std::chrono_literals;
static int running(void *ctx) { return static_cast<std::atomic<int> *>(ctx)->load(); }
static PsxDebugSocket listener(unsigned short *port) {
    auto s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s != PSX_DEBUG_SOCKET_INVALID);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
    assert(listen(s, 16) == 0);
    assert(psx_debug_socket_nonblocking(s, 1));
#ifdef _WIN32
    int size = sizeof addr;
#else
    socklen_t size = sizeof addr;
#endif
    assert(getsockname(s, reinterpret_cast<sockaddr *>(&addr), &size) == 0);
    *port = ntohs(addr.sin_port);
    return s;
}
static PsxDebugSocket connect_to(unsigned short port) {
    auto s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    assert(connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
    return s;
}
int main() {
#ifdef _WIN32
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    for (int cycle = 0; cycle < 3; ++cycle) {
        unsigned short port;
        auto ls = listener(&port);
        std::atomic<int> live{1};
        auto pending = std::async(std::launch::async, [&] {
            return psx_debug_socket_accept(ls, running, &live);
        });
        assert(pending.wait_for(350ms) == std::future_status::timeout);
        live = 0;
        assert(pending.wait_for(1500ms) == std::future_status::ready);
        assert(pending.get() == PSX_DEBUG_SOCKET_INVALID);
        psx_debug_socket_close(ls);
    }
    for (int partial = 0; partial < 2; ++partial) {
        unsigned short port;
        auto ls = listener(&port);
        std::atomic<int> live{1};
        auto peer = connect_to(port);
        auto worker = std::async(std::launch::async, [&] {
            auto client = psx_debug_socket_accept(ls, running, &live);
            assert(client != PSX_DEBUG_SOCKET_INVALID);
            char buf[128];
            int n = psx_debug_socket_recv_line(client, buf, sizeof buf, running, &live);
            psx_debug_socket_close(client);
            return n;
        });
        if (partial) assert(send(peer, "{\"cmd\":", 7, 0) == 7);
        assert(worker.wait_for(350ms) == std::future_status::timeout);
        live = 0;
        assert(worker.wait_for(1500ms) == std::future_status::ready);
        assert(worker.get() == -1);
        psx_debug_socket_close(peer);
        psx_debug_socket_close(ls);
    }
    unsigned short port;
    auto ls = listener(&port);
    std::atomic<int> live{1};
    auto peer = connect_to(port);
    auto worker = std::async(std::launch::async, [&] {
        auto client = psx_debug_socket_accept(ls, running, &live);
        char buf[128];
        int n = psx_debug_socket_recv_line(client, buf, sizeof buf, running, &live);
        assert(n == 14 && std::string(buf) == "{\"cmd\":\"ping\"}");
        assert(send(client, "pong\n", 5, 0) == 5);
        psx_debug_socket_close(client);
    });
    std::this_thread::sleep_for(350ms);
    assert(send(peer, "{\"cmd\":", 7, 0) == 7);
    std::this_thread::sleep_for(350ms);
    assert(send(peer, "\"ping\"}\n", 8, 0) == 8);
    assert(worker.wait_for(1500ms) == std::future_status::ready);
    worker.get();
    char pong[8]{};
    assert(recv(peer, pong, sizeof pong, 0) == 5);
    assert(std::string(pong) == "pong\n");
    psx_debug_socket_close(peer);
    psx_debug_socket_close(ls);
#ifdef _WIN32
    WSACleanup();
#endif
    puts("debug socket: idle/partial shutdown, delayed fragments, response and repeated initialization PASS");
}
