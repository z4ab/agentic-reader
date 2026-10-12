// M6.3: spec tests for loading and saving a project's CLAUDE.md, the GUI-free
// part of the instructions editor (InstructionsDialog only shows and edits the
// text).
//
// Fixture folders are built in a fresh temp directory per test case. No test
// data is committed.
//
// Interface choices where the spec is silent (the implementer conforms or
// files a TEST DISPUTE):
//   - Free functions at global scope, declared in app/src/project/Instructions.h
//     (implemented in app/src/project/Instructions.cpp, part of reader_core):
//
//       // <root>/CLAUDE.md
//       std::filesystem::path instructionsPath(const std::filesystem::path& root);
//
//       // Reads <root>/CLAUDE.md byte for byte into `text`. A missing file is
//       // not an error: returns true with `text` empty. Returns false with a
//       // non-empty `error` (and `text` empty) when the path exists but
//       // cannot be read as a file, e.g. it is a directory.
//       bool loadInstructions(const std::filesystem::path& root,
//                             std::string& text, std::string& error);
//
//       // Writes `text` to <root>/CLAUDE.md byte for byte, atomically: a
//       // temporary file in <root> is written, then renamed over CLAUDE.md.
//       // Returns false with a non-empty `error` on failure. After a failure
//       // CLAUDE.md is exactly as before (or still absent) and no temporary
//       // file is left in <root>. Never creates <root> or <root>/.reader/, so
//       // it works for an unsaved project. Empty `text` writes an empty file.
//       bool saveInstructions(const std::filesystem::path& root,
//                             const std::string& text, std::string& error);
//
//   - Byte for byte means no newline is added, removed or converted (no
//     trailing newline stays absent, "\r\n" stays "\r\n"), and UTF-8 passes
//     through unchanged. The text is not validated or normalised.
//   - "Atomic" is checked through a hard link to the old CLAUDE.md: a rename
//     leaves the old inode, and so the link, with the old content, while an
//     in-place rewrite would change it. The name of the temporary file is
//     free; the tests only check that none is left behind.
//   - A failed write is forced, also when running as root, with RLIMIT_FSIZE
//     (POSIX only): the new text is larger than the limit, so writing the
//     temporary file fails with EFBIG.
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

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

namespace fs = std::filesystem;

namespace {

// A fresh, empty directory that is removed with everything in it at scope exit.
class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / ("agentic-reader-test-instructions-" + std::to_string(stamp) + "-"
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
    fs::create_directories(path.parent_path());
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

// Names of every entry directly in `dir`, hidden ones included.
std::set<std::string> entries(const fs::path& dir)
{
    std::set<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir))
        names.insert(entry.path().filename().string());
    return names;
}

// A project folder with a PDF in it, as the app would have.
fs::path makeRoot(const TempDir& tmp, const std::string& name = "proj")
{
    const fs::path root = tmp.path() / name;
    writeFile(root / "paper.pdf", "%PDF-1.4 not really\n");
    return root;
}

std::string loadOk(const fs::path& root)
{
    std::string text = "stale";
    std::string error;
    const bool ok = loadInstructions(root, text, error);
    INFO("load error: " << error);
    REQUIRE(ok);
    return text;
}

void saveOk(const fs::path& root, const std::string& text)
{
    std::string error;
    const bool ok = saveInstructions(root, text, error);
    INFO("save error: " << error);
    REQUIRE(ok);
}

const std::string kFrench = "# Notes du projet\n"
                            "Always answer in French.\n"
                            "Citez les num\xC3\xA9ros de page \xE2\x80\x94 toujours.\n"
                            "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\x84\n";

} // namespace

TEST_CASE("instructionsPath is CLAUDE.md in the project root")
{
    const fs::path root = fs::path("/some/project");
    CHECK(instructionsPath(root) == root / "CLAUDE.md");
}

TEST_CASE("load gives empty text when CLAUDE.md is missing")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    const auto before = entries(root);

    CHECK(loadOk(root).empty());
    // Loading does not create the file or anything else.
    CHECK(entries(root) == before);
    CHECK_FALSE(fs::exists(root / "CLAUDE.md"));
    CHECK_FALSE(fs::exists(root / ".reader"));
}

TEST_CASE("load returns the file byte for byte")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);

    SUBCASE("UTF-8 with a trailing newline")
    {
        writeFile(root / "CLAUDE.md", kFrench);
        CHECK(loadOk(root) == kFrench);
    }
    SUBCASE("no trailing newline")
    {
        writeFile(root / "CLAUDE.md", "Answer briefly.");
        CHECK(loadOk(root) == "Answer briefly.");
    }
    SUBCASE("CRLF line endings and blank lines")
    {
        const std::string text = "line one\r\n\r\nline three\r\n\n\n";
        writeFile(root / "CLAUDE.md", text);
        CHECK(loadOk(root) == text);
    }
    SUBCASE("an empty file")
    {
        writeFile(root / "CLAUDE.md", "");
        CHECK(loadOk(root).empty());
    }
    SUBCASE("a large file")
    {
        std::string text;
        for (int i = 0; i < 20000; ++i)
            text += "Rule " + std::to_string(i) + ": cite the page. \xC3\xA9\n";
        writeFile(root / "CLAUDE.md", text);
        CHECK(loadOk(root) == text);
    }
}

TEST_CASE("load fails with an error when CLAUDE.md is a directory")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    fs::create_directories(root / "CLAUDE.md");

    std::string text = "stale";
    std::string error;
    CHECK_FALSE(loadInstructions(root, text, error));
    CHECK_FALSE(error.empty());
    CHECK(text.empty());
}

TEST_CASE("save creates CLAUDE.md with exactly the given text")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    const auto before = entries(root);

    saveOk(root, kFrench);

    CHECK(readFile(root / "CLAUDE.md") == kFrench);
    CHECK(loadOk(root) == kFrench);
    // Only CLAUDE.md is new: no temporary file, no .reader/ (an unsaved
    // project stays unsaved).
    auto expected = before;
    expected.insert("CLAUDE.md");
    CHECK(entries(root) == expected);
    CHECK(readFile(root / "paper.pdf") == "%PDF-1.4 not really\n");
}

TEST_CASE("save keeps the text byte for byte")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    writeFile(root / "CLAUDE.md", "old\n");

    SUBCASE("trailing newline kept")
    {
        saveOk(root, "Always answer in French.\n");
        CHECK(readFile(root / "CLAUDE.md") == "Always answer in French.\n");
    }
    SUBCASE("no trailing newline is not given one")
    {
        saveOk(root, "Always answer in French.");
        CHECK(readFile(root / "CLAUDE.md") == "Always answer in French.");
    }
    SUBCASE("several trailing newlines kept")
    {
        saveOk(root, "a\n\n\n");
        CHECK(readFile(root / "CLAUDE.md") == "a\n\n\n");
    }
    SUBCASE("CRLF kept")
    {
        saveOk(root, "a\r\nb\r\n");
        CHECK(readFile(root / "CLAUDE.md") == "a\r\nb\r\n");
    }
    SUBCASE("UTF-8 kept")
    {
        saveOk(root, kFrench);
        CHECK(readFile(root / "CLAUDE.md") == kFrench);
    }
    SUBCASE("empty text writes an empty file")
    {
        saveOk(root, "");
        REQUIRE(fs::is_regular_file(root / "CLAUDE.md"));
        CHECK(fs::file_size(root / "CLAUDE.md") == 0);
        CHECK(loadOk(root).empty());
    }
    CHECK(entries(root) == std::set<std::string>{"CLAUDE.md", "paper.pdf"});
}

TEST_CASE("save then load round-trips")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    writeFile(root / "CLAUDE.md", kFrench);

    std::string text = loadOk(root);
    text += "Answer briefly.\n";
    saveOk(root, text);
    CHECK(loadOk(root) == kFrench + "Answer briefly.\n");

    // Saving the loaded text unchanged leaves the file unchanged.
    saveOk(root, loadOk(root));
    CHECK(readFile(root / "CLAUDE.md") == kFrench + "Answer briefly.\n");
}

TEST_CASE("repeated saves leave only CLAUDE.md")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    for (int i = 0; i < 5; ++i)
        saveOk(root, "version " + std::to_string(i) + "\n");
    CHECK(readFile(root / "CLAUDE.md") == "version 4\n");
    CHECK(entries(root) == std::set<std::string>{"CLAUDE.md", "paper.pdf"});
}

TEST_CASE("save replaces CLAUDE.md by rename, not by rewriting it in place")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    writeFile(root / "CLAUDE.md", "old instructions\n");
    // A second name for the old file, outside the root.
    const fs::path link = tmp.path() / "old-link";
    fs::create_hard_link(root / "CLAUDE.md", link);

    saveOk(root, "new instructions\n");

    CHECK(readFile(root / "CLAUDE.md") == "new instructions\n");
    // The old inode was replaced, not rewritten: the link still holds the old
    // text and CLAUDE.md is a different file now.
    CHECK(readFile(link) == "old instructions\n");
    CHECK_FALSE(fs::equivalent(root / "CLAUDE.md", link));
    CHECK(entries(root) == std::set<std::string>{"CLAUDE.md", "paper.pdf"});
}

TEST_CASE("save works in a root with a non-ASCII name")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp, "Projets \xE2\x80\x93 \xC3\xA9t\xC3\xA9");
    saveOk(root, kFrench);
    CHECK(readFile(root / "CLAUDE.md") == kFrench);
    CHECK(loadOk(root) == kFrench);
}

TEST_CASE("save fails when the root does not exist and creates nothing")
{
    TempDir tmp;
    const fs::path root = tmp.path() / "gone";

    std::string error;
    CHECK_FALSE(saveInstructions(root, "text\n", error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(root));
    CHECK(entries(tmp.path()).empty());
}

TEST_CASE("save fails when CLAUDE.md is a directory and leaves it alone")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    fs::create_directories(root / "CLAUDE.md");
    writeFile(root / "CLAUDE.md" / "inside.txt", "keep\n");
    const auto before = entries(root);

    std::string error;
    CHECK_FALSE(saveInstructions(root, "text\n", error));
    CHECK_FALSE(error.empty());
    CHECK(fs::is_directory(root / "CLAUDE.md"));
    CHECK(readFile(root / "CLAUDE.md" / "inside.txt") == "keep\n");
    CHECK(entries(root) == before);
}

#ifndef _WIN32
namespace {

// Limits the size of any file this process writes, and restores the old
// limit at scope exit. SIGXFSZ is ignored so an oversized write fails with
// EFBIG instead of killing the test.
class FileSizeLimit
{
public:
    explicit FileSizeLimit(rlim_t bytes)
    {
        REQUIRE(getrlimit(RLIMIT_FSIZE, &m_old) == 0);
        m_oldHandler = std::signal(SIGXFSZ, SIG_IGN);
        rlimit limit = m_old;
        limit.rlim_cur = bytes;
        REQUIRE(setrlimit(RLIMIT_FSIZE, &limit) == 0);
    }
    ~FileSizeLimit()
    {
        setrlimit(RLIMIT_FSIZE, &m_old);
        std::signal(SIGXFSZ, m_oldHandler);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
    rlimit m_old{};
    void (*m_oldHandler)(int) = SIG_DFL;
};

std::string bigText()
{
    std::string text;
    while (text.size() < 256 * 1024)
        text += "This rule is long enough to overflow the file size limit.\n";
    return text;
}

bool saveLimited(const fs::path& root, const std::string& text, std::string& error)
{
    FileSizeLimit limit(4096);
    return saveInstructions(root, text, error);
}

} // namespace

TEST_CASE("a failed write keeps the original CLAUDE.md and leaves no temp file")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    writeFile(root / "CLAUDE.md", kFrench);
    const auto before = entries(root);

    std::string error;
    const bool ok = saveLimited(root, bigText(), error);

    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    CHECK(readFile(root / "CLAUDE.md") == kFrench);
    CHECK(entries(root) == before);

    // The next save works normally.
    saveOk(root, "after the failure\n");
    CHECK(readFile(root / "CLAUDE.md") == "after the failure\n");
    CHECK(entries(root) == before);
}

TEST_CASE("a failed first write leaves CLAUDE.md absent and no temp file")
{
    TempDir tmp;
    const fs::path root = makeRoot(tmp);
    const auto before = entries(root);

    std::string error;
    const bool ok = saveLimited(root, bigText(), error);

    CHECK_FALSE(ok);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(root / "CLAUDE.md"));
    CHECK(entries(root) == before);
}
#endif
