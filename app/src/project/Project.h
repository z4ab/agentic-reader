#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Reading state of one PDF, stored under "files" in project.json.
struct FileState
{
    int lastPage = 0;
    std::string zoom;
};

// One entry of "chats" in project.json. Timestamps are ISO 8601 strings kept
// as given.
struct ChatInfo
{
    std::string id;
    std::string title;
    std::optional<std::string> sessionId;
    std::string created;
    std::string updated;
};

// One line of .reader/chats/<chat_id>.jsonl.
struct ChatMessage
{
    std::string role;
    std::string text;
    std::string ts;
    std::optional<double> costUsd;
};

// A project folder: the PDFs under it and its .reader/ state.
//
// Relative paths use '/' separators everywhere (pdfs(), openFile(), files()).
// Every write goes to a "*.tmp" file first and is renamed into place.
class Project
{
public:
    static constexpr int kVersion = 1;

    // Returns nullptr and sets `error` when `folder` is not an existing
    // directory or .reader/ cannot be created. A corrupt project.json is moved
    // to project.json.bak-<timestamp> and replaced by an empty project, with a
    // warning. A newer version opens read-only with a warning.
    static std::unique_ptr<Project> open(const std::filesystem::path& folder, std::string& error);

    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    const std::filesystem::path& root() const { return m_root; }

    // Every *.pdf (any letter case) under root(), skipping folders whose name
    // starts with '.', sorted by relative path.
    std::vector<std::string> pdfs() const;

    const std::string& openFile() const { return m_openFile; }
    void setOpenFile(const std::string& rel) { m_openFile = rel; }

    const std::map<std::string, FileState>& files() const { return m_files; }
    void setFileState(const std::string& rel, const FileState& state) { m_files[rel] = state; }

    const std::vector<ChatInfo>& chats() const { return m_chats; }
    void setChats(std::vector<ChatInfo> chats) { m_chats = std::move(chats); }

    const std::vector<std::string>& warnings() const { return m_warnings; }
    bool readOnly() const { return m_readOnly; }

    // Writes .reader/project.json, keeping unknown fields from the loaded file.
    // Returns false and sets `error` on a read-only project or an IO failure.
    bool save(std::string& error) const;

    // Appends one message to .reader/chats/<chatId>.jsonl. `chatId` may hold
    // only letters, digits, '-' and '_'. Returns false and sets `error` on a
    // read-only project, a bad id, or an IO failure.
    bool appendMessage(const std::string& chatId, const ChatMessage& message,
                       std::string& error) const;

    // The messages of one chat in file order. Empty when the transcript does
    // not exist. Lines that are not a JSON object are skipped.
    std::vector<ChatMessage> transcript(const std::string& chatId) const;

private:
    explicit Project(std::filesystem::path root);

    void load();

    std::filesystem::path m_root;
    std::string m_openFile;
    std::map<std::string, FileState> m_files;
    std::vector<ChatInfo> m_chats;
    std::vector<std::string> m_warnings;
    bool m_readOnly = false;

    // The loaded project.json, so save() keeps fields this version ignores.
    nlohmann::json m_raw = nlohmann::json::object();
};
