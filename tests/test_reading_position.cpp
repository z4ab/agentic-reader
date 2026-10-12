// M6.5: spec tests for the per-file reading position (last_page and zoom) kept
// in .reader/project.json by Project (app/src/project/Project.h).
//
// Fixture folders are built in a fresh temp directory per test case. No test
// data is committed. project.json is read and written directly with
// nlohmann-json.
//
// Interface choices where the spec is silent (the implementer conforms or
// files a TEST DISPUTE):
//   - The per-file state is the existing M6.1 struct FileState { int lastPage;
//     std::string zoom; }, stored under "files" in project.json, keyed by the
//     PDF's path relative to the project root with '/' separators (the same
//     form as Project::pdfs() and open_file). Project::setFileState() and
//     Project::files() are unchanged.
//   - "last_page" is the 0-based page index, the same as
//     PdfView::currentPage(): p. 40 is stored as 39. It is a JSON integer.
//   - "zoom" is a JSON string with one of these values:
//       "fit-width"  the Fit Width mode (follows the pane width on restore)
//       "fit-page"   the Fit Page mode
//       "<number>"   a custom zoom: the scale factor as a plain decimal number
//                    with '.' as the decimal point, 1.0 = 100%, e.g. "0.75"
//                    for 75% or "1.5" for 150%. Written the same in every
//                    locale (GTK sets LC_NUMERIC from the environment).
//       ""           nothing saved: the view keeps its current zoom.
//     A fit mode is stored as the mode, never as the percentage it happened
//     to give, so it still fits after the window size changes.
//   - Window-free helpers declared in Project.h at global scope, defined in
//     Project.cpp:
//       enum class ZoomKind { None, FitWidth, FitPage, Custom };
//       struct SavedZoom { ZoomKind kind = ZoomKind::None; double scale = 0.0; };
//       std::string formatZoom(const SavedZoom& zoom);
//       SavedZoom parseZoom(const std::string& text);
//     formatZoom gives the strings above ("" for None; scale is used only for
//     Custom). parseZoom is its inverse. It gives ZoomKind::None for "" and
//     for anything it cannot read in full: unknown words, "1.5x", "abc", a
//     number that is not finite or not > 0. A custom scale is not clamped
//     here (PdfView::setZoom clamps).
//   - New Project method, the restore side:
//       FileState fileStateFor(const std::string& rel, int pageCount) const;
//     returns the stored state of `rel` with lastPage clamped to
//     0..pageCount-1 (0 when pageCount <= 0) and the zoom string as stored.
//     With no entry for `rel` it returns FileState{} (lastPage 0, zoom "").
//     It does not touch the disk and does not change the stored entry.
//   - A file removed or renamed on disk: its entry is neither an error nor
//     dropped. open() loads it, fileStateFor() still answers for the old name,
//     and save() keeps it (the file may come back). The new name starts with
//     no entry, so it opens at page 1 with the current zoom.
//   - Saving reading positions goes through setFileState() + save(), which
//     must keep open_file, chats, the other files' entries and unknown fields
//     (M6.1 behaviour, checked here again with reading positions).
//   - Unsaved projects (no .reader/, MainFrame::m_project null) persist no
//     reading positions (M6.2 spec 5). That lives in MainFrame and is covered
//     by tests/acceptance/m6.5*.steps, not here.
//   - The 1 s debounce of page changes is a wxTimer in MainFrame and is not
//     unit tested. tests/acceptance/m6.5.steps checks it on disk: not written
//     0.3 s after a page change, written 2 s after it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>

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
            / ("agentic-reader-test-reading-position-" + std::to_string(stamp) + "-"
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

// A project folder with long.pdf and book/other.pdf (contents do not matter:
// Project never parses them).
void makePdfs(const fs::path& root)
{
    writeFile(root / "long.pdf", "%PDF-1.4\n");
    writeFile(root / "book" / "other.pdf", "%PDF-1.4\n");
}

std::string customZoom(double scale)
{
    return formatZoom(SavedZoom{ZoomKind::Custom, scale});
}

// Switches the global C and C++ locale to one with a decimal comma for the
// scope, if one is installed. active() is false when none is.
class CommaLocale
{
public:
    CommaLocale()
    {
        for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8",
                                 "nl_NL.UTF-8", "ru_RU.UTF-8"}) {
            try {
                std::locale loc(name);
                std::locale::global(loc);
                std::setlocale(LC_ALL, name);
                if (*std::localeconv()->decimal_point == ',') {
                    m_active = true;
                    return;
                }
            } catch (const std::runtime_error&) {
                continue;
            }
        }
        restore();
    }
    ~CommaLocale() { restore(); }
    CommaLocale(const CommaLocale&) = delete;
    CommaLocale& operator=(const CommaLocale&) = delete;

    bool active() const { return m_active; }

private:
    static void restore()
    {
        std::locale::global(std::locale::classic());
        std::setlocale(LC_ALL, "C");
    }
    bool m_active = false;
};

} // namespace

// --- zoom strings ------------------------------------------------------------

TEST_CASE("reading_position fit modes are stored as their names")
{
    CHECK(formatZoom(SavedZoom{ZoomKind::FitWidth, 0.0}) == "fit-width");
    CHECK(formatZoom(SavedZoom{ZoomKind::FitPage, 0.0}) == "fit-page");
    // The scale that happened to result from the fit is not part of the value.
    CHECK(formatZoom(SavedZoom{ZoomKind::FitWidth, 1.194}) == "fit-width");
    CHECK(formatZoom(SavedZoom{ZoomKind::FitPage, 0.987}) == "fit-page");
    CHECK(formatZoom(SavedZoom{}) == "");

    CHECK(parseZoom("fit-width").kind == ZoomKind::FitWidth);
    CHECK(parseZoom("fit-page").kind == ZoomKind::FitPage);
    CHECK(parseZoom("").kind == ZoomKind::None);
}

TEST_CASE("reading_position custom zoom is a plain decimal scale factor")
{
    SUBCASE("hand-written values parse")
    {
        const SavedZoom a = parseZoom("0.75");
        CHECK(a.kind == ZoomKind::Custom);
        CHECK(a.scale == doctest::Approx(0.75));
        const SavedZoom b = parseZoom("1.5");
        CHECK(b.kind == ZoomKind::Custom);
        CHECK(b.scale == doctest::Approx(1.5));
        const SavedZoom c = parseZoom("2");
        CHECK(c.kind == ZoomKind::Custom);
        CHECK(c.scale == doctest::Approx(2.0));
    }

    SUBCASE("formatted values round trip and are not a fit mode")
    {
        for (double scale : {0.25, 0.75, 1.0, 1.37, 1.5, 2.5, 8.0}) {
            CAPTURE(scale);
            const std::string text = customZoom(scale);
            CAPTURE(text);
            CHECK(!text.empty());
            CHECK(text != "fit-width");
            CHECK(text != "fit-page");
            CHECK(text.find(',') == std::string::npos);
            CHECK(text.find('%') == std::string::npos);
            const SavedZoom back = parseZoom(text);
            CHECK(back.kind == ZoomKind::Custom);
            CHECK(back.scale == doctest::Approx(scale).epsilon(1e-6));
        }
    }

    SUBCASE("a zoom reached by Ctrl+wheel steps round trips")
    {
        // 1.0 + 3 * 0.1 is not exactly 1.3 in floating point.
        const double scale = 1.0 + 0.1 + 0.1 + 0.1;
        const SavedZoom back = parseZoom(customZoom(scale));
        CHECK(back.kind == ZoomKind::Custom);
        CHECK(back.scale == doctest::Approx(1.3).epsilon(1e-6));
    }
}

TEST_CASE("reading_position unreadable zoom strings give no zoom")
{
    for (const char* text :
         {"abc", "fit", "Fit Width", "fit_width", "1.5x", "x1.5", "0", "-1", "-0.5", "nan",
          "inf", "1e999"}) {
        CAPTURE(text);
        CHECK(parseZoom(text).kind == ZoomKind::None);
    }
}

TEST_CASE("reading_position custom zoom uses '.' in a decimal-comma locale")
{
    CommaLocale locale;
    if (!locale.active()) {
        MESSAGE("no decimal-comma locale installed, skipped");
        return;
    }
    const std::string text = customZoom(1.5);
    CHECK(text.find(',') == std::string::npos);
    const SavedZoom back = parseZoom("1.5");
    CHECK(back.kind == ZoomKind::Custom);
    CHECK(back.scale == doctest::Approx(1.5));
    CHECK(parseZoom(text).scale == doctest::Approx(1.5));
}

// --- restore: clamping and missing entries ------------------------------------

TEST_CASE("reading_position fileStateFor clamps last_page to the page count")
{
    TempDir dir;
    makePdfs(dir.path());
    auto project = openOk(dir.path());
    project->setFileState("long.pdf", FileState{39, customZoom(0.75)});

    SUBCASE("in range: unchanged")
    {
        const FileState s = project->fileStateFor("long.pdf", 520);
        CHECK(s.lastPage == 39);
        CHECK(parseZoom(s.zoom).kind == ZoomKind::Custom);
        CHECK(parseZoom(s.zoom).scale == doctest::Approx(0.75));
    }
    SUBCASE("the last page itself: unchanged")
    {
        CHECK(project->fileStateFor("long.pdf", 40).lastPage == 39);
    }
    SUBCASE("beyond the end: the last page, zoom kept")
    {
        const FileState s = project->fileStateFor("long.pdf", 12);
        CHECK(s.lastPage == 11);
        CHECK(parseZoom(s.zoom).scale == doctest::Approx(0.75));
    }
    SUBCASE("a one-page document: page 0")
    {
        CHECK(project->fileStateFor("long.pdf", 1).lastPage == 0);
    }
    SUBCASE("no pages: page 0")
    {
        CHECK(project->fileStateFor("long.pdf", 0).lastPage == 0);
    }
    SUBCASE("the stored entry is not changed by clamping")
    {
        (void)project->fileStateFor("long.pdf", 12);
        REQUIRE(project->files().count("long.pdf") == 1);
        CHECK(project->files().at("long.pdf").lastPage == 39);
    }
}

TEST_CASE("reading_position fileStateFor clamps a negative last_page to 0")
{
    TempDir dir;
    makePdfs(dir.path());
    writeFile(projectJson(dir.path()),
              R"({"version": 1, "open_file": null,
                  "files": {"long.pdf": {"last_page": -5, "zoom": "fit-page"}},
                  "chats": []})");
    auto project = openOk(dir.path());
    const FileState s = project->fileStateFor("long.pdf", 520);
    CHECK(s.lastPage == 0);
    CHECK(s.zoom == "fit-page");
}

TEST_CASE("reading_position a hand-edited last_page beyond the end is clamped on restore")
{
    // The acceptance run edits project.json the same way while the project is
    // closed.
    TempDir dir;
    makePdfs(dir.path());
    writeFile(projectJson(dir.path()),
              R"({"version": 1, "open_file": "book/other.pdf",
                  "files": {"book/other.pdf": {"last_page": 9999, "zoom": "fit-page"}},
                  "chats": []})");
    auto project = openOk(dir.path());
    CHECK(project->warnings().empty());
    const FileState s = project->fileStateFor("book/other.pdf", 12);
    CHECK(s.lastPage == 11);
    CHECK(s.zoom == "fit-page");
}

TEST_CASE("reading_position a file with no entry starts at page 0 with no zoom")
{
    TempDir dir;
    makePdfs(dir.path());
    auto project = openOk(dir.path());
    project->setFileState("long.pdf", FileState{39, "fit-width"});

    const FileState s = project->fileStateFor("book/other.pdf", 12);
    CHECK(s.lastPage == 0);
    CHECK(s.zoom == "");
    CHECK(parseZoom(s.zoom).kind == ZoomKind::None);
    // Asking does not create an entry.
    CHECK(project->files().count("book/other.pdf") == 0);
}

// --- persistence ---------------------------------------------------------------

TEST_CASE("reading_position positions of two files round trip through project.json")
{
    TempDir dir;
    makePdfs(dir.path());
    {
        auto project = openOk(dir.path());
        project->setOpenFile("book/other.pdf");
        project->setFileState("long.pdf", FileState{39, customZoom(0.75)});
        project->setFileState("book/other.pdf",
                              FileState{4, formatZoom(SavedZoom{ZoomKind::FitWidth, 2.44})});
        saveOk(*project);
    }

    const json j = readJson(projectJson(dir.path()));
    REQUIRE(j["files"].is_object());
    REQUIRE(j["files"].contains("long.pdf"));
    REQUIRE(j["files"].contains("book/other.pdf"));
    CHECK(j["files"]["long.pdf"]["last_page"].is_number_integer());
    CHECK(j["files"]["long.pdf"]["last_page"] == 39);
    REQUIRE(j["files"]["long.pdf"]["zoom"].is_string());
    {
        const SavedZoom z = parseZoom(j["files"]["long.pdf"]["zoom"].get<std::string>());
        CHECK(z.kind == ZoomKind::Custom);
        CHECK(z.scale == doctest::Approx(0.75));
    }
    CHECK(j["files"]["book/other.pdf"]["last_page"] == 4);
    CHECK(j["files"]["book/other.pdf"]["zoom"] == "fit-width");
    CHECK(j["open_file"] == "book/other.pdf");

    auto again = openOk(dir.path());
    CHECK(again->warnings().empty());
    const FileState a = again->fileStateFor("long.pdf", 520);
    CHECK(a.lastPage == 39);
    CHECK(parseZoom(a.zoom).kind == ZoomKind::Custom);
    CHECK(parseZoom(a.zoom).scale == doctest::Approx(0.75));
    const FileState b = again->fileStateFor("book/other.pdf", 12);
    CHECK(b.lastPage == 4);
    CHECK(parseZoom(b.zoom).kind == ZoomKind::FitWidth);
}

TEST_CASE("reading_position updating one file keeps every other field")
{
    TempDir dir;
    makePdfs(dir.path());
    const json chatExtra = {{"pinned", true}};
    const json original = {
        {"version", 1},
        {"open_file", "long.pdf"},
        {"files",
         {{"long.pdf", {{"last_page", 2}, {"zoom", "fit-page"}, {"future_file_field", "x"}}},
          {"book/other.pdf", {{"last_page", 7}, {"zoom", "1.25"}}}}},
        {"chats",
         json::array({{{"id", "c1"},
                       {"title", "First"},
                       {"session_id", "s-1"},
                       {"created", "2026-10-01T10:00:00Z"},
                       {"updated", "2026-10-02T11:00:00Z"},
                       {"future_chat_field", chatExtra}}})},
        {"future_top_field", {{"a", 1}}},
    };
    writeFile(projectJson(dir.path()), original.dump(2));

    {
        auto project = openOk(dir.path());
        project->setFileState("long.pdf", FileState{39, customZoom(0.75)});
        saveOk(*project);
    }

    const json j = readJson(projectJson(dir.path()));
    CHECK(j["version"] == 1);
    CHECK(j["open_file"] == "long.pdf");
    CHECK(j["future_top_field"] == original["future_top_field"]);
    REQUIRE(j["chats"].is_array());
    REQUIRE(j["chats"].size() == 1);
    CHECK(j["chats"][0]["id"] == "c1");
    CHECK(j["chats"][0]["title"] == "First");
    CHECK(j["chats"][0]["session_id"] == "s-1");
    CHECK(j["chats"][0]["future_chat_field"] == chatExtra);

    CHECK(j["files"]["long.pdf"]["last_page"] == 39);
    CHECK(parseZoom(j["files"]["long.pdf"]["zoom"].get<std::string>()).scale
          == doctest::Approx(0.75));
    CHECK(j["files"]["long.pdf"]["future_file_field"] == "x");
    // The other file's entry is untouched.
    CHECK(j["files"]["book/other.pdf"]["last_page"] == 7);
    CHECK(j["files"]["book/other.pdf"]["zoom"] == "1.25");
}

TEST_CASE("reading_position a removed or renamed file's entry is harmless and kept")
{
    TempDir dir;
    makePdfs(dir.path());
    {
        auto project = openOk(dir.path());
        project->setFileState("long.pdf", FileState{39, customZoom(0.75)});
        project->setFileState("book/other.pdf", FileState{4, "fit-width"});
        saveOk(*project);
    }

    // Outside the app: other.pdf is renamed, long.pdf is removed.
    fs::rename(dir.path() / "book" / "other.pdf", dir.path() / "book" / "renamed.pdf");
    fs::remove(dir.path() / "long.pdf");

    auto project = openOk(dir.path());
    CHECK(project->warnings().empty());
    CHECK_FALSE(project->readOnly());

    // The new name has no position yet.
    const FileState renamed = project->fileStateFor("book/renamed.pdf", 12);
    CHECK(renamed.lastPage == 0);
    CHECK(renamed.zoom == "");

    // The old entries still answer and survive a save with a new entry.
    CHECK(project->fileStateFor("book/other.pdf", 12).lastPage == 4);
    CHECK(project->fileStateFor("long.pdf", 520).lastPage == 39);
    project->setFileState("book/renamed.pdf", FileState{2, "fit-page"});
    saveOk(*project);

    const json j = readJson(projectJson(dir.path()));
    CHECK(j["files"]["book/renamed.pdf"]["last_page"] == 2);
    CHECK(j["files"]["book/renamed.pdf"]["zoom"] == "fit-page");
    CHECK(j["files"]["book/other.pdf"]["last_page"] == 4);
    CHECK(j["files"]["long.pdf"]["last_page"] == 39);

    // The file comes back under its old name: its position is still there.
    writeFile(dir.path() / "long.pdf", "%PDF-1.4\n");
    auto back = openOk(dir.path());
    CHECK(back->fileStateFor("long.pdf", 520).lastPage == 39);
}

TEST_CASE("reading_position an entry with only one of the two fields loads")
{
    TempDir dir;
    makePdfs(dir.path());
    writeFile(projectJson(dir.path()),
              R"({"version": 1, "open_file": null,
                  "files": {"long.pdf": {"last_page": 12},
                            "book/other.pdf": {"zoom": "fit-width"}},
                  "chats": []})");
    auto project = openOk(dir.path());
    CHECK(project->warnings().empty());

    const FileState a = project->fileStateFor("long.pdf", 520);
    CHECK(a.lastPage == 12);
    CHECK(a.zoom == "");
    const FileState b = project->fileStateFor("book/other.pdf", 12);
    CHECK(b.lastPage == 0);
    CHECK(b.zoom == "fit-width");
}

TEST_CASE("reading_position a read-only project keeps positions in memory only")
{
    TempDir dir;
    makePdfs(dir.path());
    const std::string newer =
        R"({"version": 2, "open_file": null, "files": {"long.pdf": {"last_page": 3, "zoom": "fit-page"}}, "chats": []})";
    writeFile(projectJson(dir.path()), newer);

    auto project = openOk(dir.path());
    REQUIRE(project->readOnly());
    CHECK(project->fileStateFor("long.pdf", 520).lastPage == 3);
    project->setFileState("long.pdf", FileState{39, customZoom(0.75)});
    CHECK(project->fileStateFor("long.pdf", 520).lastPage == 39);

    std::string error;
    CHECK_FALSE(project->save(error));
    CHECK(readFile(projectJson(dir.path())) == newer);
}
