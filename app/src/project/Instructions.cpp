#include "project/Instructions.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
const char* const kInstructionsFile = "CLAUDE.md";

// "<what> <path>: <reason>", the reason taken from errno when it is set.
std::string describe(const std::string& what, const fs::path& path, int err)
{
    std::string message = what + " " + path.string();
    if (err != 0)
        message += ": " + std::string(std::strerror(err));
    return message;
}

// `path` with every symlink in its last component followed, also when the
// final target does not exist yet. The result names the same file the OS
// opens through `path`. Gives up after a bounded number of links,
// as the OS does for a loop.
fs::path resolveLinks(fs::path path)
{
    constexpr int kMaxLinks = 40;
    std::error_code ec;
    for (int i = 0; i < kMaxLinks && fs::is_symlink(path, ec); ++i) {
        const fs::path target = fs::read_symlink(path, ec);
        if (ec)
            break;
        if (target.is_absolute()) {
            path = target;
            continue;
        }
        // A relative target is relative to the link's real folder. Joining
        // it there, without collapsing "..", resolves it the way the OS
        // does when load opens the link, also through a symlinked folder.
        std::error_code parentEc;
        const fs::path parent = fs::canonical(path.parent_path(), parentEc);
        path = (parentEc ? path.parent_path() : parent) / target;
    }
    return path;
}

// A name for the temporary file next to `path` that no file has yet.
fs::path uniqueTmpPath(const fs::path& path)
{
    thread_local std::mt19937_64 random{std::random_device{}()};
    std::error_code ec;
    for (;;) {
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, ".%016llx.tmp",
                      static_cast<unsigned long long>(random()));
        fs::path tmp = path;
        tmp += suffix;
        if (!fs::exists(fs::symlink_status(tmp, ec)))
            return tmp;
    }
}
} // namespace

fs::path instructionsPath(const fs::path& root)
{
    return root / kInstructionsFile;
}

bool loadInstructions(const fs::path& root, std::string& text, std::string& error)
{
    text.clear();
    error.clear();
    const fs::path path = instructionsPath(root);
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found)
        return true;
    if (ec) {
        error = "cannot read " + path.string() + ": " + ec.message();
        return false;
    }
    if (status.type() != fs::file_type::regular) {
        error = "cannot read " + path.string() + ": not a regular file";
        return false;
    }

    errno = 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = describe("cannot read", path, errno);
        return false;
    }
    std::string content{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (in.bad()) {
        error = describe("cannot read", path, errno);
        return false;
    }
    text = std::move(content);
    return true;
}

bool saveInstructions(const fs::path& root, const std::string& text, std::string& error)
{
    error.clear();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        error = "cannot write instructions: " + root.string() + " is not a folder";
        return false;
    }
    // A symlinked CLAUDE.md stays a link: the new text replaces its target.
    const fs::path path = resolveLinks(instructionsPath(root));
    // Renaming a file over a directory fails on some systems only after the
    // temporary file exists. Check first so nothing is written.
    if (fs::is_directory(path, ec)) {
        error = "cannot write " + path.string() + ": it is a folder";
        return false;
    }

    const fs::path tmp = uniqueTmpPath(path);
    std::error_code ignored;
    {
        errno = 0;
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out)
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (out)
            out.close();
        if (!out) {
            error = describe("cannot write", tmp, errno);
            out.close();
            fs::remove(tmp, ignored);
            return false;
        }
    }
    // The new file keeps the old one's permissions, e.g. 0600.
    const fs::file_status old = fs::status(path, ec);
    if (!ec && fs::is_regular_file(old)) {
        fs::permissions(tmp, old.permissions(), fs::perm_options::replace, ec);
        if (ec) {
            error = "cannot set the permissions of " + tmp.string() + ": " + ec.message();
            fs::remove(tmp, ignored);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}
