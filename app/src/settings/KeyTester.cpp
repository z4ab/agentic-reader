#include "settings/KeyTester.h"

#include "agent/SidecarClient.h"

#include <wx/filename.h>
#include <wx/log.h>
#include <wx/utils.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>

namespace
{

constexpr int kTimeoutMs = 60 * 1000;

// Message seq values, so an error without a chat_id still maps to this test.
constexpr std::int64_t kOpenSeq = 1;
constexpr std::int64_t kNewChatSeq = 2;
constexpr std::int64_t kPromptSeq = 3;

// A random (version 4) UUID string.
std::string makeUuid()
{
    std::random_device rd;
    std::mt19937_64 gen((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    std::uniform_int_distribution<unsigned> byte(0, 255);
    unsigned char b[16];
    for (auto& x : b)
        x = static_cast<unsigned char>(byte(gen));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    char out[37];
    std::snprintf(out, sizeof out,
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
                  b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
    return out;
}

// A new empty folder for the test's open_project, or empty on failure.
wxString makeProjectDir()
{
    for (int i = 0; i < 100; ++i) {
        wxFileName dir = wxFileName::DirName(wxFileName::GetTempDir());
        dir.AppendDir(wxString::Format("agentic-reader-keytest-%lu-%d", wxGetProcessId(), i));
        if (dir.DirExists())
            continue;
        wxLogNull noLog;
        if (dir.Mkdir(0700))
            return dir.GetPath();
    }
    return wxString();
}

} // namespace

KeyTester::KeyTester() : m_timeout(this)
{
    Bind(EVT_SIDECAR_MESSAGE, &KeyTester::onMessage, this);
    Bind(EVT_SIDECAR_DOWN, &KeyTester::onDown, this);
    Bind(wxEVT_TIMER, &KeyTester::onTimeout, this);
}

KeyTester::~KeyTester()
{
    stop();
}

void KeyTester::start(const wxString& key, Done done)
{
    cancel();
    m_done = std::move(done);
    m_key = key;
    m_chatId = makeUuid();

    m_projectDir = makeProjectDir();
    if (m_projectDir.empty()) {
        finish(false, "Test failed: could not create a temporary folder.");
        return;
    }

    m_sidecar = std::make_unique<SidecarClient>(this);
    m_sidecar->setLogName("sidecar-keytest.log");
    // Only the entered key may authenticate the test.
    m_sidecar->setEnvironment({
        {"ANTHROPIC_API_KEY", key},
        {"ANTHROPIC_AUTH_TOKEN", std::nullopt},
        {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt},
    });
    if (!m_sidecar->start()) {
        finish(false, "Test failed: the assistant program (reader-sidecar) was not found.");
        return;
    }

    // Queued until the sidecar is ready.
    m_sidecar->send(protocol::OpenProject{m_projectDir.utf8_string(), kOpenSeq});
    m_sidecar->send(protocol::NewChat{m_chatId, kNewChatSeq});
    m_sidecar->send(protocol::Prompt{m_chatId, "ping", std::nullopt, kPromptSeq});
    m_timeout.StartOnce(kTimeoutMs);
}

void KeyTester::cancel()
{
    stop();
    m_done = nullptr;
}

void KeyTester::onMessage(wxThreadEvent& event)
{
    const auto msg = event.GetPayload<protocol::Outbound>();
    if (const auto* result = std::get_if<protocol::Result>(&msg)) {
        if (result->chatId == m_chatId)
            finish(true, "The key works.");
    } else if (const auto* error = std::get_if<protocol::Error>(&msg)) {
        const bool ours = error->chatId ? *error->chatId == m_chatId
                                        : (error->seq && *error->seq >= kOpenSeq
                                           && *error->seq <= kPromptSeq);
        if (ours)
            finish(false, wxString::Format("Test failed, error kind \"%s\": %s",
                                           wxString::FromUTF8(error->kind),
                                           wxString::FromUTF8(error->message)));
    } else if (const auto* request = std::get_if<protocol::PermissionRequest>(&msg)) {
        // "ping" needs no tools.
        if (m_sidecar)
            m_sidecar->send(protocol::PermissionReply{request->reqId, false, std::nullopt});
    }
}

void KeyTester::onDown(wxThreadEvent& event)
{
    finish(false, "Test failed: the assistant program stopped (" + event.GetString() + ").");
}

void KeyTester::onTimeout(wxTimerEvent& /*event*/)
{
    finish(false, "Test failed: no answer within 60 seconds.");
}

void KeyTester::finish(bool ok, wxString message)
{
    if (!m_done)
        return;
    if (!m_key.empty())
        message.Replace(m_key, "<key>");
    Done done = std::move(m_done);
    m_done = nullptr;
    m_timeout.Stop();
    // Called from inside a SidecarClient event: stop it after the handler
    // returns. Pending CallAfter calls die with this object.
    const int generation = m_generation;
    CallAfter([this, generation, done = std::move(done), ok, message] {
        if (generation != m_generation)
            return;
        stop();
        done(ok, message);
    });
}

void KeyTester::stop()
{
    ++m_generation;
    m_timeout.Stop();
    if (m_sidecar) {
        m_sidecar->shutdown();
        m_sidecar.reset();
    }
    if (!m_projectDir.empty()) {
        wxLogNull noLog;
        wxFileName::Rmdir(m_projectDir, wxPATH_RMDIR_RECURSIVE);
        m_projectDir.clear();
    }
    m_key.clear();
}
