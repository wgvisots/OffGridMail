// The mailbox: one raw .b2f file per message, in folders per state.
//
//     <root>/in/<MID>.b2f      received
//     <root>/out/<MID>.b2f     waiting to go
//     <root>/sent/<MID>.b2f    delivered
//     <root>/archive/<MID>.b2f filed by the user
//
// The same layout as Pat's mailbox, so either program can read the other's.
// Files are written under a temporary name and renamed, so a crash can never
// leave half a message in a folder.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ogm/common.hpp"

namespace ogm {

class Mailbox {
public:
    enum class Folder { In, Out, Sent, Archive };

    explicit Mailbox(std::filesystem::path root);

    bool save(Folder f, const std::string& mid, ByteView raw);
    std::optional<Bytes> load(Folder f, const std::string& mid) const;
    bool move(const std::string& mid, Folder from, Folder to);
    bool remove(Folder f, const std::string& mid);
    // MIDs in a folder, oldest file first.
    std::vector<std::string> list(Folder f) const;
    // In any folder?  (So a re-offered message is refused, not fetched again.)
    bool has(const std::string& mid) const;
    std::optional<Folder> find(const std::string& mid) const;
    // Last resort when save() fails: write under <root>/rescue/ with a
    // sanitized name.  Returns the path, or empty if even that failed.
    std::filesystem::path rescue(const std::string& mid, ByteView raw);

    const std::filesystem::path& root() const { return root_; }
    static const char* folderName(Folder f);

private:
    std::filesystem::path path(Folder f, const std::string& mid) const;
    static bool safeMid(const std::string& mid);

    std::filesystem::path root_;
};

}  // namespace ogm
