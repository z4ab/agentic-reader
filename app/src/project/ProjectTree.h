#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The folder and PDF rows of the project panel, and the choice of project
// folder for a PDF. No wx types, so tests can use them without a GUI.

// One row of the project tree: a folder or a PDF.
struct ProjectTreeNode
{
    std::string name;    // last path component, e.g. "nested.pdf"
    std::string rel;     // path relative to the project root with '/'
                         // separators, e.g. "papers/deep/nested.pdf"
    bool folder = false; // true for a folder, false for a PDF
    std::vector<ProjectTreeNode> children; // always empty for a PDF
};

// The top level of the tree under `root` (root itself is not a node). PDFs are
// the files Project::pdfs() lists. A folder appears only when a PDF sits
// somewhere below it. Within a folder, subfolders come first, then PDFs, each
// sorted by name. Empty for a missing root. Never writes anything.
std::vector<ProjectTreeNode> buildProjectTree(const std::filesystem::path& root);

// The folders under `root` that can hold a PDF of the tree: root itself and
// every subfolder that is not hidden, whether or not it holds a PDF yet. The
// project panel watches these for changes.
std::vector<std::filesystem::path> projectFolders(const std::filesystem::path& root);

// The nearest folder holding a `.reader` directory, starting at the PDF's own
// folder and walking up to the filesystem root. std::nullopt when there is
// none.
std::optional<std::filesystem::path> findProjectRoot(const std::filesystem::path& pdf);

// The project folder a PDF opens in. `current` is the root of the open
// project, saved or unsaved, or empty when none is open. A PDF anywhere under
// `current` stays there. Otherwise the nearest saved project
// (findProjectRoot()), else the PDF's own folder, which opens as an unsaved
// project. Paths are absolute and lexically normal.
std::filesystem::path projectFolderFor(const std::filesystem::path& pdf,
                                       const std::filesystem::path& current);
