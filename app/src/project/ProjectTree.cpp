#include "project/ProjectTree.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

bool hasPdfExtension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pdf";
}

// A folder the tree descends into. Like Project::pdfs(): hidden folders are
// skipped and symlinked folders are not followed.
bool isTreeFolder(const fs::directory_entry& entry)
{
    std::error_code ec;
    return entry.is_directory(ec) && !entry.is_symlink(ec)
        && !entry.path().filename().string().starts_with('.');
}

std::vector<ProjectTreeNode> scanFolder(const fs::path& dir, const std::string& rel)
{
    std::vector<ProjectTreeNode> folders;
    std::vector<ProjectTreeNode> pdfs;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();
        const std::string childRel = rel.empty() ? name : rel + "/" + name;
        std::error_code typeEc;
        if (entry.is_directory(typeEc)) {
            if (!isTreeFolder(entry)) {
                continue;
            }
            std::vector<ProjectTreeNode> children = scanFolder(entry.path(), childRel);
            if (!children.empty()) {
                folders.push_back({name, childRel, true, std::move(children)});
            }
        } else if (entry.is_regular_file(typeEc) && hasPdfExtension(entry.path())) {
            pdfs.push_back({name, childRel, false, {}});
        }
    }

    auto byName = [](const ProjectTreeNode& a, const ProjectTreeNode& b) {
        return a.name < b.name;
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(pdfs.begin(), pdfs.end(), byName);
    folders.insert(folders.end(), std::make_move_iterator(pdfs.begin()),
                   std::make_move_iterator(pdfs.end()));
    return folders;
}

void collectFolders(const fs::path& dir, std::vector<fs::path>& out)
{
    out.push_back(dir);
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (const fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        if (isTreeFolder(*it)) {
            collectFolders(it->path(), out);
        }
    }
}

// True when `path` is strictly below `folder`, compared component by component.
bool isUnder(const fs::path& path, const fs::path& folder)
{
    const fs::path rel = path.lexically_relative(folder);
    return !rel.empty() && rel != "." && *rel.begin() != "..";
}

} // namespace

std::vector<ProjectTreeNode> buildProjectTree(const fs::path& root)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        return {};
    }
    return scanFolder(root, std::string());
}

std::vector<fs::path> projectFolders(const fs::path& root)
{
    std::vector<fs::path> folders;
    std::error_code ec;
    if (fs::is_directory(root, ec)) {
        collectFolders(root, folders);
    }
    return folders;
}

std::optional<fs::path> findProjectRoot(const fs::path& pdf)
{
    std::error_code ec;
    for (fs::path dir = pdf.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_directory(dir / ".reader", ec)) {
            return dir;
        }
        if (dir == dir.parent_path()) {
            break;
        }
    }
    return std::nullopt;
}

fs::path projectFolderFor(const fs::path& pdf, const fs::path& current)
{
    if (!current.empty() && isUnder(pdf, current)) {
        return current;
    }
    if (std::optional<fs::path> root = findProjectRoot(pdf)) {
        return *root;
    }
    return pdf.parent_path();
}
