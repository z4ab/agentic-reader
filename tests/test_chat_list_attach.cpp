// M6.4 (implementer): ChatList::attach, which MainFrame uses when an unsaved
// project is saved as a project, and the in-memory fallback of transcripts
// whose write failed.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "chat/ChatList.h"
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

class TempDir
{
public:
    TempDir()
    {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / ("agentic-reader-test-chat-list-attach-" + std::to_string(stamp) + "-"
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

protocol::Result resultFor(const std::string& chatId, const std::string& sessionId)
{
    protocol::Result result;
    result.chatId = chatId;
    result.sessionId = sessionId;
    result.costUsd = 0.002;
    return result;
}

} // namespace

TEST_CASE("chat_list_attach: chats of an unsaved project are written when it is saved")
{
    TempDir tmp;
    ChatList list(nullptr);
    std::string error;
    const std::string a = list.create(error);
    REQUIRE(list.addUserMessage(a, "Unsaved question", error));
    REQUIRE(list.addResult(a, "Unsaved answer", resultFor(a, "sess-a"), error));
    const std::string b = list.create(error);
    REQUIRE(list.rename(b, "Empty chat", error));
    CHECK_FALSE(fs::exists(tmp.path() / ".reader"));

    auto project = Project::open(tmp.path(), error);
    REQUIRE(project != nullptr);
    REQUIRE(list.attach(project.get(), error));
    INFO("error: " << error);

    // Written through the project from now on.
    REQUIRE(list.addUserMessage(a, "Saved question", error));

    auto reopened = Project::open(tmp.path(), error);
    REQUIRE(reopened != nullptr);
    ChatList again(reopened.get());
    REQUIRE(again.find(a) != nullptr);
    REQUIRE(again.find(b) != nullptr);
    CHECK(again.find(a)->title == "Unsaved question");
    CHECK(again.find(a)->sessionId == std::optional<std::string>("sess-a"));
    CHECK(again.find(b)->title == "Empty chat");
    const auto transcript = again.transcript(a);
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].text == "Unsaved question");
    CHECK(transcript[1].text == "Unsaved answer");
    CHECK(transcript[2].text == "Saved question");
    CHECK(again.transcript(b).empty());
}

TEST_CASE("chat_list_attach: a read-only project still shows the messages of this session")
{
    TempDir tmp;
    fs::create_directories(tmp.path() / ".reader");
    std::ofstream(tmp.path() / ".reader" / "project.json") << R"({"version": 99, "chats": []})";
    std::string error;
    auto project = Project::open(tmp.path(), error);
    REQUIRE(project != nullptr);
    REQUIRE(project->readOnly());

    ChatList list(project.get());
    const std::string id = list.create(error);
    CHECK_FALSE(list.addUserMessage(id, "Kept in memory", error));
    CHECK_FALSE(list.addResult(id, "Also in memory", resultFor(id, "s"), error));
    const auto transcript = list.transcript(id);
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].text == "Kept in memory");
    CHECK(transcript[1].text == "Also in memory");
}
