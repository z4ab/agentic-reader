// M10.2: layout tests for FileState::layout in project.json (app/src/project/Project.h).
// They are appended after the M6.1 tests, which are unchanged.
//
// Interface choices where the spec is silent (the implementer conforms or
// files a TEST DISPUTE):
//   - FileState gains a third member, std::optional<LayoutMode> layout, after
//     zoom. LayoutMode is the enum in pdf/PageLayout.h, which Project.h
//     includes. std::nullopt means unset. FileState{lastPage, zoom} still
//     compiles and means unset.
//   - The JSON key is "layout" in the file's entry, next to "last_page" and
//     "zoom". LayoutMode::SinglePage is "single" and LayoutMode::Continuous is
//     "continuous".
//   - Unset is not written: the entry has no "layout" key. save() removes a
//     "layout" key from an entry whose layout is unset, including a key that
//     held an unknown value.
//   - A "layout" value other than "single" or "continuous" (another string, a
//     number, null) reads as unset. It is not corruption: no warning, and the
//     other fields of the entry still read.
//   - Project does not compare the layout with detectLayout(). The caller
//     switches back to the detected mode by calling setFileState with
//     std::nullopt for layout.
//   - Unknown fields in the entry are kept on save, as for the other fields.
//   - This file includes "pdf/PageLayout.h" directly for LayoutMode.
//
// M6.1: spec tests for Project (app/src/project/Project.h).
//
// Fixture folders are built in a fresh temp directory per test case. No test
// data is committed. The tests read and write .reader/project.json directly
// with nlohmann-json (already in vcpkg.json, linked by tests/CMakeLists.txt).
//
// Interface choices where the spec is silent:
//   - Project::open(folder, error) returns std::unique_ptr<Project>. On failure
//     it returns nullptr and sets `error` to a non-empty message. Failures:
//     the folder does not exist, or the path is not a directory.
//   - root() returns the folder passed to open(), comparable with
//     std::filesystem::path.
//   - pdfs() returns std::vector<std::string>: paths relative to root(), with
//     '/' separators (generic form). Only regular files whose extension is
//     ".pdf" in any letter case are listed. Directories named "*.pdf" are not.
//     Sorted by plain std::string comparison of those relative paths.
//   - "Hidden folder" means a directory whose name starts with '.'. The scan
//     does not descend into it at any depth. .reader/ is one of them.
//   - Relative paths in pdfs(), openFile() and the keys of files() all use the
//     same generic '/' form as the keys in project.json.
//   - openFile() returns std::string, empty when none. setOpenFile(rel) sets it.
//   - Per-file state is the struct FileState { int lastPage; std::string zoom; }
//     mapped to "last_page" and "zoom". files() returns
//     const std::map<std::string, FileState>&. setFileState(rel, state) inserts
//     or replaces the known fields of one entry and keeps its unknown fields.
//   - Chats are the struct ChatInfo { std::string id; std::string title;
//     std::optional<std::string> sessionId; std::string created;
//     std::string updated; } mapped to "id", "title", "session_id" (null when
//     std::nullopt), "created", "updated". Timestamps are stored as given.
//     chats() returns const std::vector<ChatInfo>&. setChats(vector) replaces
//     the list. Order is kept.
//   - FileState and ChatInfo are declared in Project.h at global scope.
//   - Opening a folder with no .reader/project.json gives an empty project
//     (openFile() empty, files() and chats() empty), no warnings, and
//     readOnly() == false. Open does not have to write project.json then.
//   - warnings() returns const std::vector<std::string>&. It is non-empty after
//     a corrupt-file recovery or when the file has a newer version.
//   - readOnly() returns bool. It is true when project.json has "version" > 1.
//   - save(error) returns bool. It writes .reader/project.json (version 1) and
//     returns true. On a read-only project it returns false, sets `error`, and
//     leaves project.json byte-for-byte unchanged.
//   - "Corrupt" covers text that is not valid JSON and valid JSON whose top
//     level is not an object. Recovery happens inside open(): the old file is
//     renamed to .reader/project.json.bak-<timestamp> (any non-empty suffix),
//     a new project.json holding an empty version 1 project is written, and
//     open() succeeds with a warning.
//   - Transcripts (.reader/chats/<chat_id>.jsonl, spec item 4) are not covered
//     here because the task's done-when list does not name them.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "pdf/PageLayout.h"
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

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
            / ("agentic-reader-test-project-" + std::to_string(stamp) + "-"
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
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path projectJson(const fs::path& root)
{
    return root / ".reader" / "project.json";
}

json readJson(const fs::path& path)
{
    return json::parse(readFile(path));
}

std::unique_ptr<Project> openOk(const fs::path& root)
{
    std::string error;
    auto project = Project::open(root, error);
    INFO("open error: " << error);
    REQUIRE(project != nullptr);
    return project;
}

void saveOk(Project& project)
{
    std::string error;
    const bool ok = project.save(error);
    INFO("save error: " << error);
    REQUIRE(ok);
}

// Every file name under `dir` (recursive) that ends with ".tmp".
std::vector<std::string> tmpFilesUnder(const fs::path& dir)
{
    std::vector<std::string> found;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) {
            found.push_back(entry.path().string());
        }
    }
    return found;
}

// Names of the files in .reader/ that start with "project.json.bak-".
std::vector<std::string> backupFiles(const fs::path& root)
{
    const std::string prefix = "project.json.bak-";
    std::vector<std::string> found;
    for (const auto& entry : fs::directory_iterator(root / ".reader")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0) {
            found.push_back(name);
        }
    }
    return found;
}

// project.json holds an empty version 1 project: no open file, no file state,
// no chats. Missing keys count as empty.
void checkEmptyProjectJson(const json& j)
{
    REQUIRE(j.is_object());
    CHECK(j.value("version", 0) == 1);
    if (j.contains("open_file")) {
        const auto& openFile = j["open_file"];
        CHECK((openFile.is_null() || openFile == ""));
    }
    if (j.contains("files")) {
        CHECK(j["files"].is_object());
        CHECK(j["files"].empty());
    }
    if (j.contains("chats")) {
        CHECK(j["chats"].is_array());
        CHECK(j["chats"].empty());
    }
}

void checkEmptyProject(const Project& project)
{
    CHECK(project.openFile().empty());
    CHECK(project.files().empty());
    CHECK(project.chats().empty());
}

} // namespace

TEST_CASE("project open fails for a folder that does not exist")
{
    TempDir tmp;
    const fs::path missing = tmp.path() / "no-such-folder";
    std::string error;
    const auto project = Project::open(missing, error);
    CHECK(project == nullptr);
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(missing));
}

TEST_CASE("project open fails for a path that is a file")
{
    TempDir tmp;
    const fs::path file = tmp.path() / "paper.pdf";
    writeFile(file, "%PDF-1.4\n");
    std::string error;
    const auto project = Project::open(file, error);
    CHECK(project == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("project open creates .reader and starts empty")
{
    TempDir tmp;
    REQUIRE_FALSE(fs::exists(tmp.path() / ".reader"));

    const auto project = openOk(tmp.path());
    CHECK(fs::is_directory(tmp.path() / ".reader"));
    CHECK(fs::path(project->root()) == tmp.path());
    checkEmptyProject(*project);
    CHECK(project->warnings().empty());
    CHECK_FALSE(project->readOnly());
}

TEST_CASE("project pdfs scans recursively, filters, and sorts by relative path")
{
    TempDir tmp;
    const fs::path& root = tmp.path();

    // Listed: any letter case of the .pdf extension, at any depth.
    writeFile(root / "z.Pdf", "x");
    writeFile(root / "b" / "d" / "e.PDF", "x");
    writeFile(root / "a.pdf", "x");
    writeFile(root / "b" / "c.pdf", "x");
    writeFile(root / "m" / "n.pDf", "x");

    // Not listed: other extensions, files named like the extension, a
    // directory named *.pdf, .reader/, and hidden folders at any depth.
    writeFile(root / "notes.txt", "x");
    writeFile(root / "paper.pdf.txt", "x");
    writeFile(root / "pdf", "x");
    writeFile(root / "b" / "readme.md", "x");
    fs::create_directories(root / "folder.pdf");
    writeFile(root / ".reader" / "cached.pdf", "x");
    writeFile(root / ".hidden" / "secret.pdf", "x");
    writeFile(root / "b" / ".git" / "objects" / "blob.pdf", "x");
    writeFile(root / "m" / ".cache" / "deep" / "thumb.PDF", "x");

    const auto project = openOk(root);
    const std::vector<std::string> expected = {
        "a.pdf",
        "b/c.pdf",
        "b/d/e.PDF",
        "m/n.pDf",
        "z.Pdf",
    };
    CHECK(project->pdfs() == expected);
}

TEST_CASE("project pdfs is empty for a folder without PDFs")
{
    TempDir tmp;
    writeFile(tmp.path() / "notes.txt", "x");
    const auto project = openOk(tmp.path());
    CHECK(project->pdfs().empty());
}

TEST_CASE("project round trips open_file, files, and chats")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(root / "papers" / "attention.pdf", "x");
    writeFile(root / "intro.pdf", "x");

    {
        const auto project = openOk(root);
        project->setOpenFile("papers/attention.pdf");
        project->setFileState("papers/attention.pdf", FileState{11, "fit-width"});
        project->setFileState("intro.pdf", FileState{0, "fit-page"});

        ChatInfo first;
        first.id = "0b6f3c2e-6a43-4c39-9d0f-1f6b0e9a7c11";
        first.title = "Attention questions";
        first.sessionId = "session-abc";
        first.created = "2026-10-01T09:30:00Z";
        first.updated = "2026-10-02T17:45:12Z";

        ChatInfo second;
        second.id = "5c1d9a70-2f0e-4b8b-8f39-3d2a6e0b4f22";
        second.title = "Draft chat";
        second.sessionId = std::nullopt;
        second.created = "2026-10-03T08:00:00Z";
        second.updated = "2026-10-03T08:00:00Z";

        project->setChats({first, second});
        saveOk(*project);
    }

    SUBCASE("project.json follows the version 1 schema")
    {
        const json j = readJson(projectJson(root));
        CHECK(j["version"] == 1);
        CHECK(j["open_file"] == "papers/attention.pdf");
        CHECK(j["files"]["papers/attention.pdf"]["last_page"] == 11);
        CHECK(j["files"]["papers/attention.pdf"]["zoom"] == "fit-width");
        CHECK(j["files"]["intro.pdf"]["last_page"] == 0);
        CHECK(j["files"]["intro.pdf"]["zoom"] == "fit-page");

        REQUIRE(j["chats"].is_array());
        REQUIRE(j["chats"].size() == 2);
        CHECK(j["chats"][0]["id"] == "0b6f3c2e-6a43-4c39-9d0f-1f6b0e9a7c11");
        CHECK(j["chats"][0]["title"] == "Attention questions");
        CHECK(j["chats"][0]["session_id"] == "session-abc");
        CHECK(j["chats"][0]["created"] == "2026-10-01T09:30:00Z");
        CHECK(j["chats"][0]["updated"] == "2026-10-02T17:45:12Z");
        CHECK(j["chats"][1]["id"] == "5c1d9a70-2f0e-4b8b-8f39-3d2a6e0b4f22");
        REQUIRE(j["chats"][1].contains("session_id"));
        CHECK(j["chats"][1]["session_id"].is_null());
    }

    SUBCASE("reopening restores the same state")
    {
        const auto project = openOk(root);
        CHECK(project->warnings().empty());
        CHECK_FALSE(project->readOnly());
        CHECK(project->openFile() == "papers/attention.pdf");

        const auto& files = project->files();
        REQUIRE(files.size() == 2);
        REQUIRE(files.count("papers/attention.pdf") == 1);
        CHECK(files.at("papers/attention.pdf").lastPage == 11);
        CHECK(files.at("papers/attention.pdf").zoom == "fit-width");
        REQUIRE(files.count("intro.pdf") == 1);
        CHECK(files.at("intro.pdf").lastPage == 0);
        CHECK(files.at("intro.pdf").zoom == "fit-page");

        const auto& chats = project->chats();
        REQUIRE(chats.size() == 2);
        CHECK(chats[0].id == "0b6f3c2e-6a43-4c39-9d0f-1f6b0e9a7c11");
        CHECK(chats[0].title == "Attention questions");
        REQUIRE(chats[0].sessionId.has_value());
        CHECK(*chats[0].sessionId == "session-abc");
        CHECK(chats[0].created == "2026-10-01T09:30:00Z");
        CHECK(chats[0].updated == "2026-10-02T17:45:12Z");
        CHECK(chats[1].id == "5c1d9a70-2f0e-4b8b-8f39-3d2a6e0b4f22");
        CHECK(chats[1].title == "Draft chat");
        CHECK_FALSE(chats[1].sessionId.has_value());
        CHECK(chats[1].created == "2026-10-03T08:00:00Z");
        CHECK(chats[1].updated == "2026-10-03T08:00:00Z");
    }
}

TEST_CASE("project loads a hand-written version 1 project.json")
{
    TempDir tmp;
    writeFile(projectJson(tmp.path()), R"({
        "version": 1,
        "open_file": "rel/path.pdf",
        "files": {"rel/path.pdf": {"last_page": 11, "zoom": "fit-width"}},
        "chats": [{"id": "c1", "title": "T", "session_id": null,
                   "created": "2026-01-01T00:00:00Z",
                   "updated": "2026-01-02T00:00:00Z"}]
    })");

    const auto project = openOk(tmp.path());
    CHECK(project->warnings().empty());
    CHECK_FALSE(project->readOnly());
    CHECK(project->openFile() == "rel/path.pdf");
    REQUIRE(project->files().count("rel/path.pdf") == 1);
    CHECK(project->files().at("rel/path.pdf").lastPage == 11);
    CHECK(project->files().at("rel/path.pdf").zoom == "fit-width");
    REQUIRE(project->chats().size() == 1);
    CHECK(project->chats()[0].id == "c1");
    CHECK(project->chats()[0].title == "T");
    CHECK_FALSE(project->chats()[0].sessionId.has_value());
    CHECK(project->chats()[0].created == "2026-01-01T00:00:00Z");
    CHECK(project->chats()[0].updated == "2026-01-02T00:00:00Z");
}

TEST_CASE("project keeps unknown fields on save")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    const json topLevelExtra = {{"nested", {1, 2, 3}}, {"flag", true}};
    const json fileExtra = {{"color", "yellow"}, {"marks", {4, 5}}};

    json original = {
        {"version", 1},
        {"open_file", "a.pdf"},
        {"files",
         {{"a.pdf", {{"last_page", 3}, {"zoom", "fit-width"}, {"future_file_field", fileExtra}}},
          {"b.pdf", {{"last_page", 1}, {"zoom", "fit-page"}, {"future_file_field", 42}}}}},
        {"chats", json::array()},
        {"future_top_field", topLevelExtra},
    };
    writeFile(projectJson(root), original.dump(2));

    {
        const auto project = openOk(root);
        CHECK(project->warnings().empty());
        project->setOpenFile("b.pdf");
        project->setFileState("a.pdf", FileState{7, "fit-page"});
        saveOk(*project);
    }

    const json saved = readJson(projectJson(root));
    CHECK(saved["version"] == 1);
    CHECK(saved["open_file"] == "b.pdf");

    REQUIRE(saved.contains("future_top_field"));
    CHECK(saved["future_top_field"] == topLevelExtra);

    // The updated entry keeps its unknown field next to the new values.
    CHECK(saved["files"]["a.pdf"]["last_page"] == 7);
    CHECK(saved["files"]["a.pdf"]["zoom"] == "fit-page");
    REQUIRE(saved["files"]["a.pdf"].contains("future_file_field"));
    CHECK(saved["files"]["a.pdf"]["future_file_field"] == fileExtra);

    // The untouched entry keeps everything.
    CHECK(saved["files"]["b.pdf"]["last_page"] == 1);
    CHECK(saved["files"]["b.pdf"]["zoom"] == "fit-page");
    CHECK(saved["files"]["b.pdf"]["future_file_field"] == 42);

    // A second open and save keeps them too.
    {
        const auto project = openOk(root);
        saveOk(*project);
    }
    const json again = readJson(projectJson(root));
    CHECK(again["future_top_field"] == topLevelExtra);
    CHECK(again["files"]["a.pdf"]["future_file_field"] == fileExtra);
}

TEST_CASE("project recovers from a corrupt project.json")
{
    TempDir tmp;
    const fs::path& root = tmp.path();

    std::string corrupt;
    SUBCASE("text that is not JSON")
    {
        corrupt = "{\"version\": 1, \"files\": {oops";
    }
    SUBCASE("JSON that is not an object")
    {
        corrupt = "[1, 2, 3]";
    }
    writeFile(projectJson(root), corrupt);

    const auto project = openOk(root);
    CHECK_FALSE(project->warnings().empty());
    CHECK_FALSE(project->readOnly());
    checkEmptyProject(*project);

    // The corrupt bytes moved to exactly one backup with a non-empty suffix.
    const auto backups = backupFiles(root);
    REQUIRE(backups.size() == 1);
    CHECK(backups[0].size() > std::string("project.json.bak-").size());
    CHECK(readFile(root / ".reader" / backups[0]) == corrupt);

    // project.json now holds an empty version 1 project.
    REQUIRE(fs::is_regular_file(projectJson(root)));
    checkEmptyProjectJson(readJson(projectJson(root)));
    CHECK(tmpFilesUnder(root / ".reader").empty());

    // The recovered project saves and reopens cleanly.
    project->setOpenFile("a.pdf");
    saveOk(*project);
    const auto reopened = openOk(root);
    CHECK(reopened->warnings().empty());
    CHECK(reopened->openFile() == "a.pdf");
    CHECK(backupFiles(root).size() == 1);
}

TEST_CASE("project save is atomic and leaves no .tmp file behind")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(root / "a.pdf", "x");

    const auto project = openOk(root);
    project->setOpenFile("a.pdf");
    project->setFileState("a.pdf", FileState{2, "fit-width"});
    saveOk(*project);

    REQUIRE(fs::is_regular_file(projectJson(root)));
    CHECK(tmpFilesUnder(root).empty());

    // Overwriting an existing project.json also leaves none behind.
    project->setFileState("a.pdf", FileState{5, "fit-width"});
    saveOk(*project);
    CHECK(tmpFilesUnder(root).empty());
    CHECK(readJson(projectJson(root))["files"]["a.pdf"]["last_page"] == 5);
}

TEST_CASE("project with a newer version opens read-only with a warning")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    const std::string newer =
        R"({"version": 2, "open_file": "a.pdf", "files": {}, "chats": [], "new_thing": {"x": 1}})";
    writeFile(projectJson(root), newer);

    const auto project = openOk(root);
    CHECK(project->readOnly());
    CHECK_FALSE(project->warnings().empty());

    // Not treated as corrupt.
    CHECK(backupFiles(root).empty());

    // Saving is refused and the file is untouched.
    project->setOpenFile("b.pdf");
    std::string error;
    CHECK_FALSE(project->save(error));
    CHECK_FALSE(error.empty());
    CHECK(readFile(projectJson(root)) == newer);
    CHECK(tmpFilesUnder(root).empty());
}

TEST_CASE("project with version 1 is writable")
{
    TempDir tmp;
    writeFile(projectJson(tmp.path()), R"({"version": 1})");
    const auto project = openOk(tmp.path());
    CHECK_FALSE(project->readOnly());
    CHECK(project->warnings().empty());
    checkEmptyProject(*project);
}

TEST_CASE("project reads layout single and continuous from project.json")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(projectJson(root), R"({
        "version": 1,
        "open_file": "deck.pdf",
        "files": {
            "deck.pdf": {"last_page": 4, "zoom": "fit-page", "layout": "single"},
            "paper.pdf": {"last_page": 0, "zoom": "fit-width", "layout": "continuous"}
        },
        "chats": []
    })");

    const auto project = openOk(root);
    CHECK(project->warnings().empty());
    REQUIRE(project->files().count("deck.pdf") == 1);
    REQUIRE(project->files().count("paper.pdf") == 1);
    CHECK(project->files().at("deck.pdf").layout == LayoutMode::SinglePage);
    CHECK(project->files().at("paper.pdf").layout == LayoutMode::Continuous);
    CHECK(project->files().at("deck.pdf").lastPage == 4);
    CHECK(project->files().at("deck.pdf").zoom == "fit-page");
    CHECK(project->fileStateFor("deck.pdf", 10).layout == LayoutMode::SinglePage);
}

TEST_CASE("project layout round trips and is written only when set")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(root / "deck.pdf", "x");
    writeFile(root / "paper.pdf", "x");
    writeFile(root / "plain.pdf", "x");

    {
        const auto project = openOk(root);
        project->setFileState("deck.pdf", FileState{4, "fit-page", LayoutMode::SinglePage});
        project->setFileState("paper.pdf", FileState{0, "fit-width", LayoutMode::Continuous});
        project->setFileState("plain.pdf", FileState{2, "fit-width", std::nullopt});
        saveOk(*project);
    }

    const json saved = readJson(projectJson(root));
    CHECK(saved["files"]["deck.pdf"]["layout"] == "single");
    CHECK(saved["files"]["paper.pdf"]["layout"] == "continuous");
    REQUIRE(saved["files"].contains("plain.pdf"));
    CHECK_FALSE(saved["files"]["plain.pdf"].contains("layout"));
    CHECK(saved["files"]["plain.pdf"]["last_page"] == 2);
    CHECK(saved["files"]["plain.pdf"]["zoom"] == "fit-width");

    const auto reopened = openOk(root);
    REQUIRE(reopened->files().count("deck.pdf") == 1);
    REQUIRE(reopened->files().count("paper.pdf") == 1);
    REQUIRE(reopened->files().count("plain.pdf") == 1);
    CHECK(reopened->files().at("deck.pdf").layout == LayoutMode::SinglePage);
    CHECK(reopened->files().at("paper.pdf").layout == LayoutMode::Continuous);
    CHECK_FALSE(reopened->files().at("plain.pdf").layout.has_value());
}

TEST_CASE("project layout key is removed when the choice returns to unset")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(root / "sample.pdf", "x");

    {
        const auto project = openOk(root);
        project->setFileState("sample.pdf", FileState{1, "fit-width", LayoutMode::SinglePage});
        saveOk(*project);
    }
    REQUIRE(readJson(projectJson(root))["files"]["sample.pdf"]["layout"] == "single");

    {
        // The user switched back to the detected mode.
        const auto project = openOk(root);
        REQUIRE(project->files().count("sample.pdf") == 1);
        CHECK(project->files().at("sample.pdf").layout == LayoutMode::SinglePage);
        project->setFileState("sample.pdf", FileState{1, "fit-width", std::nullopt});
        saveOk(*project);
    }

    const json saved = readJson(projectJson(root));
    REQUIRE(saved["files"].contains("sample.pdf"));
    CHECK_FALSE(saved["files"]["sample.pdf"].contains("layout"));
    CHECK(saved["files"]["sample.pdf"]["last_page"] == 1);
    CHECK(saved["files"]["sample.pdf"]["zoom"] == "fit-width");
}

TEST_CASE("project layout reads an unknown value as unset")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    writeFile(projectJson(root), R"({
        "version": 1,
        "files": {
            "grid.pdf": {"last_page": 3, "zoom": "fit-page", "layout": "grid"},
            "number.pdf": {"last_page": 1, "zoom": "fit-width", "layout": 1},
            "null.pdf": {"last_page": 2, "zoom": "fit-width", "layout": null},
            "empty.pdf": {"last_page": 0, "zoom": "", "layout": ""}
        },
        "chats": []
    })");

    {
        const auto project = openOk(root);
        CHECK(project->warnings().empty());
        CHECK_FALSE(project->readOnly());
        for (const char* rel : {"grid.pdf", "number.pdf", "null.pdf", "empty.pdf"}) {
            INFO("entry: " << rel);
            REQUIRE(project->files().count(rel) == 1);
            CHECK_FALSE(project->files().at(rel).layout.has_value());
        }
        CHECK(project->files().at("grid.pdf").lastPage == 3);
        CHECK(project->files().at("grid.pdf").zoom == "fit-page");
        CHECK(project->files().at("number.pdf").lastPage == 1);
        CHECK(project->files().at("null.pdf").zoom == "fit-width");
        saveOk(*project);
    }

    // Unset is not written, so the unknown values are gone after a save.
    const json saved = readJson(projectJson(root));
    for (const char* rel : {"grid.pdf", "number.pdf", "null.pdf", "empty.pdf"}) {
        INFO("entry: " << rel);
        REQUIRE(saved["files"].contains(rel));
        CHECK_FALSE(saved["files"][rel].contains("layout"));
    }
    CHECK(saved["files"]["grid.pdf"]["last_page"] == 3);
}

TEST_CASE("project layout keeps unknown fields on save")
{
    TempDir tmp;
    const fs::path& root = tmp.path();
    const json fileExtra = {{"color", "yellow"}, {"marks", {4, 5}}};
    const json topLevelExtra = {{"nested", {1, 2, 3}}};

    json original = {
        {"version", 1},
        {"open_file", "deck.pdf"},
        {"files",
         {{"deck.pdf",
           {{"last_page", 3},
            {"zoom", "fit-page"},
            {"layout", "single"},
            {"future_file_field", fileExtra}}},
          {"other.pdf",
           {{"last_page", 1},
            {"zoom", "fit-width"},
            {"layout", "continuous"},
            {"future_file_field", 42}}}}},
        {"chats", json::array()},
        {"future_top_field", topLevelExtra},
    };
    writeFile(projectJson(root), original.dump(2));

    {
        const auto project = openOk(root);
        CHECK(project->warnings().empty());
        // deck.pdf switches back to the detected mode. other.pdf is not touched.
        project->setFileState("deck.pdf", FileState{5, "fit-page", std::nullopt});
        saveOk(*project);
    }

    const json saved = readJson(projectJson(root));
    REQUIRE(saved.contains("future_top_field"));
    CHECK(saved["future_top_field"] == topLevelExtra);

    CHECK(saved["files"]["deck.pdf"]["last_page"] == 5);
    CHECK(saved["files"]["deck.pdf"]["zoom"] == "fit-page");
    CHECK_FALSE(saved["files"]["deck.pdf"].contains("layout"));
    REQUIRE(saved["files"]["deck.pdf"].contains("future_file_field"));
    CHECK(saved["files"]["deck.pdf"]["future_file_field"] == fileExtra);

    CHECK(saved["files"]["other.pdf"]["layout"] == "continuous");
    CHECK(saved["files"]["other.pdf"]["future_file_field"] == 42);
}
