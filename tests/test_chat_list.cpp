// M9.2: spec tests for chatlist::neighbor, the step to the next or previous
// chat that Ctrl+Tab and Ctrl+Shift+Tab use. tests/acceptance/m9.2.sh checks
// the keys on screen.
//
// Interface choice where the spec is silent (the implementer conforms or
// files a TEST DISPUTE). Declared in app/src/chat/ChatList.h, in namespace
// chatlist, linked into reader_core:
//
//   // The id `step` places from `current` in `ids`, in the order given (the
//   // dropdown order, newest first, is passed as is and not sorted), wrapping
//   // at both ends. A positive step goes forward, a negative one back, and a
//   // step of any size works. Returns `current` when `ids` has fewer than two
//   // entries (an empty list included) or does not contain `current`.
//   std::string neighbor(const std::vector<std::string>& ids,
//                        const std::string& current, int step);
//
// M6.4: spec tests for the chat list model (GUI-free): ordering, titles,
// persistence of chats and transcripts, session ids, delete, the "Continue in
// new chat" prompt, and the two carried M6.1 notes on Project::appendMessage.
// tests/acceptance/m6.4.sh checks what the user sees.
//
// Interface choices where the spec is silent (the implementer conforms or
// files a TEST DISPUTE). Everything is declared in app/src/chat/ChatList.h
// (the spec's new file) and links into reader_core:
//
//   #include "agent/Protocol.h"
//   #include "project/Project.h"   // ChatInfo, ChatMessage, Project
//
//   namespace chatlist
//   {
//   constexpr std::size_t kTitleChars = 60;      // Unicode code points
//   constexpr std::size_t kContextMessages = 20; // for continuePrompt()
//
//   // A random RFC 4122 version 4 UUID, lower-case hex, 8-4-4-4-12.
//   std::string newChatId();
//
//   // The current UTC time as ISO 8601: "YYYY-MM-DDTHH:MM:SS" with optional
//   // fractional seconds, then "Z". Used for created, updated and ts.
//   std::string utcNow();
//
//   // The title of a chat whose first user message is `text`: line breaks
//   // and tabs become spaces, leading and trailing whitespace is dropped,
//   // then the first kTitleChars code points are kept. Never cuts a UTF-8
//   // sequence. No ellipsis is added.
//   std::string titleFor(const std::string& text);
//
//   // The first prompt of a chat continued from one that cannot resume: the
//   // last kContextMessages messages of `transcript` (all of them when there
//   // are fewer), oldest first, as context, followed by `text`. Each
//   // message's text appears unchanged. An empty transcript gives exactly
//   // `text`. The wording around the messages is free.
//   std::string continuePrompt(const std::vector<ChatMessage>& transcript,
//                              const std::string& text);
//
//   // The message that opens `chat` in the sidecar, at selection and again
//   // after a sidecar restart: resume_chat {id, session_id} when the chat
//   // has a non-empty session id. A chat without one (its first reply
//   // failed, so no `result` brought a session_id, M4.5 carried note) opens
//   // with new_chat {id} under its own id; its transcript stays and its
//   // first `result` stores the session id. seq is left empty.
//   protocol::Inbound openMessage(const ChatInfo& chat);
//   } // namespace chatlist
//
//   // The chats of the open project. With a Project, every change is written
//   // through it at once: project.json via setChats() + save(), transcripts
//   // via appendMessage() into .reader/chats/<id>.jsonl. With a null project
//   // (an unsaved project) chats and transcripts live in memory only and
//   // nothing is written anywhere.
//   class ChatList
//   {
//   public:
//       // Returns the timestamp for created, updated and each message's ts.
//       using Clock = std::function<std::string()>;
//
//       // `project` may be null and must otherwise outlive the list. Starts
//       // from project->chats(). An empty `clock` means chatlist::utcNow.
//       explicit ChatList(Project* project, Clock clock = {});
//
//       // Every chat, newest `updated` first (ISO 8601 strings compare as
//       // text). The order of equal timestamps is not specified.
//       std::vector<ChatInfo> chats() const;
//       // Null for an unknown id.
//       const ChatInfo* find(const std::string& id) const;
//
//       // Spec 2: a new chat with a newChatId() id, an empty title, created
//       // and updated set to now and no session id, added to project.json.
//       // Returns the id. The chat is added even when the write fails; then
//       // `error` says why (it is cleared on success).
//       std::string create(std::string& error);
//
//       // Spec 3, after every user message: appends {"user", text, now} to
//       // the transcript, sets updated to now, and sets the title to
//       // titleFor(text) while the title is empty.
//       bool addUserMessage(const std::string& id, const std::string& text,
//                           std::string& error);
//
//       // Spec 3, after every `result` (never per text_delta): appends
//       // {"assistant", replyText, now, result.costUsd} to the transcript,
//       // sets updated to now, and stores result.sessionId while the chat
//       // has none, so the first result's session id sticks.
//       bool addResult(const std::string& id, const std::string& replyText,
//                      const protocol::Result& result, std::string& error);
//
//       bool rename(const std::string& id, const std::string& title, std::string& error);
//
//       // Spec 5: drops the entry from project.json and deletes
//       // .reader/chats/<id>.jsonl. Touches nothing else (not the agent
//       // CLI's own session files under ~/.claude).
//       bool remove(const std::string& id, std::string& error);
//
//       // The chat's messages in order: from the JSONL file with a project,
//       // from memory without one. Empty for an unknown id.
//       std::vector<ChatMessage> transcript(const std::string& id) const;
//   };
//
// The mutators return false and set `error` for an unknown id (nothing
// changes) or a failed write (the change is kept in memory, so the chat goes
// on, as for a read-only project).
//
// Carried notes from the M6.1 review (Project::appendMessage):
//   - Append cost: only finished messages are appended, one line each (one
//     per user message, one per `result`), never per text_delta. Checked
//     here by line counts and in the acceptance run against a fake sidecar
//     that streams several deltas per reply. If appends move to plain
//     O_APPEND, a torn last line (no '\n', left by a crash) must not swallow
//     the next message: it starts on a line of its own.
//   - A stale "<id>.jsonl.tmp" left by a crash while "<id>.jsonl" does not
//     exist must not leak into the transcript.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "agent/Protocol.h"
#include "chat/ChatList.h"
#include "project/Project.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <variant>
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
            / ("agentic-reader-test-chat-list-" + std::to_string(stamp) + "-"
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

// Sets an environment variable for one scope.
class ScopedEnv
{
public:
    ScopedEnv(const char* name, const std::string& value)
        : m_name(name)
    {
        if (const char* old = std::getenv(name)) {
            m_old = old;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv()
    {
        if (m_old) {
            ::setenv(m_name, m_old->c_str(), 1);
        } else {
            ::unsetenv(m_name);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* m_name;
    std::optional<std::string> m_old;
};

// Changes the working directory for one scope.
class ScopedCwd
{
public:
    explicit ScopedCwd(const fs::path& dir)
        : m_old(fs::current_path())
    {
        fs::current_path(dir);
    }
    ~ScopedCwd()
    {
        std::error_code ec;
        fs::current_path(m_old, ec);
    }
    ScopedCwd(const ScopedCwd&) = delete;
    ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
    fs::path m_old;
};

// A clock the test sets by hand: ChatList stamps whatever `now` holds.
struct FakeClock
{
    std::string now = "2026-10-12T10:00:00Z";
    ChatList::Clock clock()
    {
        return [this] { return now; };
    }
};

std::unique_ptr<Project> openOk(const fs::path& root)
{
    std::string error;
    auto project = Project::open(root, error);
    INFO("open error: " << error);
    REQUIRE(project != nullptr);
    return project;
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    REQUIRE(out.good());
}

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

fs::path projectJson(const fs::path& root)
{
    return root / ".reader" / "project.json";
}

fs::path jsonlPath(const fs::path& root, const std::string& id)
{
    return root / ".reader" / "chats" / (id + ".jsonl");
}

// The "chats" entry with `id` in project.json on disk, or null when absent.
json diskChat(const fs::path& root, const std::string& id)
{
    const json j = json::parse(readFile(projectJson(root)));
    REQUIRE(j.contains("chats"));
    REQUIRE(j["chats"].is_array());
    for (const json& entry : j["chats"]) {
        if (entry.value("id", "") == id) {
            return entry;
        }
    }
    return nullptr;
}

std::size_t diskChatCount(const fs::path& root)
{
    const json j = json::parse(readFile(projectJson(root)));
    return j.contains("chats") ? j["chats"].size() : 0;
}

std::vector<std::string> ids(const std::vector<ChatInfo>& chats)
{
    std::vector<std::string> out;
    for (const ChatInfo& chat : chats) {
        out.push_back(chat.id);
    }
    return out;
}

protocol::Result resultFor(const std::string& chatId, const std::string& sessionId,
                           double cost = 0.0125)
{
    protocol::Result result;
    result.chatId = chatId;
    result.sessionId = sessionId;
    result.usage = json{{"input_tokens", 10}, {"output_tokens", 5}};
    result.costUsd = cost;
    return result;
}

// Number of Unicode code points in valid UTF-8 `s`. Fails the test on bytes
// that are not valid UTF-8, such as a cut sequence.
std::size_t codePoints(const std::string& s)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 1;
        if (c >= 0xF0) {
            len = 4;
        } else if (c >= 0xE0) {
            len = 3;
        } else if (c >= 0xC0) {
            len = 2;
        } else {
            REQUIRE_MESSAGE(c < 0x80, "stray continuation byte at " << i);
        }
        REQUIRE_MESSAGE(i + len <= s.size(), "UTF-8 sequence cut at " << i);
        for (std::size_t k = 1; k < len; ++k) {
            REQUIRE((static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80);
        }
        i += len;
        ++count;
    }
    return count;
}

std::string repeat(const std::string& piece, std::size_t n)
{
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out += piece;
    }
    return out;
}

bool dirIsEmpty(const fs::path& dir)
{
    return fs::directory_iterator(dir) == fs::directory_iterator();
}

} // namespace

// ---- Free helpers -------------------------------------------------------

TEST_CASE("chat_list: newChatId is a lower-case version 4 UUID, unique, and a valid transcript id")
{
    const std::regex uuid("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    std::set<std::string> seen;
    for (int i = 0; i < 500; ++i) {
        const std::string id = chatlist::newChatId();
        INFO("id: " << id);
        CHECK(std::regex_match(id, uuid));
        seen.insert(id);
    }
    CHECK(seen.size() == 500);

    TempDir tmp;
    const auto project = openOk(tmp.path());
    const std::string id = chatlist::newChatId();
    std::string error;
    CHECK(project->appendMessage(id, ChatMessage{"user", "hi", "2026-10-12T10:00:00Z", std::nullopt}, error));
    CHECK(fs::exists(jsonlPath(tmp.path(), id)));
}

TEST_CASE("chat_list: utcNow is an ISO 8601 UTC timestamp")
{
    const std::string now = chatlist::utcNow();
    INFO("now: " << now);
    CHECK(std::regex_match(now, std::regex(R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d+)?Z$)")));
    CHECK(now.substr(0, 2) == "20");
}

TEST_CASE("chat_list: titleFor keeps a short message whole")
{
    CHECK(chatlist::titleFor("What is on page 3?") == "What is on page 3?");
    CHECK(chatlist::titleFor(std::string(60, 'a')) == std::string(60, 'a'));
}

TEST_CASE("chat_list: titleFor keeps the first 60 characters, without an ellipsis")
{
    CHECK(chatlist::kTitleChars == 60);
    const std::string text = "Summarize the method section and compare it with the baseline in table 2 please";
    REQUIRE(text.size() > 60);
    CHECK(chatlist::titleFor(text) == text.substr(0, 60));
    CHECK(chatlist::titleFor(std::string(70, 'x')) == std::string(60, 'x'));
}

TEST_CASE("chat_list: titleFor counts characters, not bytes, and never cuts a UTF-8 sequence")
{
    const std::string e = "\xc3\xa9";             // é, 2 bytes
    const std::string ja = "\xe6\x97\xa5";        // 日, 3 bytes
    const std::string smile = "\xf0\x9f\x98\x80"; // 😀, 4 bytes

    // 59 ASCII then é: the 60th character is é, whole.
    CHECK(chatlist::titleFor(std::string(59, 'a') + e + "bbb") == std::string(59, 'a') + e);
    // 58 ASCII then 日 then more: 日 is the 59th, one more ASCII is the 60th.
    CHECK(chatlist::titleFor(std::string(58, 'a') + ja + "bcd") == std::string(58, 'a') + ja + "b");

    CHECK(chatlist::titleFor(repeat(e, 70)) == repeat(e, 60));
    CHECK(chatlist::titleFor(repeat(ja, 61)) == repeat(ja, 60));
    CHECK(chatlist::titleFor(repeat(smile, 65)) == repeat(smile, 60));

    // Every byte boundary near the cut: still valid UTF-8, at most 60 characters.
    for (int ascii = 50; ascii <= 62; ++ascii) {
        const std::string title = chatlist::titleFor(std::string(ascii, 'q') + repeat(smile + e + ja, 10));
        INFO("ascii prefix " << ascii);
        CHECK(codePoints(title) == 60);
    }
}

TEST_CASE("chat_list: titleFor puts a multi-line message on one line")
{
    const std::string title = chatlist::titleFor("line one\nline two");
    CHECK(title == "line one line two");
    CHECK(chatlist::titleFor("a\tb") == "a b");
    CHECK(chatlist::titleFor("   padded question  \n") == "padded question");
}

TEST_CASE("chat_list: openMessage resumes a chat with a session id")
{
    ChatInfo chat;
    chat.id = "6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10";
    chat.sessionId = "b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b";
    const protocol::Inbound msg = chatlist::openMessage(chat);
    const auto* resume = std::get_if<protocol::ResumeChat>(&msg);
    REQUIRE(resume != nullptr);
    CHECK(resume->chatId == chat.id);
    CHECK(resume->sessionId == "b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b");
}

TEST_CASE("chat_list: openMessage starts a chat without a session id as new_chat under its own id")
{
    ChatInfo chat;
    chat.id = "0a0b0c0d-1111-4222-8333-444455556666";
    SUBCASE("no session id")
    {
        chat.sessionId = std::nullopt;
    }
    SUBCASE("empty session id")
    {
        chat.sessionId = std::string();
    }
    const protocol::Inbound msg = chatlist::openMessage(chat);
    CHECK(std::holds_alternative<protocol::NewChat>(msg));
    const auto* fresh = std::get_if<protocol::NewChat>(&msg);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->chatId == chat.id);
}

TEST_CASE("chat_list: continuePrompt with an empty transcript is the text alone")
{
    CHECK(chatlist::continuePrompt({}, "What next?") == "What next?");
}

TEST_CASE("chat_list: continuePrompt carries the last 20 messages, oldest first, then the text")
{
    CHECK(chatlist::kContextMessages == 20);
    std::vector<ChatMessage> transcript;
    for (int i = 0; i < 25; ++i) {
        char marker[16];
        std::snprintf(marker, sizeof marker, "<m%02d>", i);
        transcript.push_back(ChatMessage{i % 2 == 0 ? "user" : "assistant",
                                         std::string("message ") + marker + " body",
                                         "2026-10-12T10:00:00Z", std::nullopt});
    }
    const std::string text = "Please continue from here <final>";
    const std::string prompt = chatlist::continuePrompt(transcript, text);
    INFO("prompt: " << prompt);

    for (int i = 0; i < 5; ++i) {
        char marker[16];
        std::snprintf(marker, sizeof marker, "<m%02d>", i);
        CHECK(prompt.find(marker) == std::string::npos);
    }
    std::size_t last = 0;
    for (int i = 5; i < 25; ++i) {
        const std::string& body = transcript[static_cast<std::size_t>(i)].text;
        const std::size_t at = prompt.find(body);
        INFO("message " << i);
        REQUIRE(at != std::string::npos);
        CHECK(at >= last);
        last = at;
    }
    const std::size_t textAt = prompt.rfind(text);
    REQUIRE(textAt != std::string::npos);
    CHECK(textAt > last);
    CHECK(prompt.substr(prompt.size() - text.size()) == text);
}

TEST_CASE("chat_list: continuePrompt keeps every message of a short transcript, text unchanged")
{
    const std::vector<ChatMessage> transcript{
        {"user", "Remember the word PLUM.", "2026-10-12T10:00:00Z", std::nullopt},
        {"assistant", "Noted: PLUM.\nAnything else?", "2026-10-12T10:00:05Z", 0.001},
        {"user", "What word did I ask you to remember?", "2026-10-12T10:01:00Z", std::nullopt},
    };
    const std::string prompt = chatlist::continuePrompt(transcript, "Go on.");
    INFO("prompt: " << prompt);
    const std::size_t a = prompt.find("Remember the word PLUM.");
    const std::size_t b = prompt.find("Noted: PLUM.\nAnything else?");
    const std::size_t c = prompt.find("What word did I ask you to remember?");
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    REQUIRE(c != std::string::npos);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(prompt.substr(prompt.size() - 6) == "Go on.");
}

// ---- ChatList with a saved project ---------------------------------------

TEST_CASE("chat_list: create adds an untitled chat to project.json at once")
{
    TempDir tmp;
    FakeClock time;
    const auto project = openOk(tmp.path());
    ChatList list(project.get(), time.clock());
    CHECK(list.chats().empty());

    time.now = "2026-10-12T10:00:01Z";
    std::string error = "stale";
    const std::string id = list.create(error);
    CHECK(error.empty());
    CHECK(std::regex_match(id, std::regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$")));

    const ChatInfo* chat = list.find(id);
    REQUIRE(chat != nullptr);
    CHECK(chat->title.empty());
    CHECK_FALSE(chat->sessionId.has_value());
    CHECK(chat->created == "2026-10-12T10:00:01Z");
    CHECK(chat->updated == "2026-10-12T10:00:01Z");
    CHECK(list.transcript(id).empty());
    CHECK(list.find("no-such-chat") == nullptr);

    const json entry = diskChat(tmp.path(), id);
    REQUIRE(entry.is_object());
    CHECK(entry["title"] == "");
    CHECK(entry["session_id"].is_null());
    CHECK(entry["created"] == "2026-10-12T10:00:01Z");
    CHECK(entry["updated"] == "2026-10-12T10:00:01Z");
    // No transcript file until there is a message.
    CHECK_FALSE(fs::exists(jsonlPath(tmp.path(), id)));
}

TEST_CASE("chat_list: chats are ordered by updated, newest first")
{
    TempDir tmp;
    FakeClock time;
    const auto project = openOk(tmp.path());
    ChatList list(project.get(), time.clock());
    std::string error;

    time.now = "2026-10-12T10:00:01Z";
    const std::string a = list.create(error);
    time.now = "2026-10-12T10:00:02Z";
    const std::string b = list.create(error);
    time.now = "2026-10-12T10:00:03Z";
    const std::string c = list.create(error);
    CHECK(ids(list.chats()) == std::vector<std::string>{c, b, a});

    time.now = "2026-10-12T10:05:00Z";
    REQUIRE(list.addUserMessage(a, "First question for A", error));
    CHECK(ids(list.chats()) == std::vector<std::string>{a, c, b});

    time.now = "2026-10-12T10:06:00Z";
    REQUIRE(list.addResult(b, "Reply in B", resultFor(b, "sess-b"), error));
    CHECK(ids(list.chats()) == std::vector<std::string>{b, a, c});

    // Across days and months, as ISO 8601 text.
    time.now = "2026-11-01T00:00:00Z";
    REQUIRE(list.addUserMessage(c, "Later question for C", error));
    CHECK(ids(list.chats()) == std::vector<std::string>{c, b, a});
}

TEST_CASE("chat_list: chats from project.json come back sorted by updated")
{
    TempDir tmp;
    writeFile(projectJson(tmp.path()), R"({
  "version": 1,
  "open_file": null,
  "files": {},
  "chats": [
    {"id": "mid", "title": "Middle", "session_id": "s-mid", "created": "2026-10-01T09:00:00Z", "updated": "2026-10-05T09:00:00Z"},
    {"id": "old", "title": "Oldest", "session_id": null, "created": "2026-09-01T09:00:00Z", "updated": "2026-09-02T09:00:00Z"},
    {"id": "new", "title": "Newest", "session_id": "s-new", "created": "2026-10-02T09:00:00Z", "updated": "2026-10-11T23:59:59Z"},
    {"id": "dec", "title": "December", "session_id": "s-dec", "created": "2025-12-01T09:00:00Z", "updated": "2025-12-31T09:00:00Z"}
  ]
})");
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    CHECK(ids(list.chats()) == std::vector<std::string>{"new", "mid", "old", "dec"});
    const ChatInfo* mid = list.find("mid");
    REQUIRE(mid != nullptr);
    CHECK(mid->title == "Middle");
    CHECK(mid->sessionId == std::optional<std::string>("s-mid"));
}

TEST_CASE("chat_list: the first user message names the chat and is appended with updated")
{
    TempDir tmp;
    FakeClock time;
    const auto project = openOk(tmp.path());
    ChatList list(project.get(), time.clock());
    std::string error;
    time.now = "2026-10-12T10:00:00Z";
    const std::string id = list.create(error);

    const std::string first =
        "Explain the attention formula on page 4 and why it divides by the square root of d_k";
    time.now = "2026-10-12T10:00:10Z";
    REQUIRE(list.addUserMessage(id, first, error));
    CHECK(list.find(id)->title == first.substr(0, 60));
    CHECK(list.find(id)->updated == "2026-10-12T10:00:10Z");

    time.now = "2026-10-12T10:00:20Z";
    REQUIRE(list.addUserMessage(id, "A second question does not rename the chat", error));
    CHECK(list.find(id)->title == first.substr(0, 60));
    CHECK(list.find(id)->updated == "2026-10-12T10:00:20Z");

    const json entry = diskChat(tmp.path(), id);
    REQUIRE(entry.is_object());
    CHECK(entry["title"] == first.substr(0, 60));
    CHECK(entry["updated"] == "2026-10-12T10:00:20Z");
    CHECK(entry["created"] == "2026-10-12T10:00:00Z");

    const auto lines = readLines(jsonlPath(tmp.path(), id));
    REQUIRE(lines.size() == 2);
    const json one = json::parse(lines[0]);
    CHECK(one["role"] == "user");
    CHECK(one["text"] == first);
    CHECK(one["ts"] == "2026-10-12T10:00:10Z");
    CHECK(json::parse(lines[1])["text"] == "A second question does not rename the chat");
}

TEST_CASE("chat_list: a UTF-8 first message gives a UTF-8 safe title on disk")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    const std::string text = std::string(59, 'a') + "\xc3\xa9\xc3\xa9\xc3\xa9 tail";
    REQUIRE(list.addUserMessage(id, text, error));
    const std::string expected = std::string(59, 'a') + "\xc3\xa9";
    CHECK(list.find(id)->title == expected);
    // nlohmann throws on invalid UTF-8 when dumping, so a cut sequence would
    // also break save().
    const json entry = diskChat(tmp.path(), id);
    REQUIRE(entry.is_object());
    CHECK(entry["title"] == expected);
}

TEST_CASE("chat_list: a chat renamed before its first message keeps the name")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    REQUIRE(list.rename(id, "Attention notes", error));
    CHECK(list.find(id)->title == "Attention notes");
    CHECK(diskChat(tmp.path(), id)["title"] == "Attention notes");
    REQUIRE(list.addUserMessage(id, "Original first question", error));
    CHECK(list.find(id)->title == "Attention notes");
    CHECK(diskChat(tmp.path(), id)["title"] == "Attention notes");

    REQUIRE(list.rename(id, "Renamed again", error));
    CHECK(diskChat(tmp.path(), id)["title"] == "Renamed again");

    error.clear();
    CHECK_FALSE(list.rename("no-such-chat", "x", error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("chat_list: a result appends the reply with its cost and stores the first session id")
{
    TempDir tmp;
    FakeClock time;
    const auto project = openOk(tmp.path());
    ChatList list(project.get(), time.clock());
    std::string error;
    time.now = "2026-10-12T10:00:00Z";
    const std::string id = list.create(error);

    time.now = "2026-10-12T10:00:01Z";
    REQUIRE(list.addUserMessage(id, "Question one", error));
    time.now = "2026-10-12T10:00:09Z";
    REQUIRE(list.addResult(id, "## Answer\n\nOne.", resultFor(id, "sess-first", 0.0123), error));

    CHECK(list.find(id)->sessionId == std::optional<std::string>("sess-first"));
    CHECK(list.find(id)->updated == "2026-10-12T10:00:09Z");
    CHECK(diskChat(tmp.path(), id)["session_id"] == "sess-first");
    CHECK(diskChat(tmp.path(), id)["updated"] == "2026-10-12T10:00:09Z");

    // A later result with another session id does not replace the first.
    time.now = "2026-10-12T10:01:00Z";
    REQUIRE(list.addUserMessage(id, "Question two", error));
    time.now = "2026-10-12T10:01:07Z";
    REQUIRE(list.addResult(id, "Two.", resultFor(id, "sess-later", 0.002), error));
    CHECK(list.find(id)->sessionId == std::optional<std::string>("sess-first"));
    CHECK(diskChat(tmp.path(), id)["session_id"] == "sess-first");

    const auto lines = readLines(jsonlPath(tmp.path(), id));
    REQUIRE(lines.size() == 4);
    const json reply = json::parse(lines[1]);
    CHECK(reply["role"] == "assistant");
    CHECK(reply["text"] == "## Answer\n\nOne.");
    CHECK(reply["ts"] == "2026-10-12T10:00:09Z");
    REQUIRE(reply.contains("cost_usd"));
    CHECK(reply["cost_usd"].get<double>() == doctest::Approx(0.0123));

    const auto transcript = list.transcript(id);
    REQUIRE(transcript.size() == 4);
    CHECK(transcript[0].role == "user");
    CHECK(transcript[0].text == "Question one");
    CHECK_FALSE(transcript[0].costUsd.has_value());
    CHECK(transcript[1].role == "assistant");
    CHECK(transcript[2].text == "Question two");
    CHECK(transcript[3].text == "Two.");
    REQUIRE(transcript[3].costUsd.has_value());
    CHECK(*transcript[3].costUsd == doctest::Approx(0.002));
}

TEST_CASE("chat_list: one line per finished message, however the reply streamed")
{
    // ChatPanel collects text_delta pieces and hands over the whole reply at
    // `result`. Ten turns are twenty lines.
    TempDir tmp;
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    for (int turn = 0; turn < 10; ++turn) {
        const std::string n = std::to_string(turn);
        REQUIRE(list.addUserMessage(id, "question " + n, error));
        REQUIRE(list.addResult(id, "piece one of " + n + ", piece two, piece three",
                               resultFor(id, "sess-1"), error));
    }
    const auto lines = readLines(jsonlPath(tmp.path(), id));
    CHECK(lines.size() == 20);
    const auto transcript = list.transcript(id);
    REQUIRE(transcript.size() == 20);
    for (std::size_t i = 0; i < transcript.size(); ++i) {
        CHECK(transcript[i].role == (i % 2 == 0 ? "user" : "assistant"));
    }
    CHECK(transcript[19].text == "piece one of 9, piece two, piece three");
}

TEST_CASE("chat_list: a chat whose first reply failed has no session id and opens with new_chat")
{
    TempDir tmp;
    std::string error;
    std::string id;
    {
        const auto project = openOk(tmp.path());
        ChatList list(project.get());
        id = list.create(error);
        // The reply ended with an `error`, so only the user message is kept.
        REQUIRE(list.addUserMessage(id, "This first reply fails", error));
        CHECK_FALSE(list.find(id)->sessionId.has_value());
    }
    CHECK(diskChat(tmp.path(), id)["session_id"].is_null());

    // After a restart: the transcript is there, and the chat opens under its
    // own id with new_chat (never resume_chat without a session id).
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    const ChatInfo* chat = list.find(id);
    REQUIRE(chat != nullptr);
    CHECK(chat->title == "This first reply fails");
    const auto transcript = list.transcript(id);
    REQUIRE(transcript.size() == 1);
    CHECK(transcript[0].text == "This first reply fails");
    const protocol::Inbound open = chatlist::openMessage(*chat);
    const auto* fresh = std::get_if<protocol::NewChat>(&open);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->chatId == id);

    // Its first successful result stores the session id; from then on it resumes.
    REQUIRE(list.addUserMessage(id, "Try again", error));
    REQUIRE(list.addResult(id, "Worked this time.", resultFor(id, "sess-late"), error));
    CHECK(list.find(id)->sessionId == std::optional<std::string>("sess-late"));
    CHECK(diskChat(tmp.path(), id)["session_id"] == "sess-late");
    const protocol::Inbound reopen = chatlist::openMessage(*list.find(id));
    const auto* resume = std::get_if<protocol::ResumeChat>(&reopen);
    REQUIRE(resume != nullptr);
    CHECK(resume->chatId == id);
    CHECK(resume->sessionId == "sess-late");
    CHECK(list.transcript(id).size() == 3);
}

TEST_CASE("chat_list: chats and transcripts survive a restart")
{
    TempDir tmp;
    FakeClock time;
    std::string error;
    std::string a;
    std::string b;
    {
        const auto project = openOk(tmp.path());
        ChatList list(project.get(), time.clock());
        time.now = "2026-10-12T10:00:00Z";
        a = list.create(error);
        REQUIRE(list.addUserMessage(a, "Alpha question", error));
        time.now = "2026-10-12T10:00:05Z";
        REQUIRE(list.addResult(a, "Alpha answer", resultFor(a, "sess-a"), error));
        time.now = "2026-10-12T10:01:00Z";
        b = list.create(error);
        REQUIRE(list.addUserMessage(b, "Bravo question", error));
    }

    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    CHECK(ids(list.chats()) == std::vector<std::string>{b, a});
    const ChatInfo* chatA = list.find(a);
    REQUIRE(chatA != nullptr);
    CHECK(chatA->title == "Alpha question");
    CHECK(chatA->sessionId == std::optional<std::string>("sess-a"));
    CHECK(chatA->created == "2026-10-12T10:00:00Z");
    CHECK(chatA->updated == "2026-10-12T10:00:05Z");
    const auto transcript = list.transcript(a);
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].role == "user");
    CHECK(transcript[0].text == "Alpha question");
    CHECK(transcript[1].role == "assistant");
    CHECK(transcript[1].text == "Alpha answer");
    CHECK(transcript[1].ts == "2026-10-12T10:00:05Z");

    const protocol::Inbound open = chatlist::openMessage(*chatA);
    const auto* resume = std::get_if<protocol::ResumeChat>(&open);
    REQUIRE(resume != nullptr);
    CHECK(resume->chatId == a);
    CHECK(resume->sessionId == "sess-a");
}

TEST_CASE("chat_list: delete removes the entry and its JSONL, and nothing else")
{
    TempDir tmp;
    TempDir home;
    ScopedEnv homeEnv("HOME", home.path().string());

    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string keep = list.create(error);
    REQUIRE(list.addUserMessage(keep, "Keep me", error));
    REQUIRE(list.addResult(keep, "Kept.", resultFor(keep, "sess-keep"), error));
    const std::string gone = list.create(error);
    REQUIRE(list.addUserMessage(gone, "Delete me", error));
    REQUIRE(list.addResult(gone, "Deleted.", resultFor(gone, "sess-gone"), error));
    REQUIRE(fs::exists(jsonlPath(tmp.path(), gone)));

    // The agent CLI's own session files for the deleted chat's session.
    const fs::path claudeSession = home.path() / ".claude" / "projects" / "proj" / "sess-gone.jsonl";
    writeFile(claudeSession, "{\"session\":\"sess-gone\"}\n");
    const fs::path inProject = tmp.path() / "sess-gone.jsonl";
    writeFile(inProject, "not ours\n");
    const std::string keepBefore = readFile(jsonlPath(tmp.path(), keep));

    REQUIRE(list.remove(gone, error));
    CHECK(list.find(gone) == nullptr);
    CHECK(ids(list.chats()) == std::vector<std::string>{keep});
    CHECK(list.transcript(gone).empty());

    CHECK_FALSE(fs::exists(jsonlPath(tmp.path(), gone)));
    CHECK(diskChat(tmp.path(), gone).is_null());
    CHECK(diskChatCount(tmp.path()) == 1);
    REQUIRE(diskChat(tmp.path(), keep).is_object());
    CHECK(readFile(jsonlPath(tmp.path(), keep)) == keepBefore);
    CHECK(fs::exists(claudeSession));
    CHECK(readFile(claudeSession) == "{\"session\":\"sess-gone\"}\n");
    CHECK(fs::exists(inProject));

    // Gone after a restart too.
    const auto reopened = openOk(tmp.path());
    ChatList again(reopened.get());
    CHECK(again.find(gone) == nullptr);
    CHECK(again.find(keep) != nullptr);
}

TEST_CASE("chat_list: delete works for a chat that has no transcript yet")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    REQUIRE(list.remove(id, error));
    CHECK(list.chats().empty());
    CHECK(diskChatCount(tmp.path()) == 0);
}

TEST_CASE("chat_list: unknown ids are refused without changes")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    const std::string before = readFile(projectJson(tmp.path()));

    error.clear();
    CHECK_FALSE(list.addUserMessage("no-such-chat", "hello", error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(list.addResult("no-such-chat", "reply", resultFor("no-such-chat", "s"), error));
    CHECK_FALSE(error.empty());
    error.clear();
    CHECK_FALSE(list.remove("no-such-chat", error));
    CHECK_FALSE(error.empty());

    CHECK(ids(list.chats()) == std::vector<std::string>{id});
    CHECK(readFile(projectJson(tmp.path())) == before);
    CHECK_FALSE(fs::exists(jsonlPath(tmp.path(), "no-such-chat")));
    CHECK(list.transcript("no-such-chat").empty());
}

TEST_CASE("chat_list: a read-only project keeps chats in memory and reports the failed writes")
{
    TempDir tmp;
    const std::string original = R"({"version": 99, "open_file": null, "files": {}, "chats": []})";
    writeFile(projectJson(tmp.path()), original);
    const auto project = openOk(tmp.path());
    REQUIRE(project->readOnly());

    ChatList list(project.get());
    std::string error;
    const std::string id = list.create(error);
    CHECK_FALSE(error.empty());
    REQUIRE(list.find(id) != nullptr);

    error.clear();
    CHECK_FALSE(list.addUserMessage(id, "Still works in memory", error));
    CHECK_FALSE(error.empty());
    CHECK(list.find(id)->title == "Still works in memory");

    CHECK(readFile(projectJson(tmp.path())) == original);
    CHECK_FALSE(fs::exists(tmp.path() / ".reader" / "chats"));
}

// ---- ChatList without a project (unsaved project) ------------------------

TEST_CASE("chat_list: an unsaved project keeps chats in memory and writes nothing")
{
    TempDir cwd;
    TempDir home;
    ScopedCwd inCwd(cwd.path());
    ScopedEnv homeEnv("HOME", home.path().string());
    FakeClock time;

    ChatList list(nullptr, time.clock());
    CHECK(list.chats().empty());
    std::string error;

    time.now = "2026-10-12T10:00:00Z";
    const std::string a = list.create(error);
    CHECK(error.empty());
    REQUIRE(list.addUserMessage(a, "Unsaved first question", error));
    time.now = "2026-10-12T10:00:04Z";
    REQUIRE(list.addResult(a, "Unsaved answer", resultFor(a, "sess-unsaved"), error));

    time.now = "2026-10-12T10:01:00Z";
    const std::string b = list.create(error);
    REQUIRE(list.addUserMessage(b, "Second unsaved chat", error));

    CHECK(ids(list.chats()) == std::vector<std::string>{b, a});
    CHECK(list.find(a)->title == "Unsaved first question");
    CHECK(list.find(a)->sessionId == std::optional<std::string>("sess-unsaved"));

    const auto transcript = list.transcript(a);
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].role == "user");
    CHECK(transcript[0].text == "Unsaved first question");
    CHECK(transcript[1].role == "assistant");
    CHECK(transcript[1].text == "Unsaved answer");
    REQUIRE(transcript[1].costUsd.has_value());
    CHECK(list.transcript(b).size() == 1);

    REQUIRE(list.rename(b, "Renamed in memory", error));
    CHECK(list.find(b)->title == "Renamed in memory");
    REQUIRE(list.remove(a, error));
    CHECK(ids(list.chats()) == std::vector<std::string>{b});
    CHECK(list.transcript(a).empty());

    CHECK(dirIsEmpty(cwd.path()));
    CHECK(dirIsEmpty(home.path()));
}

// ---- Carried M6.1 notes on Project::appendMessage -------------------------

TEST_CASE("chat_list: a stale .tmp left by a crash does not leak into a new transcript")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    const fs::path file = jsonlPath(tmp.path(), "c1");
    fs::path stale = file;
    stale += ".tmp";
    writeFile(stale, "{\"role\":\"user\",\"text\":\"STALE line from a crash\",\"ts\":\"2026-01-01T00:00:00Z\"}\n");
    REQUIRE_FALSE(fs::exists(file));

    std::string error;
    REQUIRE(project->appendMessage("c1", ChatMessage{"user", "Fresh question", "2026-10-12T10:00:00Z", std::nullopt}, error));
    const auto transcript = project->transcript("c1");
    REQUIRE(transcript.size() == 1);
    CHECK(transcript[0].text == "Fresh question");
    CHECK(readFile(file).find("STALE") == std::string::npos);

    REQUIRE(project->appendMessage("c1", ChatMessage{"assistant", "Fresh answer", "2026-10-12T10:00:05Z", 0.001}, error));
    const auto two = project->transcript("c1");
    REQUIRE(two.size() == 2);
    CHECK(two[0].text == "Fresh question");
    CHECK(two[1].text == "Fresh answer");
}

TEST_CASE("chat_list: a stale .tmp next to an existing transcript is not committed either")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    std::string error;
    REQUIRE(project->appendMessage("c2", ChatMessage{"user", "Kept question", "2026-10-12T10:00:00Z", std::nullopt}, error));
    const fs::path file = jsonlPath(tmp.path(), "c2");
    fs::path stale = file;
    stale += ".tmp";
    writeFile(stale, "{\"role\":\"user\",\"text\":\"STALE line from a crash\",\"ts\":\"2026-01-01T00:00:00Z\"}\n");

    REQUIRE(project->appendMessage("c2", ChatMessage{"assistant", "Kept answer", "2026-10-12T10:00:05Z", 0.001}, error));
    const auto transcript = project->transcript("c2");
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].text == "Kept question");
    CHECK(transcript[1].text == "Kept answer");
    CHECK(readFile(file).find("STALE") == std::string::npos);
}

TEST_CASE("chat_list: a torn last line does not swallow the next message")
{
    // What a crash in the middle of a plain O_APPEND write leaves behind.
    TempDir tmp;
    const auto project = openOk(tmp.path());
    const fs::path file = jsonlPath(tmp.path(), "c3");
    writeFile(file,
              "{\"role\":\"user\",\"text\":\"Whole line\",\"ts\":\"2026-10-12T10:00:00Z\"}\n"
              "{\"role\":\"assistant\",\"text\":\"Cut sh");

    std::string error;
    REQUIRE(project->appendMessage("c3", ChatMessage{"user", "After the crash", "2026-10-12T10:01:00Z", std::nullopt}, error));
    const auto transcript = project->transcript("c3");
    REQUIRE(transcript.size() == 2);
    CHECK(transcript[0].text == "Whole line");
    CHECK(transcript[1].text == "After the crash");

    REQUIRE(project->appendMessage("c3", ChatMessage{"assistant", "And on", "2026-10-12T10:01:05Z", 0.001}, error));
    CHECK(project->transcript("c3").size() == 3);
}

TEST_CASE("chat_list: appends keep every earlier byte of a long transcript")
{
    TempDir tmp;
    const auto project = openOk(tmp.path());
    std::string error;
    const std::string body(2000, 'z');
    for (int i = 0; i < 300; ++i) {
        REQUIRE(project->appendMessage("long", ChatMessage{i % 2 == 0 ? "user" : "assistant",
                                                           std::to_string(i) + body,
                                                           "2026-10-12T10:00:00Z", std::nullopt},
                                       error));
    }
    const fs::path file = jsonlPath(tmp.path(), "long");
    const std::string before = readFile(file);
    REQUIRE(project->appendMessage("long", ChatMessage{"user", "one more", "2026-10-12T11:00:00Z", std::nullopt}, error));
    const std::string after = readFile(file);
    REQUIRE(after.size() > before.size());
    CHECK(after.compare(0, before.size(), before) == 0);
    const auto transcript = project->transcript("long");
    REQUIRE(transcript.size() == 301);
    CHECK(transcript[300].text == "one more");
    CHECK(transcript[299].text == std::to_string(299) + body);
}

TEST_CASE("chat_list: neighbor steps forward to the next id")
{
    const std::vector<std::string> ids{"a", "b", "c", "d"};
    CHECK(chatlist::neighbor(ids, "a", 1) == "b");
    CHECK(chatlist::neighbor(ids, "b", 1) == "c");
}

TEST_CASE("chat_list: neighbor steps back to the previous id")
{
    const std::vector<std::string> ids{"a", "b", "c", "d"};
    CHECK(chatlist::neighbor(ids, "d", -1) == "c");
    CHECK(chatlist::neighbor(ids, "c", -1) == "b");
}

TEST_CASE("chat_list: neighbor wraps past the last id forward and past the first back")
{
    const std::vector<std::string> ids{"a", "b", "c", "d"};
    CHECK(chatlist::neighbor(ids, "d", 1) == "a");
    CHECK(chatlist::neighbor(ids, "a", -1) == "d");
}

TEST_CASE("chat_list: neighbor takes steps larger than 1 and wraps them")
{
    const std::vector<std::string> ids{"a", "b", "c", "d", "e"};
    CHECK(chatlist::neighbor(ids, "a", 2) == "c");
    CHECK(chatlist::neighbor(ids, "b", 3) == "e");
    CHECK(chatlist::neighbor(ids, "d", 2) == "a");
    CHECK(chatlist::neighbor(ids, "a", -2) == "d");
    CHECK(chatlist::neighbor(ids, "a", 7) == "c");
    CHECK(chatlist::neighbor(ids, "a", -7) == "d");
}

TEST_CASE("chat_list: neighbor with a single id returns that id")
{
    const std::vector<std::string> ids{"only"};
    CHECK(chatlist::neighbor(ids, "only", 1) == "only");
    CHECK(chatlist::neighbor(ids, "only", -1) == "only");
}

TEST_CASE("chat_list: neighbor with an empty list returns current")
{
    const std::vector<std::string> ids;
    CHECK(chatlist::neighbor(ids, "x", 1) == "x");
    CHECK(chatlist::neighbor(ids, "x", -1) == "x");
}

TEST_CASE("chat_list: neighbor returns current when the id is not in the list")
{
    const std::vector<std::string> ids{"a", "b", "c"};
    CHECK(chatlist::neighbor(ids, "zzz", 1) == "zzz");
    CHECK(chatlist::neighbor(ids, "zzz", -1) == "zzz");
}

TEST_CASE("chat_list: neighbor follows the order given, and two ids are each other's neighbor")
{
    const std::vector<std::string> ids{"newest", "middle", "oldest"};
    CHECK(chatlist::neighbor(ids, "newest", 1) == "middle");
    CHECK(chatlist::neighbor(ids, "newest", -1) == "oldest");
    const std::vector<std::string> two{"x", "y"};
    CHECK(chatlist::neighbor(two, "x", 1) == "y");
    CHECK(chatlist::neighbor(two, "x", -1) == "y");
}
