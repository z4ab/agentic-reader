// M6.3 review 1: saveInstructions() on a symlinked CLAUDE.md and on a file
// with restricted permissions. Implementer-owned, next to the tester's
// test_instructions.cpp. POSIX only: symlinks and mode bits.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/Instructions.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace fs = std::filesystem;

#ifndef _WIN32
namespace {

class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / ("agentic-reader-test-instructions-save-" + std::to_string(stamp) + "-"
               + std::to_string(counter++));
        fs::create_directories(m_path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(m_path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void writeFile(const fs::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << content;
    out.close();
    REQUIRE(out.good());
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::set<std::string> entries(const fs::path& dir)
{
    std::set<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
        names.insert(entry.path().filename().string());
    return names;
}

void saveOk(const fs::path& root, const std::string& text)
{
    std::string error;
    const bool ok = saveInstructions(root, text, error);
    INFO("save error: " << error);
    REQUIRE(ok);
}

constexpr fs::perms kModeBits = fs::perms::all;

} // namespace

TEST_CASE("a relative CLAUDE.md symlink stays a link and its target gets the text")
{
    TempDir tmp;
    const fs::path root = tmp.path();
    writeFile(root / "AGENTS.md", "old\n");
    fs::create_symlink("AGENTS.md", root / "CLAUDE.md");

    saveOk(root, "new\n");

    CHECK(fs::is_symlink(root / "CLAUDE.md"));
    CHECK(fs::read_symlink(root / "CLAUDE.md") == fs::path("AGENTS.md"));
    CHECK(readFile(root / "AGENTS.md") == "new\n");
    std::string text;
    std::string error;
    REQUIRE(loadInstructions(root, text, error));
    CHECK(text == "new\n");
    CHECK(entries(root) == std::set<std::string>{"AGENTS.md", "CLAUDE.md"});
}

TEST_CASE("a CLAUDE.md symlink into another folder writes there and leaves no temp file")
{
    TempDir tmp;
    const fs::path root = tmp.path() / "proj";
    const fs::path shared = tmp.path() / "shared";
    fs::create_directories(root);
    fs::create_directories(shared);
    writeFile(shared / "rules.md", "old\n");
    fs::create_symlink(shared / "rules.md", root / "CLAUDE.md");

    saveOk(root, "new\n");

    CHECK(fs::is_symlink(root / "CLAUDE.md"));
    CHECK(readFile(shared / "rules.md") == "new\n");
    CHECK(entries(root) == std::set<std::string>{"CLAUDE.md"});
    CHECK(entries(shared) == std::set<std::string>{"rules.md"});
}

TEST_CASE("a dangling CLAUDE.md symlink creates its target")
{
    TempDir tmp;
    const fs::path root = tmp.path();
    fs::create_symlink("AGENTS.md", root / "CLAUDE.md");

    saveOk(root, "first\n");

    CHECK(fs::is_symlink(root / "CLAUDE.md"));
    CHECK(readFile(root / "AGENTS.md") == "first\n");
    CHECK(entries(root) == std::set<std::string>{"AGENTS.md", "CLAUDE.md"});
}

TEST_CASE("a relative symlink with .. in a project opened through a symlinked folder")
{
    // home/papers -> ../data/papers, and papers/CLAUDE.md -> ../shared/AGENTS.md.
    // Through the real folder the link names data/shared/AGENTS.md; collapsing
    // ".." against the typed path would wrongly give home/shared/AGENTS.md.
    TempDir tmp;
    const fs::path base = tmp.path();
    fs::create_directories(base / "home" / "shared");
    fs::create_directories(base / "data" / "papers");
    fs::create_directories(base / "data" / "shared");
    writeFile(base / "data" / "shared" / "AGENTS.md", "real\n");
    writeFile(base / "home" / "shared" / "AGENTS.md", "decoy\n");
    fs::create_symlink("../data/papers", base / "home" / "papers");
    fs::create_symlink("../shared/AGENTS.md", base / "data" / "papers" / "CLAUDE.md");
    const fs::path root = base / "home" / "papers";

    std::string text;
    std::string error;
    REQUIRE(loadInstructions(root, text, error));
    CHECK(text == "real\n");

    saveOk(root, "new\n");

    CHECK(readFile(base / "data" / "shared" / "AGENTS.md") == "new\n");
    CHECK(readFile(base / "home" / "shared" / "AGENTS.md") == "decoy\n");
    CHECK(fs::is_symlink(base / "data" / "papers" / "CLAUDE.md"));
    REQUIRE(loadInstructions(root, text, error));
    CHECK(text == "new\n");
    CHECK(entries(base / "data" / "shared") == std::set<std::string>{"AGENTS.md"});
    CHECK(entries(base / "home" / "shared") == std::set<std::string>{"AGENTS.md"});
    CHECK(entries(base / "data" / "papers") == std::set<std::string>{"CLAUDE.md"});
}

TEST_CASE("save keeps the permissions of CLAUDE.md")
{
    TempDir tmp;
    const fs::path root = tmp.path();
    writeFile(root / "CLAUDE.md", "old\n");

    SUBCASE("0600")
    {
        const fs::perms mode = fs::perms::owner_read | fs::perms::owner_write;
        fs::permissions(root / "CLAUDE.md", mode, fs::perm_options::replace);
        saveOk(root, "new\n");
        CHECK((fs::status(root / "CLAUDE.md").permissions() & kModeBits) == mode);
    }
    SUBCASE("0664")
    {
        const fs::perms mode = fs::perms::owner_read | fs::perms::owner_write
            | fs::perms::group_read | fs::perms::group_write | fs::perms::others_read;
        fs::permissions(root / "CLAUDE.md", mode, fs::perm_options::replace);
        saveOk(root, "new\n");
        CHECK((fs::status(root / "CLAUDE.md").permissions() & kModeBits) == mode);
    }
    CHECK(readFile(root / "CLAUDE.md") == "new\n");
}

TEST_CASE("save keeps the permissions of a symlink's target")
{
    TempDir tmp;
    const fs::path root = tmp.path();
    writeFile(root / "AGENTS.md", "old\n");
    const fs::perms mode = fs::perms::owner_read | fs::perms::owner_write;
    fs::permissions(root / "AGENTS.md", mode, fs::perm_options::replace);
    fs::create_symlink("AGENTS.md", root / "CLAUDE.md");

    saveOk(root, "new\n");

    CHECK(fs::is_symlink(root / "CLAUDE.md"));
    CHECK((fs::status(root / "AGENTS.md").permissions() & kModeBits) == mode);
    CHECK(readFile(root / "AGENTS.md") == "new\n");
}

TEST_CASE("save leaves a user file named CLAUDE.md.tmp alone")
{
    TempDir tmp;
    const fs::path root = tmp.path();
    writeFile(root / "CLAUDE.md.tmp", "mine\n");

    saveOk(root, "new\n");

    CHECK(readFile(root / "CLAUDE.md.tmp") == "mine\n");
    CHECK(readFile(root / "CLAUDE.md") == "new\n");
    CHECK(entries(root) == std::set<std::string>{"CLAUDE.md", "CLAUDE.md.tmp"});
}
#endif
