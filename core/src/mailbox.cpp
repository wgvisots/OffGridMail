#include "ogm/mailbox.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace ogm {

const char* Mailbox::folderName(Folder f) {
    switch (f) {
        case Folder::In: return "in";
        case Folder::Out: return "out";
        case Folder::Sent: return "sent";
        case Folder::Archive: return "archive";
    }
    return "?";
}

Mailbox::Mailbox(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    for (auto f : {Folder::In, Folder::Out, Folder::Sent, Folder::Archive}) fs::create_directories(root_ / folderName(f), ec);
}

// MIDs come off the air: never let one name a path outside the mailbox.
// The CMS uses IDs like "NZ5R#$29RI1R", so allow printable ASCII except path
// separators, and no leading dot.
bool Mailbox::safeMid(const std::string& mid) {
    if (mid.empty() || mid.size() > 32 || mid.front() == '.') return false;
    return std::all_of(mid.begin(), mid.end(), [](char c) {
        auto u = static_cast<unsigned char>(c);
        return u > 0x20 && u < 0x7F && c != '/' && c != '\\';
    });
}

std::filesystem::path Mailbox::rescue(const std::string& mid, ByteView raw) {
    std::error_code ec;
    fs::create_directories(root_ / "rescue", ec);
    std::string safe;
    for (char c : mid) safe += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    for (int n = 0; n < 1000; ++n) {
        fs::path p = root_ / "rescue" / (safe + (n ? "-" + std::to_string(n) : std::string()) + ".b2f");
        if (fs::exists(p, ec)) continue;
        std::ofstream out(p, std::ios::binary);
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        if (out) return p;
    }
    return {};
}

fs::path Mailbox::path(Folder f, const std::string& mid) const { return root_ / folderName(f) / (mid + ".b2f"); }

bool Mailbox::save(Folder f, const std::string& mid, ByteView raw) {
    if (!safeMid(mid)) return false;
    fs::path final = path(f, mid);
    fs::path tmp = final;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, final, ec);
    return !ec;
}

std::optional<Bytes> Mailbox::load(Folder f, const std::string& mid) const {
    if (!safeMid(mid)) return std::nullopt;
    std::ifstream in(path(f, mid), std::ios::binary);
    if (!in) return std::nullopt;
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

bool Mailbox::move(const std::string& mid, Folder from, Folder to) {
    if (!safeMid(mid)) return false;
    std::error_code ec;
    fs::rename(path(from, mid), path(to, mid), ec);
    return !ec;
}

bool Mailbox::remove(Folder f, const std::string& mid) {
    if (!safeMid(mid)) return false;
    std::error_code ec;
    return fs::remove(path(f, mid), ec);
}

std::vector<std::string> Mailbox::list(Folder f) const {
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root_ / folderName(f), ec)) {
        if (!e.is_regular_file() || e.path().extension() != ".b2f") continue;
        found.emplace_back(e.last_write_time(ec), e.path().stem().string());
    }
    std::sort(found.begin(), found.end());
    std::vector<std::string> out;
    for (auto& [t, mid] : found) out.push_back(std::move(mid));
    return out;
}

std::optional<Mailbox::Folder> Mailbox::find(const std::string& mid) const {
    if (!safeMid(mid)) return std::nullopt;
    std::error_code ec;
    for (auto f : {Folder::In, Folder::Out, Folder::Sent, Folder::Archive})
        if (fs::exists(path(f, mid), ec)) return f;
    return std::nullopt;
}

bool Mailbox::has(const std::string& mid) const { return find(mid).has_value(); }

}  // namespace ogm
