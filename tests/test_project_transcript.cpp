// M6.1 spec item 4: chat transcripts in .reader/chats/<chat_id>.jsonl.
// Implementer-added tests. The tester's tests/test_project.cpp covers the rest.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / ("agentic-reader-test-transcript-" + std::to_string(stamp) + "-"
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

std::vector<std::string> readLines(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::unique_ptr<Project> openOk(const fs::path& root)
{
    std::string error;
    auto project = Project::open(root, error);
    INFO("open error: " << error);
    REQUIRE(project != nullptr);
    return project;
}

} // namespace

TEST_CASE("project transcript appends one JSON line per message and reads them back")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    CHECK(project->transcript("c1").empty());

    std::string error;
    REQUIRE(project->appendMessage("c1", ChatMessage{"user", "What is on page 3?", "2026-10-08T10:00:00Z", std::nullopt}, error));
    REQUIRE(project->appendMessage("c1", ChatMessage{"assistant", "A table.\nTwo rows.", "2026-10-08T10:00:05Z", 0.0125}, error));

    const fs::path file = tmp.path() / ".reader" / "chats" / "c1.jsonl";
    const auto lines = readLines(file);
    REQUIRE(lines.size() == 2);
    const json first = json::parse(lines[0]);
    CHECK(first["role"] == "user");
    CHECK(first["text"] == "What is on page 3?");
    CHECK(first["ts"] == "2026-10-08T10:00:00Z");
    CHECK_FALSE(first.contains("cost_usd"));
    const json second = json::parse(lines[1]);
    CHECK(second["text"] == "A table.\nTwo rows.");
    CHECK(second["cost_usd"] == doctest::Approx(0.0125));

    // No .tmp file is left behind.
    for (const auto& entry : fs::directory_iterator(file.parent_path())) {
        CHECK(entry.path().extension() != ".tmp");
    }

    const auto reopened = openOk(tmp.path());
    const auto messages = reopened->transcript("c1");
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].role == "user");
    CHECK_FALSE(messages[0].costUsd.has_value());
    CHECK(messages[1].role == "assistant");
    CHECK(messages[1].text == "A table.\nTwo rows.");
    CHECK(messages[1].ts == "2026-10-08T10:00:05Z");
    REQUIRE(messages[1].costUsd.has_value());
    CHECK(*messages[1].costUsd == doctest::Approx(0.0125));
}

TEST_CASE("project transcript skips lines that are not JSON objects")
{
    TempDir tmp;
    const fs::path dir = tmp.path() / ".reader" / "chats";
    fs::create_directories(dir);
    std::ofstream(dir / "c2.jsonl") << "{\"role\":\"user\",\"text\":\"hi\",\"ts\":\"t\"}\n"
                                    << "not json\n"
                                    << "[1]\n"
                                    << "{\"role\":\"assistant\",\"text\":\"yo\",\"ts\":\"t\"}\n";

    const auto project = openOk(tmp.path());
    const auto messages = project->transcript("c2");
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].text == "hi");
    CHECK(messages[1].text == "yo");
}

TEST_CASE("project transcript rejects chat ids that are not plain names")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    const ChatMessage message{"user", "x", "t", std::nullopt};
    for (const std::string id : {"", "../escape", "a/b", "a.b"}) {
        std::string error;
        CHECK_FALSE(project->appendMessage(id, message, error));
        CHECK_FALSE(error.empty());
    }
    CHECK_FALSE(fs::exists(tmp.path() / ".reader" / "escape.jsonl"));
}

TEST_CASE("project transcript append is refused on a read-only project")
{
    TempDir tmp;
    fs::create_directories(tmp.path() / ".reader");
    std::ofstream(tmp.path() / ".reader" / "project.json") << R"({"version": 2})";

    const auto project = openOk(tmp.path());
    REQUIRE(project->readOnly());
    std::string error;
    CHECK_FALSE(project->appendMessage("c1", ChatMessage{"user", "x", "t", std::nullopt}, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(tmp.path() / ".reader" / "chats" / "c1.jsonl"));
}
