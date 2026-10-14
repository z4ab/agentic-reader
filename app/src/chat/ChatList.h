#pragma once

#include "agent/Protocol.h"
#include "project/Project.h"

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace chatlist
{

constexpr std::size_t kTitleChars = 60;      // Unicode code points
constexpr std::size_t kContextMessages = 20; // for continuePrompt()

// A random RFC 4122 version 4 UUID, lower-case hex, 8-4-4-4-12.
std::string newChatId();

// The current UTC time as ISO 8601 with milliseconds, such as
// "2026-10-12T10:00:00.123Z". Used for created, updated and ts.
std::string utcNow();

// The title of a chat whose first user message is `text`: line breaks and
// tabs become spaces, leading and trailing whitespace is dropped, then the
// first kTitleChars code points are kept. Never cuts a UTF-8 sequence. No
// ellipsis is added.
std::string titleFor(const std::string& text);

// The first prompt of a chat continued from one that cannot resume: the last
// kContextMessages messages of `transcript`, oldest first, as context, then
// `text`, which ends the prompt. An empty transcript gives exactly `text`.
std::string continuePrompt(const std::vector<ChatMessage>& transcript, const std::string& text);

// The message that opens `chat` in the sidecar: resume_chat when the chat has
// a non-empty session id, else new_chat under its own id (its first reply
// failed, so no `result` brought a session id). seq is left empty.
protocol::Inbound openMessage(const ChatInfo& chat);

// The id `step` places from `current` in `ids`, in the order given, wrapping
// at both ends. A positive step goes forward, a negative one back, of any
// size. Returns `current` when `ids` has fewer than two entries or does not
// contain `current`.
std::string neighbor(const std::vector<std::string>& ids, const std::string& current, int step);

} // namespace chatlist

// The chats of the open project. With a Project, every change is written
// through it at once: project.json via setChats() + save(), transcripts via
// appendMessage() into .reader/chats/<id>.jsonl. With a null project (an
// unsaved project) chats and transcripts live in memory only and nothing is
// written anywhere.
//
// The mutators return false and set `error` for an unknown id (nothing
// changes) or a failed write. A failed write keeps the change in memory, so
// the chat goes on, as in a read-only project.
class ChatList
{
public:
    // Returns the timestamp for created, updated and each message's ts.
    using Clock = std::function<std::string()>;

    // `project` may be null and must otherwise outlive the list. Starts from
    // project->chats(). An empty `clock` means chatlist::utcNow.
    explicit ChatList(Project* project, Clock clock = {});

    // Every chat, newest `updated` first (ISO 8601 strings compare as text).
    std::vector<ChatInfo> chats() const;
    // Null for an unknown id. Valid until the next change.
    const ChatInfo* find(const std::string& id) const;

    // A new chat with a newChatId() id, an empty title, created and updated
    // set to now and no session id, added to project.json. Returns the id.
    // The chat is added even when the write fails; then `error` says why (it
    // is cleared on success).
    std::string create(std::string& error);

    // After every user message: appends {"user", text, now}, sets updated to
    // now, and sets the title to titleFor(text) while the title is empty.
    bool addUserMessage(const std::string& id, const std::string& text, std::string& error);

    // After every `result` (never per text_delta): appends {"assistant",
    // replyText, now, result.costUsd}, sets updated to now, and stores
    // result.sessionId while the chat has none.
    bool addResult(const std::string& id, const std::string& replyText,
                   const protocol::Result& result, std::string& error);

    bool rename(const std::string& id, const std::string& title, std::string& error);

    // Drops the entry from project.json and deletes .reader/chats/<id>.jsonl.
    // Touches nothing else (not the agent CLI's own session files).
    bool remove(const std::string& id, std::string& error);

    // The chat's messages in order: from the JSONL file with a project, from
    // memory without one (and for messages whose write failed). Empty for an
    // unknown id.
    std::vector<ChatMessage> transcript(const std::string& id) const;

    // An unsaved project was saved as `project`: writes the chats kept in
    // memory so far to it, and writes through it from now on.
    bool attach(Project* project, std::string& error);

private:
    ChatInfo* findChat(const std::string& id);
    bool append(const std::string& id, const ChatMessage& message, std::string& error);
    bool save(std::string& error);

    Project* m_project = nullptr;
    Clock m_clock;
    std::vector<ChatInfo> m_chats;
    // Messages not on disk: all of them without a project, otherwise those
    // whose write failed.
    std::map<std::string, std::vector<ChatMessage>> m_memory;
};
