#pragma once

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <functional>
#include <memory>
#include <string>

class SidecarClient;

// Checks an API key with a temporary sidecar. The sidecar runs with the key
// as ANTHROPIC_API_KEY, gets open_project (an empty temp folder), new_chat
// and prompt "ping", and the first `result` or `error` for that chat decides
// the outcome. The sidecar then stops. UI thread only.
class KeyTester : public wxEvtHandler
{
public:
    // ok is true when the prompt got a `result`. message is one line for the
    // user and never contains the key.
    using Done = std::function<void(bool ok, const wxString& message)>;

    KeyTester();
    ~KeyTester() override;

    KeyTester(const KeyTester&) = delete;
    KeyTester& operator=(const KeyTester&) = delete;

    // Starts a test. `done` runs later on the UI thread, once, unless
    // cancel() or the destructor runs first. Cancels a running test.
    void start(const wxString& key, Done done);

    bool running() const { return m_sidecar != nullptr; }

    // Stops a running test without calling `done`.
    void cancel();

private:
    void onMessage(wxThreadEvent& event);
    void onDown(wxThreadEvent& event);
    void onTimeout(wxTimerEvent& event);
    void finish(bool ok, wxString message);
    void stop();

    std::unique_ptr<SidecarClient> m_sidecar;
    Done m_done;
    wxString m_key;
    wxString m_projectDir;
    std::string m_chatId;
    wxTimer m_timeout;
    // Bumped by every start and stop, so a finish queued by an older test
    // does nothing.
    int m_generation = 0;
};
