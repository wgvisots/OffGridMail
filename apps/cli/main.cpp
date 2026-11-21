// ogm - Winlink email from a terminal (Linux), on top of libwlecore.
//
//   ogm compose --to friend@example.com --subject "Hi" < body.txt
//   ogm list [in|out|sent|archive]
//   ogm read MID
//   ogm connect d75 28:3C:90:77:11:DF W1AW-10        # TH-D75 over Bluetooth
//   ogm connect kiss-tcp 127.0.0.1:8001 W1AW-10      # Direwolf, or ogmfake
//   ogm connect telnet                               # the Winlink CMS, no radio
//
// Settings come from ~/.config/ogm/config (call=, grid=, mailbox=), the
// OGM_CALL environment variable, or flags (--call, --grid, --mailbox).
// The Winlink password comes from OGM_PASSWORD, the desktop keyring
// (secret-tool, service "ogm", account <CALL>), ~/.config/ogm/password-<CALL>
// (mode 600), or is asked for when needed.
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#if __has_include(<bluetooth/rfcomm.h>)
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>
#define OGM_HAVE_BLUEZ 1
#endif

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ogm/engine.hpp"
#include "ogm/radio.hpp"

using namespace ogm;
namespace fs = std::filesystem;

namespace {

volatile sig_atomic_t gInterrupts = 0;
void onSigint(int) { gInterrupts = gInterrupts + 1; }

TimeMs nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::int64_t unixNow() { return static_cast<std::int64_t>(std::time(nullptr)); }

int usage() {
    std::cerr <<
        R"(usage: ogm [--call CALL] [--grid GRID] [--mailbox DIR] COMMAND ...

  compose --to ADDR [--to ADDR] [--cc ADDR] --subject TEXT [--attach FILE]...
                         body is read from standard input
  list [in|out|sent|archive]
  read MID
  connect telnet [HOST:PORT]                  Winlink CMS (default server.winlink.org:8772)
  connect kiss-tcp HOST:PORT GATEWAY          a KISS TNC on TCP (Direwolf, ogmfake)
  connect d75 MAC GATEWAY [--channel N]       TH-D75 over Bluetooth (channel 2 by default)

connect options:
  --via DIGI[,DIGI]   digipeaters on the way to the gateway
  --paclen N          bytes per AX.25 frame (128)
  --maxframe N        frames per burst (4)
  --monitor           show every AX.25 frame
  --verbose           show the B2F protocol lines

Addresses: a callsign (W1AW), a Winlink address (w1aw@winlink.org) or any
internet address (friend@example.com).
)";
    return 2;
}

std::map<std::string, std::string> readConfig() {
    std::map<std::string, std::string> cfg;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path p = xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config";
    std::ifstream in(p / "ogm" / "config");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
            return s;
        };
        cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return cfg;
}

fs::path defaultMailbox(const std::string& call) {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".local" / "share";
    return base / "ogm" / "mailbox" / call;
}

fs::path configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    return (xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config") / "ogm";
}

// The GNOME/KDE keyring through libsecret's secret-tool, if it is installed:
//   secret-tool store --label "Winlink NZ5R" service ogm account NZ5R
std::string keyringPassword(const std::string& call) {
    if (std::system("command -v secret-tool >/dev/null 2>&1") != 0) return {};
    std::string cmd = "secret-tool lookup service ogm account '" + call + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[256];
    while (std::fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// ~/.config/ogm/password-<CALL>, readable by its owner only.
std::string filePassword(const std::string& call) {
    fs::path f = configDir() / ("password-" + call);
    std::error_code ec;
    auto st = fs::status(f, ec);
    if (ec || !fs::is_regular_file(st)) return {};
    auto perms = st.permissions();
    if ((perms & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none) {
        std::cerr << "ignoring " << f << ": other users can read it (chmod 600 " << f << ")\n";
        return {};
    }
    std::ifstream in(f);
    std::string pw;
    std::getline(in, pw);
    while (!pw.empty() && (pw.back() == '\r' || pw.back() == ' ')) pw.pop_back();
    return pw;
}

// OGM_PASSWORD, then the keyring, then the password file, then ask.
std::string askPassword(const std::string& call) {
    if (const char* p = std::getenv("OGM_PASSWORD")) return p;
    if (auto pw = keyringPassword(call); !pw.empty()) return pw;
    if (auto pw = filePassword(call); !pw.empty()) return pw;
    if (!isatty(0)) return {};
    std::cerr << "Winlink password for " << call << " (Enter for none): " << std::flush;
    termios old{};
    tcgetattr(0, &old);
    termios quiet = old;
    quiet.c_lflag &= static_cast<tcflag_t>(~ECHO);
    tcsetattr(0, TCSANOW, &quiet);
    std::string pw;
    std::getline(std::cin, pw);
    tcsetattr(0, TCSANOW, &old);
    std::cerr << "\n";
    return pw;
}

int connectTcp(const std::string& hostPort, std::string* err) {
    auto colon = hostPort.rfind(':');
    std::string host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
    std::string port = colon == std::string::npos ? "8001" : hostPort.substr(colon + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        *err = gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (auto* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        *err = std::strerror(errno);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

#ifdef OGM_HAVE_BLUEZ
int connectRfcomm(const std::string& mac, int channel, std::string* err) {
    sockaddr_rc addr{};
    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = static_cast<std::uint8_t>(channel);
    unsigned b[6];
    if (std::sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        *err = "not a Bluetooth address: " + mac;
        return -1;
    }
    for (int i = 0; i < 6; ++i) addr.rc_bdaddr.b[5 - i] = static_cast<std::uint8_t>(b[i]);  // BlueZ stores it reversed
    for (int attempt = 0; attempt < 4; ++attempt) {
        int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (fd < 0) break;
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0) return fd;
        int e = errno;
        close(fd);
        *err = std::strerror(e);
        if (e != EBUSY) return -1;
        sleep(2);  // the radio is still releasing a previous link
    }
    return -1;
}
#endif

bool writeAll(int fd, const Bytes& b) {
    std::size_t off = 0;
    while (off < b.size()) {
        ssize_t n = ::write(fd, b.data() + off, b.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

std::string clockNow() {
    std::time_t t = std::time(nullptr);
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    return buf;
}

int runSession(Engine& engine, std::unique_ptr<Channel> channel, int fd, bool verbose) {
    signal(SIGINT, onSigint);
    TimeMs t0 = nowMs();
    engine.startSession(std::move(channel), t0);
    engine.onLowerUp(t0);
    bool ok = false;
    bool aborted = false;
    std::string lastProgress;
    for (;;) {
        TimeMs now = nowMs();
        if (gInterrupts == 1 && !aborted) {
            aborted = true;
            std::cerr << clockNow() << "  stopping (Ctrl-C again to quit at once)\n";
            engine.abortSession(now);
        } else if (gInterrupts > 1) {
            std::cerr << "quit\n";
            break;
        }
        pollfd pfd{fd, POLLIN, 0};
        int rc = poll(&pfd, 1, 100);
        now = nowMs();
        if (rc > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            std::uint8_t buf[4096];
            ssize_t n = ::read(fd, buf, sizeof buf);
            if (n > 0) {
                engine.onLowerBytes(ByteView(buf, static_cast<std::size_t>(n)), now);
            } else if (n == 0 || errno != EINTR) {
                engine.onLowerDown(now);
            }
        }
        engine.tick(now);
        if (auto out = engine.takeLowerOut(); !out.empty()) {
            if (!writeAll(fd, out)) engine.onLowerDown(now);
        }
        for (const auto& e : engine.takeEvents()) {
            switch (e.kind) {
                case Engine::Event::Kind::Status: std::cerr << clockNow() << "  " << e.text << "\n"; break;
                case Engine::Event::Kind::Log:
                    if (verbose) std::cerr << clockNow() << "    " << e.text << "\n";
                    break;
                case Engine::Event::Kind::Received:
                    std::cerr << clockNow() << "  received " << e.mid << "  " << e.text << "\n";
                    break;
                case Engine::Event::Kind::Sent: std::cerr << clockNow() << "  sent " << e.mid << "\n"; break;
                case Engine::Event::Kind::AccountActivated:
                    std::cerr << "\n  ************************************************************\n"
                              << "  Your new Winlink account is active.\n";
                    if (!e.text.empty())
                        std::cerr << "  Password: " << e.text << "\n"
                                  << "  Write it down now.  From the next connection on, use it\n"
                                  << "  (OGM_PASSWORD=" << e.text << " or type it when asked).\n";
                    else
                        std::cerr << "  The password is in message " << e.mid << ": ogm read " << e.mid << "\n";
                    std::cerr << "  Then log in at winlink.org (callsign + password) and set a\n"
                              << "  password recovery email, or the password can't be recovered.\n"
                              << "  ************************************************************\n\n";
                    break;
                case Engine::Event::Kind::Progress: {
                    std::string p = e.mid + " " + std::to_string(e.done) + "/" + std::to_string(e.total);
                    if (p != lastProgress) std::cerr << clockNow() << "    receiving " << p << " bytes\n";
                    lastProgress = p;
                    break;
                }
                case Engine::Event::Kind::SessionEnded:
                    ok = e.ok;
                    std::cerr << clockNow() << "  " << (e.ok ? "" : "failed: ") << e.text << "\n";
                    break;
            }
        }
        if (engine.lowerDone() && !engine.sessionActive()) break;
    }
    if (auto out = engine.takeLowerOut(); !out.empty()) writeAll(fd, out);
    close(fd);
    return ok ? 0 : 1;
}

Bytes readAll(std::istream& in) { return Bytes(std::istreambuf_iterator<char>(in), {}); }

std::string summaryLine(const Mailbox& mb, Mailbox::Folder f, const std::string& mid) {
    auto raw = mb.load(f, mid);
    if (!raw) return mid + "  (unreadable)";
    auto m = Message::parse(*raw);
    if (!m) return mid + "  (unreadable)";
    std::string who = f == Mailbox::Folder::In ? m->from() : (m->to().empty() ? "" : m->to()[0]);
    if (who.rfind("SMTP:", 0) == 0) who = who.substr(5);
    char buf[256];
    std::snprintf(buf, sizeof buf, "%-12s  %s  %-28.28s  %s%s", mid.c_str(), m->date().c_str(), who.c_str(),
                  m->subject().c_str(), m->files().empty() ? "" : "  [+]");
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    auto cfg = readConfig();
    std::string call = cfg.count("call") ? cfg["call"] : "";
    if (const char* e = std::getenv("OGM_CALL")) call = e;
    std::string grid = cfg.count("grid") ? cfg["grid"] : "";
    std::string mailboxDir = cfg.count("mailbox") ? cfg["mailbox"] : "";
    std::vector<std::string> to, cc, attach, rest;
    std::string subject, via;
    int channel = 2;
    std::size_t paclen = 128, maxframe = 4;
    bool monitor = false, verbose = false, haveSubject = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::cerr << a << " needs a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--call") call = next();
        else if (a == "--grid") grid = next();
        else if (a == "--mailbox") mailboxDir = next();
        else if (a == "--to") to.push_back(next());
        else if (a == "--cc") cc.push_back(next());
        else if (a == "--subject") {
            subject = next();
            haveSubject = true;
        } else if (a == "--attach") attach.push_back(next());
        else if (a == "--via") via = next();
        else if (a == "--channel") channel = std::atoi(next().c_str());
        else if (a == "--paclen") paclen = static_cast<std::size_t>(std::atoi(next().c_str()));
        else if (a == "--maxframe") maxframe = static_cast<std::size_t>(std::atoi(next().c_str()));
        else if (a == "--monitor") monitor = true;
        else if (a == "--verbose" || a == "-v") verbose = true;
        else if (a == "--help" || a == "-h") return usage();
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "unknown option " << a << "\n";
            return usage();
        } else rest.push_back(a);
    }
    if (rest.empty()) return usage();
    for (char& c : call) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (call.empty()) {
        std::cerr << "set your callsign: --call N0CALL, OGM_CALL, or call= in ~/.config/ogm/config\n";
        return 2;
    }
    auto dash = call.find('-');
    std::string account = call.substr(0, dash);  // the Winlink account has no SSID
    Mailbox mailbox(mailboxDir.empty() ? defaultMailbox(account) : fs::path(mailboxDir));
    const std::string& cmd = rest[0];

    if (cmd == "compose") {
        if (to.empty() && cc.empty()) {
            std::cerr << "compose needs at least one --to\n";
            return 2;
        }
        if (!haveSubject) {
            std::cerr << "compose needs --subject\n";
            return 2;
        }
        if (isatty(0)) std::cerr << "Type the message, then Ctrl-D on a line of its own:\n";
        Bytes body = readAll(std::cin);
        Message::Draft d;
        d.from = account;
        d.to = to;
        d.cc = cc;
        d.subject = subject;
        d.body.assign(body.begin(), body.end());
        for (const auto& path : attach) {
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                std::cerr << "cannot read " << path << "\n";
                return 1;
            }
            d.files.push_back({fs::path(path).filename().string(), readAll(f)});
        }
        Engine engine({account, "", grid}, mailbox);
        std::string err;
        std::string mid = engine.compose(d, unixNow(), &err);
        if (mid.empty()) {
            std::cerr << "not saved: " << err << "\n";
            return 1;
        }
        auto raw = mailbox.load(Mailbox::Folder::Out, mid);
        std::cout << mid << "  queued (" << (raw ? raw->size() : 0) << " bytes)\n";
        return 0;
    }

    if (cmd == "list") {
        Mailbox::Folder f = Mailbox::Folder::In;
        std::string which = rest.size() > 1 ? rest[1] : "in";
        if (which == "out") f = Mailbox::Folder::Out;
        else if (which == "sent") f = Mailbox::Folder::Sent;
        else if (which == "archive") f = Mailbox::Folder::Archive;
        else if (which != "in") return usage();
        auto mids = mailbox.list(f);
        if (mids.empty()) std::cout << "(" << which << " is empty)\n";
        for (const auto& mid : mids) std::cout << summaryLine(mailbox, f, mid) << "\n";
        return 0;
    }

    if (cmd == "read") {
        if (rest.size() < 2) return usage();
        auto where = mailbox.find(rest[1]);
        if (!where) {
            std::cerr << "no message " << rest[1] << "\n";
            return 1;
        }
        auto m = Message::parse(*mailbox.load(*where, rest[1]));
        if (!m) {
            std::cerr << "message " << rest[1] << " is unreadable\n";
            return 1;
        }
        std::cout << "From:    " << m->from() << "\n";
        for (const auto& t : m->to()) std::cout << "To:      " << t << "\n";
        for (const auto& t : m->cc()) std::cout << "Cc:      " << t << "\n";
        std::cout << "Date:    " << m->date() << " UTC\nSubject: " << m->subject() << "\n\n";
        std::string body = m->bodyText();
        for (char c : body)
            if (c != '\r') std::cout << c;
        if (!body.empty() && body.back() != '\n') std::cout << "\n";
        for (const auto& f : m->files()) std::cout << "\n[attachment] " << f.name << " (" << f.data.size() << " bytes)\n";
        return 0;
    }

    if (cmd == "connect") {
        if (rest.size() < 2) return usage();
        const std::string& how = rest[1];
        std::string err;
        int fd = -1;
        std::unique_ptr<Channel> ch;
        if (how == "telnet") {
            std::string hostPort = rest.size() > 2 ? rest[2] : "server.winlink.org:8772";
            std::cerr << clockNow() << "  connecting to " << hostPort << "\n";
            fd = connectTcp(hostPort, &err);
            TelnetChannel::Config tc;
            tc.mycall = call;
            ch = std::make_unique<TelnetChannel>(tc);
        } else if (how == "kiss-tcp" || how == "d75") {
            if (rest.size() < 4) return usage();
            auto target = ax25::Address::parse(rest[3]);
            auto path = ax25::parsePath(via);
            auto me = ax25::Address::parse(call);
            if (!target || !path || !me) {
                std::cerr << "bad callsign in " << rest[3] << " / " << call << " / " << via << "\n";
                return 2;
            }
            Ax25Channel::Config ac;
            ac.link.mycall = *me;
            ac.link.paclen = paclen;
            ac.link.maxframe = maxframe;
            ac.target = *target;
            ac.via = *path;
            ac.monitor = monitor;
            std::unique_ptr<radio::KissTnc> tnc;
            if (how == "d75") {
#ifdef OGM_HAVE_BLUEZ
                std::cerr << clockNow() << "  connecting to the radio at " << rest[2] << "\n";
                fd = connectRfcomm(rest[2], channel, &err);
                tnc = std::make_unique<radio::ThD75>();
#else
                std::cerr << "built without BlueZ: use kiss-tcp, or a serial port bridged to TCP\n";
                return 1;
#endif
            } else {
                std::cerr << clockNow() << "  connecting to the TNC at " << rest[2] << "\n";
                fd = connectTcp(rest[2], &err);
                tnc = std::make_unique<radio::PlainKiss>();
            }
            ch = std::make_unique<Ax25Channel>(ac, std::move(tnc));
        } else {
            return usage();
        }
        if (fd < 0) {
            std::cerr << "cannot connect: " << err << "\n";
            return 1;
        }
        Engine engine({account, askPassword(account), grid}, mailbox);
        std::size_t waiting = mailbox.list(Mailbox::Folder::Out).size();
        std::cerr << clockNow() << "  " << waiting << " message" << (waiting == 1 ? "" : "s") << " in the outbox\n";
        return runSession(engine, std::move(ch), fd, verbose);
    }
    return usage();
}
