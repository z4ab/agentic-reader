// M6.2: spec tests for the GUI-free part of the project panel.
//
// The panel itself (wxTreeCtrl, wxFileSystemWatcher, the header with "Save as
// project", File > Open Project and Recent Projects, reopening the last
// project at start) is wx code. tests/acceptance/m6.2.sh drives it headless
// (see tests/acceptance/m6.2.steps). This file covers the two decisions the
// panel and MainFrame make without the GUI: which entries the tree shows, and
// which folder a PDF opens in.
//
// Fixture folders are built in a fresh temp directory per test case. No test
// data is committed.
//
// Interface choices where the spec is silent. Declared in
// app/src/project/ProjectTree.h, built into reader_core, no wx types:
//
//     // One row of the project tree: a folder or a PDF.
//     struct ProjectTreeNode
//     {
//         std::string name;    // last path component, e.g. "nested.pdf"
//         std::string rel;     // path relative to the project root with '/'
//                              // separators, e.g. "papers/deep/nested.pdf"
//         bool folder = false; // true for a folder, false for a PDF
//         std::vector<ProjectTreeNode> children; // always empty for a PDF
//     };
//
//     // The top level of the tree under `root` (root itself is not a node).
//     std::vector<ProjectTreeNode> buildProjectTree(const std::filesystem::path& root);
//
//     // The nearest folder that holds a `.reader` directory, starting at the
//     // PDF's own folder and walking up to the filesystem root. std::nullopt
//     // when there is none.
//     std::optional<std::filesystem::path> findProjectRoot(const std::filesystem::path& pdf);
//
//     // The project folder a PDF opens in. `current` is the root of the open
//     // project, saved or unsaved, or empty when none is open.
//     std::filesystem::path projectFolderFor(const std::filesystem::path& pdf,
//                                            const std::filesystem::path& current);
//
//   buildProjectTree():
//   - PDFs are exactly the files Project::pdfs() lists (regular files with a
//     ".pdf" extension in any letter case; folders whose name starts with '.'
//     are skipped at any depth, .reader/ among them). Other files never
//     appear.
//   - A folder appears only when a PDF sits somewhere below it. Folders with
//     no PDF at any depth are left out, so every leaf is a PDF.
//   - Order within one folder: subfolders first, then PDFs, each group sorted
//     by name with plain std::string comparison.
//   - A missing or empty root gives an empty vector. It only reads: it never
//     creates .reader/ or anything else.
//   - The panel shows these nodes in a wxTreeCtrl with a hidden root and every
//     folder expanded, so a PDF's row is reachable without expanding anything
//     (the steps file relies on this).
//
//   findProjectRoot() / projectFolderFor() (spec item 5, "a single PDF outside
//   any project"):
//   - A PDF is inside a project when it is anywhere under the open project's
//     root (any depth), or when its folder or one of that folder's ancestors
//     holds a `.reader` directory. A file named `.reader` does not count.
//   - projectFolderFor() returns `current` when the PDF is under `current`
//     (component-wise: /x/proj2/a.pdf is not under /x/proj). Otherwise it
//     returns findProjectRoot(pdf) when that finds one, else the PDF's own
//     folder, which MainFrame then opens as an unsaved project (no .reader/).
//   - Paths are absolute and lexically normal. Results compare equal (==) to
//     `current`, to the found folder, or to pdf.parent_path(); no symlink
//     resolution is needed.
//
// Not covered here:
//   - The 300 ms refresh debounce. It is a restarted one-shot wxTimer in the
//     panel, with no logic worth a separate interface. The steps check the
//     visible result: the tree is up to date about 1 s after a change.
//   - Recent and last project persistence. That is wxFileHistory and wxConfig
//     like M2.5's recent files, checked by the restart run of m6.2.sh.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/Project.h"
#include "project/ProjectTree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
        const fs::path path = fs::temp_directory_path()
            / ("agentic-reader-test-project-panel-" + std::to_string(stamp) + "-"
               + std::to_string(counter++));
        fs::create_directories(path);
        m_path = fs::canonical(path);
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

void writeFile(const fs::path& path, const std::string& content = "x")
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << content;
}

// The fixture of tests/acceptance/m6.2.sh in miniature.
//   alpha.pdf beta.PDF notes.txt readme.md
//   papers/long.pdf papers/data.csv papers/deep/nested.pdf
//   empty/                (no files)
//   misc/todo.txt         (no PDF)
//   folder.pdf/notes.txt  (a folder named like a PDF, no PDF inside)
//   .hidden/secret.pdf    (hidden folder)
//   .reader/stray.pdf     (the project's own state folder)
void makeFixture(const fs::path& root)
{
    writeFile(root / "alpha.pdf");
    writeFile(root / "beta.PDF");
    writeFile(root / "notes.txt");
    writeFile(root / "readme.md");
    writeFile(root / "papers" / "long.pdf");
    writeFile(root / "papers" / "data.csv");
    writeFile(root / "papers" / "deep" / "nested.pdf");
    fs::create_directories(root / "empty");
    writeFile(root / "misc" / "todo.txt");
    writeFile(root / "folder.pdf" / "notes.txt");
    writeFile(root / ".hidden" / "secret.pdf");
    writeFile(root / ".reader" / "stray.pdf");
}

std::vector<std::string> names(const std::vector<ProjectTreeNode>& nodes)
{
    std::vector<std::string> out;
    for (const ProjectTreeNode& node : nodes) {
        out.push_back(node.name);
    }
    return out;
}

void collectLeaves(const std::vector<ProjectTreeNode>& nodes, std::vector<std::string>& out)
{
    for (const ProjectTreeNode& node : nodes) {
        if (node.folder) {
            collectLeaves(node.children, out);
        } else {
            out.push_back(node.rel);
        }
    }
}

// Every node, depth first, as "rel" plus "/" for folders.
void collectAll(const std::vector<ProjectTreeNode>& nodes, std::vector<std::string>& out)
{
    for (const ProjectTreeNode& node : nodes) {
        out.push_back(node.folder ? node.rel + "/" : node.rel);
        if (node.folder) {
            collectAll(node.children, out);
        }
    }
}

bool anyAncestorHasReader(const fs::path& dir)
{
    std::error_code ec;
    for (fs::path p = dir;; p = p.parent_path()) {
        if (fs::is_directory(p / ".reader", ec)) {
            return true;
        }
        if (p == p.parent_path()) {
            return false;
        }
    }
}

} // namespace

TEST_CASE("project_panel: tree lists folders and PDFs only, folders first")
{
    TempDir tmp;
    makeFixture(tmp.path());

    const std::vector<ProjectTreeNode> tree = buildProjectTree(tmp.path());

    REQUIRE(names(tree) == std::vector<std::string>{"papers", "alpha.pdf", "beta.PDF"});

    const ProjectTreeNode& papers = tree[0];
    CHECK(papers.folder);
    CHECK(papers.rel == "papers");
    REQUIRE(names(papers.children) == std::vector<std::string>{"deep", "long.pdf"});

    const ProjectTreeNode& deep = papers.children[0];
    CHECK(deep.folder);
    CHECK(deep.rel == "papers/deep");
    REQUIRE(names(deep.children) == std::vector<std::string>{"nested.pdf"});
    CHECK_FALSE(deep.children[0].folder);
    CHECK(deep.children[0].rel == "papers/deep/nested.pdf");
    CHECK(deep.children[0].children.empty());

    const ProjectTreeNode& longPdf = papers.children[1];
    CHECK_FALSE(longPdf.folder);
    CHECK(longPdf.rel == "papers/long.pdf");

    CHECK_FALSE(tree[1].folder);
    CHECK(tree[1].rel == "alpha.pdf");
    CHECK_FALSE(tree[2].folder);
    CHECK(tree[2].rel == "beta.PDF");

    std::vector<std::string> all;
    collectAll(tree, all);
    CHECK(all
          == std::vector<std::string>{"papers/", "papers/deep/", "papers/deep/nested.pdf",
                                      "papers/long.pdf", "alpha.pdf", "beta.PDF"});
}

TEST_CASE("project_panel: tree leaves are exactly Project::pdfs()")
{
    TempDir tmp;
    makeFixture(tmp.path());
    writeFile(tmp.path() / "papers" / "deep" / "deeper" / "Upper.Pdf");
    writeFile(tmp.path() / "papers" / ".cache" / "cached.pdf");

    std::string error;
    auto project = Project::open(tmp.path(), error);
    INFO("open error: " << error);
    REQUIRE(project != nullptr);

    std::vector<std::string> leaves;
    collectLeaves(buildProjectTree(tmp.path()), leaves);
    std::sort(leaves.begin(), leaves.end());

    CHECK(leaves == project->pdfs());
    CHECK(std::find(leaves.begin(), leaves.end(), "papers/deep/deeper/Upper.Pdf") != leaves.end());
}

TEST_CASE("project_panel: tree orders subfolders before PDFs, each by name")
{
    TempDir tmp;
    writeFile(tmp.path() / "m.pdf");
    writeFile(tmp.path() / "b.pdf");
    writeFile(tmp.path() / "zz" / "x.pdf");
    writeFile(tmp.path() / "aa" / "y.pdf");
    writeFile(tmp.path() / "aa" / "sub" / "z.pdf");
    writeFile(tmp.path() / "aa" / "c.pdf");

    const std::vector<ProjectTreeNode> tree = buildProjectTree(tmp.path());

    REQUIRE(names(tree) == std::vector<std::string>{"aa", "zz", "b.pdf", "m.pdf"});
    CHECK(names(tree[0].children) == std::vector<std::string>{"sub", "c.pdf", "y.pdf"});
    CHECK(names(tree[1].children) == std::vector<std::string>{"x.pdf"});
}

TEST_CASE("project_panel: folders without any PDF below are left out")
{
    TempDir tmp;
    fs::create_directories(tmp.path() / "empty");
    writeFile(tmp.path() / "misc" / "todo.txt");
    fs::create_directories(tmp.path() / "outer" / "inner" / "innermost");
    writeFile(tmp.path() / "outer" / "inner" / "notes.txt");
    writeFile(tmp.path() / "folder.pdf" / "notes.txt");
    writeFile(tmp.path() / "keep" / "deeper" / "only.pdf");

    const std::vector<ProjectTreeNode> tree = buildProjectTree(tmp.path());

    std::vector<std::string> all;
    collectAll(tree, all);
    CHECK(all == std::vector<std::string>{"keep/", "keep/deeper/", "keep/deeper/only.pdf"});
}

TEST_CASE("project_panel: empty or missing root gives an empty tree")
{
    TempDir tmp;
    CHECK(buildProjectTree(tmp.path()).empty());

    writeFile(tmp.path() / "notes.txt");
    writeFile(tmp.path() / ".hidden" / "secret.pdf");
    CHECK(buildProjectTree(tmp.path()).empty());

    CHECK(buildProjectTree(tmp.path() / "does-not-exist").empty());
}

TEST_CASE("project_panel: building the tree never creates .reader/")
{
    TempDir tmp;
    writeFile(tmp.path() / "single.pdf");
    writeFile(tmp.path() / "other.txt");

    const std::vector<ProjectTreeNode> tree = buildProjectTree(tmp.path());

    REQUIRE(names(tree) == std::vector<std::string>{"single.pdf"});
    CHECK_FALSE(fs::exists(tmp.path() / ".reader"));
}

TEST_CASE("project_panel: findProjectRoot finds the nearest folder with .reader/")
{
    TempDir tmp;
    if (anyAncestorHasReader(tmp.path())) {
        MESSAGE("skipped: an ancestor of " << tmp.path().string() << " holds .reader/");
        return;
    }
    const fs::path proj = tmp.path() / "proj";
    writeFile(proj / "alpha.pdf");
    writeFile(proj / "papers" / "deep" / "nested.pdf");
    fs::create_directories(proj / ".reader");
    const fs::path loose = tmp.path() / "loose";
    writeFile(loose / "single.pdf");

    SUBCASE("PDF in the project folder")
    {
        CHECK(findProjectRoot(proj / "alpha.pdf") == std::optional<fs::path>(proj));
    }
    SUBCASE("PDF in a nested folder of the project")
    {
        CHECK(findProjectRoot(proj / "papers" / "deep" / "nested.pdf")
              == std::optional<fs::path>(proj));
    }
    SUBCASE("the nearest .reader/ wins")
    {
        fs::create_directories(proj / "papers" / ".reader");
        CHECK(findProjectRoot(proj / "papers" / "deep" / "nested.pdf")
              == std::optional<fs::path>(proj / "papers"));
    }
    SUBCASE("PDF outside any project")
    {
        CHECK(findProjectRoot(loose / "single.pdf") == std::nullopt);
        CHECK_FALSE(fs::exists(loose / ".reader"));
    }
    SUBCASE("a file named .reader does not make a project")
    {
        writeFile(loose / ".reader");
        CHECK(findProjectRoot(loose / "single.pdf") == std::nullopt);
    }
}

TEST_CASE("project_panel: projectFolderFor picks the folder a PDF opens in")
{
    TempDir tmp;
    if (anyAncestorHasReader(tmp.path())) {
        MESSAGE("skipped: an ancestor of " << tmp.path().string() << " holds .reader/");
        return;
    }
    const fs::path proj = tmp.path() / "proj";
    writeFile(proj / "alpha.pdf");
    writeFile(proj / "papers" / "deep" / "nested.pdf");
    fs::create_directories(proj / ".reader");
    const fs::path proj2 = tmp.path() / "proj2";
    writeFile(proj2 / "neighbour.pdf");
    const fs::path loose = tmp.path() / "loose";
    writeFile(loose / "single.pdf");
    writeFile(loose / "sub" / "child.pdf");

    SUBCASE("inside the open project at any depth: the open project")
    {
        CHECK(projectFolderFor(proj / "alpha.pdf", proj) == proj);
        CHECK(projectFolderFor(proj / "papers" / "deep" / "nested.pdf", proj) == proj);
    }
    SUBCASE("inside an open unsaved project: that project, still without .reader/")
    {
        CHECK(projectFolderFor(loose / "sub" / "child.pdf", loose) == loose);
        CHECK_FALSE(fs::exists(loose / ".reader"));
    }
    SUBCASE("in a saved project that is not open: that project's root")
    {
        CHECK(projectFolderFor(proj / "papers" / "deep" / "nested.pdf", loose) == proj);
        CHECK(projectFolderFor(proj / "papers" / "deep" / "nested.pdf", fs::path()) == proj);
    }
    SUBCASE("outside any project: the PDF's own folder")
    {
        CHECK(projectFolderFor(loose / "single.pdf", proj) == loose);
        CHECK(projectFolderFor(loose / "sub" / "child.pdf", proj) == loose / "sub");
        CHECK(projectFolderFor(loose / "single.pdf", fs::path()) == loose);
        CHECK_FALSE(fs::exists(loose / ".reader"));
        CHECK_FALSE(fs::exists(loose / "sub" / ".reader"));
    }
    SUBCASE("a sibling folder that shares a name prefix is not inside")
    {
        CHECK(projectFolderFor(proj2 / "neighbour.pdf", proj) == proj2);
    }
}
