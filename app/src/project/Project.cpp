#include "project/Project.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr const char* kReaderDir = ".reader";
constexpr const char* kProjectFile = "project.json";
constexpr const char* kChatsDir = "chats";

bool hasPdfExtension(const fs::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".pdf";
}

bool readText(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

fs::path tmpPathFor(const fs::path& path)
{
    fs::path tmp = path;
    tmp += ".tmp";
    return tmp;
}

// Moves a finished `tmp` over `path`, removing `tmp` on failure.
bool commitTmp(const fs::path& tmp, const fs::path& path, std::string& error)
{
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// Writes `content` to `path` through `path`.tmp and a rename.
bool writeAtomic(const fs::path& path, const std::string& content, std::string& error)
{
    const fs::path tmp = tmpPathFor(path);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        if (!out) {
            error = "cannot write " + tmp.string();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    return commitTmp(tmp, path, error);
}

// UTC time as 20261008T155613Z, safe in a file name.
std::string fileTimestamp()
{
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &utc);
    return buf;
}

bool validChatId(const std::string& id)
{
    return !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
}

json emptyProjectJson()
{
    return json{{"version", Project::kVersion},
                {"open_file", nullptr},
                {"files", json::object()},
                {"chats", json::array()}};
}

} // namespace

Project::Project(fs::path root)
    : m_root(std::move(root))
{
}

std::unique_ptr<Project> Project::open(const fs::path& folder, std::string& error)
{
    std::error_code ec;
    if (!fs::exists(folder, ec)) {
        error = "folder not found: " + folder.string();
        return nullptr;
    }
    if (!fs::is_directory(folder, ec)) {
        error = "not a folder: " + folder.string();
        return nullptr;
    }
    fs::create_directories(folder / kReaderDir, ec);
    if (ec) {
        error = "cannot create " + (folder / kReaderDir).string() + ": " + ec.message();
        return nullptr;
    }

    std::unique_ptr<Project> project(new Project(folder));
    project->load();
    return project;
}

void Project::load()
{
    const fs::path file = m_root / kReaderDir / kProjectFile;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return;
    }

    std::string text;
    if (!readText(file, text)) {
        m_readOnly = true;
        m_warnings.push_back("cannot read " + file.string() + ", project opened read-only");
        return;
    }

    // Text that is not JSON, a top level that is not an object, and known
    // fields of the wrong type all count as corrupt.
    try {
        const json j = json::parse(text);
        const bool shapeOk = j.is_object() && (!j.contains("files") || j["files"].is_object())
            && (!j.contains("chats") || j["chats"].is_array());
        if (!shapeOk) {
            throw std::invalid_argument("unexpected project.json shape");
        }

        const int version = j.value("version", kVersion);
        if (version > kVersion) {
            m_readOnly = true;
            m_warnings.push_back("project.json has version " + std::to_string(version)
                                 + ", newer than " + std::to_string(kVersion)
                                 + ". The project is read-only.");
        }

        std::string openFile;
        if (j.contains("open_file") && !j["open_file"].is_null()) {
            openFile = j["open_file"].get<std::string>();
        }

        std::map<std::string, FileState> files;
        if (j.contains("files")) {
            for (const auto& [rel, entry] : j.at("files").items()) {
                files[rel] = FileState{entry.value("last_page", 0), entry.value("zoom", "")};
            }
        }

        std::vector<ChatInfo> chats;
        if (j.contains("chats")) {
            for (const auto& entry : j.at("chats")) {
                ChatInfo chat;
                chat.id = entry.at("id").get<std::string>();
                chat.title = entry.value("title", "");
                if (entry.contains("session_id") && !entry["session_id"].is_null()) {
                    chat.sessionId = entry["session_id"].get<std::string>();
                }
                chat.created = entry.value("created", "");
                chat.updated = entry.value("updated", "");
                chats.push_back(std::move(chat));
            }
        }

        m_raw = j;
        m_openFile = std::move(openFile);
        m_files = std::move(files);
        m_chats = std::move(chats);
        return;
    } catch (const std::exception&) {
        // Falls through to recovery below.
    }

    m_readOnly = false;
    m_warnings.clear();
    m_raw = json::object();

    fs::path backup = file;
    backup += ".bak-" + fileTimestamp();
    for (int n = 1; fs::exists(backup, ec); ++n) {
        backup = file;
        backup += ".bak-" + fileTimestamp() + "-" + std::to_string(n);
    }
    fs::rename(file, backup, ec);
    if (ec) {
        m_readOnly = true;
        m_warnings.push_back("project.json is corrupt and cannot be moved aside ("
                             + ec.message() + "). The project is read-only.");
        return;
    }

    std::string error;
    if (!writeAtomic(file, emptyProjectJson().dump(2) + "\n", error)) {
        m_warnings.push_back("project.json was corrupt and moved to " + backup.filename().string()
                             + ". Writing a new one failed: " + error);
        return;
    }
    m_warnings.push_back("project.json was corrupt and moved to " + backup.filename().string()
                         + ". Started an empty project.");
}

std::vector<std::string> Project::pdfs() const
{
    std::vector<std::string> found;
    std::error_code ec;
    fs::recursive_directory_iterator it(m_root, fs::directory_options::skip_permission_denied, ec);
    for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code typeEc;
        if (entry.is_directory(typeEc)) {
            if (entry.path().filename().string().starts_with('.')) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (entry.is_regular_file(typeEc) && hasPdfExtension(entry.path())) {
            found.push_back(entry.path().lexically_relative(m_root).generic_string());
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

bool Project::save(std::string& error) const
{
    if (m_readOnly) {
        error = "project is read-only";
        return false;
    }

    json out = m_raw;
    out["version"] = kVersion;
    out["open_file"] = m_openFile.empty() ? json(nullptr) : json(m_openFile);

    const json* oldFiles =
        m_raw.contains("files") && m_raw["files"].is_object() ? &m_raw["files"] : nullptr;
    json files = json::object();
    for (const auto& [rel, state] : m_files) {
        json entry = json::object();
        if (oldFiles && oldFiles->contains(rel) && (*oldFiles)[rel].is_object()) {
            entry = (*oldFiles)[rel];
        }
        entry["last_page"] = state.lastPage;
        entry["zoom"] = state.zoom;
        files[rel] = std::move(entry);
    }
    out["files"] = std::move(files);

    json chats = json::array();
    for (const ChatInfo& chat : m_chats) {
        json entry = json::object();
        if (m_raw.contains("chats") && m_raw["chats"].is_array()) {
            for (const json& old : m_raw["chats"]) {
                if (old.is_object() && old.value("id", "") == chat.id) {
                    entry = old;
                    break;
                }
            }
        }
        entry["id"] = chat.id;
        entry["title"] = chat.title;
        entry["session_id"] = chat.sessionId ? json(*chat.sessionId) : json(nullptr);
        entry["created"] = chat.created;
        entry["updated"] = chat.updated;
        chats.push_back(std::move(entry));
    }
    out["chats"] = std::move(chats);

    return writeAtomic(m_root / kReaderDir / kProjectFile, out.dump(2) + "\n", error);
}

bool Project::appendMessage(const std::string& chatId, const ChatMessage& message,
                            std::string& error) const
{
    if (m_readOnly) {
        error = "project is read-only";
        return false;
    }
    if (!validChatId(chatId)) {
        error = "invalid chat id: " + chatId;
        return false;
    }

    const fs::path dir = m_root / kReaderDir / kChatsDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        error = "cannot create " + dir.string() + ": " + ec.message();
        return false;
    }

    json line{{"role", message.role}, {"text", message.text}, {"ts", message.ts}};
    if (message.costUsd) {
        line["cost_usd"] = *message.costUsd;
    }

    // A plain append, so the cost of a message does not grow with the
    // transcript. A crash can leave a torn last line, which transcript()
    // skips: the next message starts on a line of its own.
    const fs::path file = dir / (chatId + ".jsonl");
    // A stale .tmp from the copy-and-rename appends of earlier versions is
    // never part of the transcript.
    fs::remove(tmpPathFor(file), ec);
    bool tornLastLine = false;
    {
        std::ifstream in(file, std::ios::binary | std::ios::ate);
        if (in && in.tellg() > 0) {
            in.seekg(-1, std::ios::end);
            char last = '\n';
            in.get(last);
            tornLastLine = last != '\n';
        }
    }
    std::ofstream out(file, std::ios::binary | std::ios::app);
    if (tornLastLine)
        out << '\n';
    out << line.dump() << '\n';
    out.close();
    if (!out) {
        error = "cannot write " + file.string();
        return false;
    }
    return true;
}

bool Project::removeTranscript(const std::string& chatId, std::string& error) const
{
    if (m_readOnly) {
        error = "project is read-only";
        return false;
    }
    if (!validChatId(chatId)) {
        error = "invalid chat id: " + chatId;
        return false;
    }
    const fs::path file = m_root / kReaderDir / kChatsDir / (chatId + ".jsonl");
    std::error_code ec;
    fs::remove(tmpPathFor(file), ec);
    fs::remove(file, ec);
    if (ec) {
        error = "cannot delete " + file.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::vector<ChatMessage> Project::transcript(const std::string& chatId) const
{
    std::vector<ChatMessage> messages;
    if (!validChatId(chatId)) {
        return messages;
    }
    std::ifstream in(m_root / kReaderDir / kChatsDir / (chatId + ".jsonl"), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        try {
            const json j = json::parse(line);
            if (!j.is_object()) {
                continue;
            }
            ChatMessage message;
            message.role = j.value("role", "");
            message.text = j.value("text", "");
            message.ts = j.value("ts", "");
            if (j.contains("cost_usd") && !j["cost_usd"].is_null()) {
                message.costUsd = j["cost_usd"].get<double>();
            }
            messages.push_back(std::move(message));
        } catch (const json::exception&) {
            continue;
        }
    }
    return messages;
}
