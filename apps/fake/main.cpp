// ogmfake - a pretend Winlink RMS on a TCP port, for trying ogm and the
// apps without a radio or the internet.
//
//   ogmfake                          AX.25 over KISS on 127.0.0.1:8001, as W1AW-10
//   ogmfake --telnet --port 8772     CMS-style Telnet login instead
//   ogmfake --password pw --drop 6   secure login; lose every 6th frame
//
// Messages in its mailbox's out/ folder are offered to whoever connects, and
// what it receives lands in in/.  To queue one for your station:
//   ogm --call W1AW --mailbox /tmp/ogmfake compose --to N0CALL --subject Hi
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#include "fake_rms.hpp"

using namespace ogm;

namespace {
TimeMs nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

int main(int argc, char** argv) {
    sim::FakeRms::Config cfg;
    int port = 0;
    std::string mailboxDir = (std::filesystem::temp_directory_path() / "ogmfake").string();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--telnet") cfg.telnet = true;
        else if (a == "--call") cfg.call = next();
        else if (a == "--password") cfg.password = next();
        else if (a == "--drop") cfg.dropEvery = std::atoi(next().c_str());
        else if (a == "--mailbox") mailboxDir = next();
        else {
            std::cerr << "usage: ogmfake [--telnet] [--port N] [--call W1AW-10] [--password PW] [--drop N] [--mailbox DIR]\n";
            return 2;
        }
    }
    if (port == 0) port = cfg.telnet ? 8772 : 8001;
    Mailbox mailbox(mailboxDir);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(srv, 1) < 0) {
        std::cerr << "cannot listen on port " << port << ": " << std::strerror(errno) << "\n";
        return 1;
    }
    std::cerr << "ogmfake: " << cfg.call << (cfg.telnet ? " (Telnet)" : " (AX.25 over KISS)") << " on 127.0.0.1:" << port
              << ", mailbox " << mailboxDir << "\n";

    sim::FakeRms rms(cfg, mailbox);
    int client = -1;
    for (;;) {
        pollfd fds[2] = {{srv, POLLIN, 0}, {client, POLLIN, 0}};
        poll(fds, 2, 100);
        TimeMs now = nowMs();
        if (fds[0].revents & POLLIN) {
            int c = accept(srv, nullptr, nullptr);
            if (client >= 0) {
                close(c);  // one at a time, like a radio
            } else {
                client = c;
                std::cerr << "client connected\n";
                rms.onClientConnected(now);
            }
        }
        if (client >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            std::uint8_t buf[4096];
            ssize_t n = read(client, buf, sizeof buf);
            if (n <= 0) {
                std::cerr << "client left\n";
                close(client);
                client = -1;
            } else {
                rms.onBytes(ByteView(buf, static_cast<std::size_t>(n)), now);
            }
        }
        rms.tick(now);
        Bytes out = rms.takeOut();
        if (client >= 0 && !out.empty()) {
            if (write(client, out.data(), out.size()) < 0) std::cerr << "write failed\n";
        }
        for (const auto& line : rms.takeLog()) std::cerr << "  " << line << "\n";
        if (client >= 0 && rms.wantsHangup()) {
            std::cerr << "hanging up\n";
            close(client);
            client = -1;
            rms.onClientConnected(now);  // reset for the next caller
            rms.takeOut();               // ...without sending a prompt to nobody
        }
    }
}
