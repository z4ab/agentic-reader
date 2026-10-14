#include "chat/ChatList.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <random>
#include <utility>

namespace chatlist
{

std::string newChatId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    hi = (hi & ~std::uint64_t{0xf000}) | 0x4000;                      // version 4
    lo = (lo & ~(std::uint64_t{3} << 62)) | (std::uint64_t{2} << 62); // RFC 4122 variant
    char buf[37];
    std::snprintf(buf, sizeof buf,
                  "%08" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%04" PRIx64 "-%012" PRIx64,
                  hi >> 32, (hi >> 16) & 0xffff, hi & 0xffff, lo >> 48,
                  lo & std::uint64_t{0xffffffffffff});
    return buf;
}

std::string utcNow()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t secs = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &secs);
#else
    gmtime_r(&secs, &utc);
#endif
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &utc);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s.%03dZ", date, static_cast<int>(millis));
    return buf;
}

std::string titleFor(const std::string& text)
{
    std::string line;
    line.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
            continue; // "\r\n" is one break
        line += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    const std::size_t first = line.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    line = line.substr(first, line.find_last_not_of(' ') - first + 1);

    // Keep kTitleChars code points: a byte that is not a continuation byte
    // starts the next one.
    std::size_t count = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if ((static_cast<unsigned char>(line[i]) & 0xC0) == 0x80)
            continue;
        if (count == kTitleChars) {
            line.resize(i);
            break;
        }
        ++count;
    }
    return line;
}

std::string continuePrompt(const std::vector<ChatMessage>& transcript, const std::string& text)
{
    if (transcript.empty())
        return text;
    const std::size_t start =
        transcript.size() > kContextMessages ? transcript.size() - kContextMessages : 0;
    std::string out = "This conversation continues an earlier one that cannot be resumed. "
                      "Its last messages follow as context.\n\n<earlier_conversation>\n";
    for (std::size_t i = start; i < transcript.size(); ++i) {
        const ChatMessage& message = transcript[i];
        out += message.role == "assistant" ? "[assistant]\n" : "[user]\n";
        out += message.text;
        out += "\n\n";
    }
    out += "</earlier_conversation>\n\n";
    out += text;
    return out;
}

protocol::Inbound openMessage(const ChatInfo& chat)
{
    if (chat.sessionId && !chat.sessionId->empty())
        return protocol::ResumeChat{chat.id, *chat.sessionId, std::nullopt};
    return protocol::NewChat{chat.id, std::nullopt};
}

std::string neighbor(const std::vector<std::string>& ids, const std::string& current, int step)
{
    const auto it = std::find(ids.begin(), ids.end(), current);
    if (ids.size() < 2 || it == ids.end())
        return current;
    const auto size = static_cast<long long>(ids.size());
    long long pos = (static_cast<long long>(it - ids.begin()) + step) % size;
    if (pos < 0)
        pos += size;
    return ids[static_cast<std::size_t>(pos)];
}

} // namespace chatlist

ChatList::ChatList(Project* project, Clock clock)
    : m_project(project), m_clock(clock ? std::move(clock) : Clock(&chatlist::utcNow))
{
    if (m_project)
        m_chats = m_project->chats();
}

std::vector<ChatInfo> ChatList::chats() const
{
    std::vector<ChatInfo> sorted = m_chats;
    std::stable_sort(sorted.begin(), sorted.end(), [](const ChatInfo& a, const ChatInfo& b) {
        return a.updated > b.updated;
    });
    return sorted;
}

const ChatInfo* ChatList::find(const std::string& id) const
{
    const auto it = std::find_if(m_chats.begin(), m_chats.end(),
                                 [&](const ChatInfo& chat) { return chat.id == id; });
    return it == m_chats.end() ? nullptr : &*it;
}

ChatInfo* ChatList::findChat(const std::string& id)
{
    return const_cast<ChatInfo*>(std::as_const(*this).find(id));
}

std::string ChatList::create(std::string& error)
{
    error.clear();
    ChatInfo chat;
    chat.id = chatlist::newChatId();
    chat.created = m_clock();
    chat.updated = chat.created;
    m_chats.push_back(chat);
    save(error);
    return chat.id;
}

bool ChatList::addUserMessage(const std::string& id, const std::string& text, std::string& error)
{
    ChatInfo* chat = findChat(id);
    if (!chat) {
        error = "unknown chat: " + id;
        return false;
    }
    const std::string now = m_clock();
    chat->updated = now;
    if (chat->title.empty())
        chat->title = chatlist::titleFor(text);
    std::string appendError;
    const bool appended = append(id, ChatMessage{"user", text, now, std::nullopt}, appendError);
    const bool saved = save(error);
    if (!appended)
        error = appendError;
    return appended && saved;
}

bool ChatList::addResult(const std::string& id, const std::string& replyText,
                         const protocol::Result& result, std::string& error)
{
    ChatInfo* chat = findChat(id);
    if (!chat) {
        error = "unknown chat: " + id;
        return false;
    }
    const std::string now = m_clock();
    chat->updated = now;
    if ((!chat->sessionId || chat->sessionId->empty()) && !result.sessionId.empty())
        chat->sessionId = result.sessionId;
    std::string appendError;
    const bool appended =
        append(id, ChatMessage{"assistant", replyText, now, result.costUsd}, appendError);
    const bool saved = save(error);
    if (!appended)
        error = appendError;
    return appended && saved;
}

bool ChatList::rename(const std::string& id, const std::string& title, std::string& error)
{
    ChatInfo* chat = findChat(id);
    if (!chat) {
        error = "unknown chat: " + id;
        return false;
    }
    chat->title = title;
    return save(error);
}

bool ChatList::remove(const std::string& id, std::string& error)
{
    const auto it = std::find_if(m_chats.begin(), m_chats.end(),
                                 [&](const ChatInfo& chat) { return chat.id == id; });
    if (it == m_chats.end()) {
        error = "unknown chat: " + id;
        return false;
    }
    m_chats.erase(it);
    m_memory.erase(id);
    std::string removeError;
    const bool removed = !m_project || m_project->removeTranscript(id, removeError);
    const bool saved = save(error);
    if (!removed)
        error = removeError;
    return removed && saved;
}

std::vector<ChatMessage> ChatList::transcript(const std::string& id) const
{
    if (!find(id))
        return {};
    std::vector<ChatMessage> messages;
    if (m_project)
        messages = m_project->transcript(id);
    if (const auto it = m_memory.find(id); it != m_memory.end())
        messages.insert(messages.end(), it->second.begin(), it->second.end());
    return messages;
}

bool ChatList::attach(Project* project, std::string& error)
{
    error.clear();
    m_project = project;
    if (!m_project)
        return true;

    // The project's own chats (none for a folder saved just now) stay.
    std::vector<ChatInfo> merged = m_project->chats();
    for (const ChatInfo& chat : m_chats) {
        const bool known = std::any_of(merged.begin(), merged.end(),
                                       [&](const ChatInfo& other) { return other.id == chat.id; });
        if (!known)
            merged.push_back(chat);
    }
    m_chats = std::move(merged);

    bool ok = true;
    auto memory = std::move(m_memory);
    m_memory.clear();
    for (auto& [id, messages] : memory) {
        for (const ChatMessage& message : messages) {
            std::string appendError;
            if (!append(id, message, appendError) && ok) {
                ok = false;
                error = appendError;
            }
        }
    }
    std::string saveError;
    if (!save(saveError) && ok) {
        ok = false;
        error = saveError;
    }
    return ok;
}

bool ChatList::append(const std::string& id, const ChatMessage& message, std::string& error)
{
    // Once a write failed, later messages stay in memory too, so the order
    // holds.
    if (m_project && !m_memory.contains(id) && m_project->appendMessage(id, message, error))
        return true;
    m_memory[id].push_back(message);
    if (m_project && error.empty())
        error = "an earlier message of this chat could not be saved";
    return !m_project;
}

bool ChatList::save(std::string& error)
{
    if (!m_project) {
        error.clear();
        return true;
    }
    m_project->setChats(m_chats);
    if (!m_project->save(error))
        return false;
    error.clear();
    return true;
}
