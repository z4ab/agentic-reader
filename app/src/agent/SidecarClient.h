#pragma once

#include "agent/Protocol.h"

#include <wx/event.h>
#include <wx/string.h>
#include <wx/timer.h>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Events SidecarClient sends to its sink, always on the UI thread.
//   EVT_SIDECAR_READY    The sidecar sent `ready`. GetString() is its version.
//   EVT_SIDECAR_MESSAGE  Any other sidecar message.
//                        GetPayload<protocol::Outbound>() holds it.
//   EVT_SIDECAR_DOWN     The sidecar exited unexpectedly, did not send `ready`
//                        within 10 s, or could not start. GetString() is the
//                        reason. A restart follows.
wxDECLARE_EVENT(EVT_SIDECAR_READY, wxThreadEvent);
wxDECLARE_EVENT(EVT_SIDECAR_MESSAGE, wxThreadEvent);
wxDECLARE_EVENT(EVT_SIDECAR_DOWN, wxThreadEvent);

// Runs the agent sidecar process and speaks the NDJSON protocol over its
// stdin and stdout (sidecar/PROTOCOL.md).
//
// A reader thread parses stdout lines and posts them to the UI thread. The
// sidecar's stderr goes to <user data dir>/logs/sidecar.log, together with
// lifecycle lines from this class prefixed "[app]".
//
// Sends wait in a queue until the sidecar is ready. After an unexpected exit
// the client restarts the sidecar with backoff (1 s, 2 s, 4 s, ... 30 s) and
// re-sends the last `open_project` before any queued message. Open chats do
// not survive a restart.
class SidecarClient : public wxEvtHandler
{
public:
    // `sink` receives the EVT_SIDECAR_* events. It must outlive this object.
    explicit SidecarClient(wxEvtHandler* sink);
    ~SidecarClient() override;

    SidecarClient(const SidecarClient&) = delete;
    SidecarClient& operator=(const SidecarClient&) = delete;

    // The sidecar command line, or an empty string when none is found:
    // READER_SIDECAR_CMD when set, else a `reader-sidecar` executable next to
    // the app binary (Contents/Resources on macOS).
    static wxString locateCommand();

    // Starts the sidecar. Returns false, and starts nothing, when
    // locateCommand() finds no sidecar.
    bool start();

    // Writes one message line to the sidecar's stdin. UI thread only.
    // Queued until `ready`. Dropped before start() and after shutdown().
    void send(const protocol::Inbound& msg);

    // Closes the sidecar's stdin, waits up to 3 s for it to exit, then kills
    // it. No restart follows. Blocks the calling (UI) thread meanwhile.
    void shutdown();

    // As shutdown(), but returns at once: a background thread waits for the
    // exit and kills the sidecar when it is still running 3 s later. No event
    // reaches the sink afterwards.
    void shutdownInBackground();

    // Stops the sidecar as shutdown() does, then starts a new one, which
    // builds its environment afresh. Queued messages are dropped. The last
    // `open_project` is re-sent once the new sidecar is ready. Sends no
    // EVT_SIDECAR_DOWN. Starts the sidecar when it was never started.
    // Returns false when no sidecar is found or after shutdown().
    bool restart();

    bool isReady() const { return m_state == State::Ready; }

    // Builds the sidecar's environment from the app's own at every spawn.
    // Without a source the sidecar gets the app's environment unchanged.
    using EnvironmentSource = std::function<std::map<wxString, wxString>(
        const std::map<wxString, wxString>& inherited)>;
    void setEnvironmentSource(EnvironmentSource source);

    // Changes applied after the environment source, at every spawn. A
    // std::nullopt value removes the variable. Call before start().
    void setEnvironment(std::map<wxString, std::optional<wxString>> changes);

    // The log file name under <user data dir>/logs. Default "sidecar.log".
    // Call before start().
    void setLogName(const wxString& name);

    // A sidecar log file: <user data dir>/logs/<name>.
    static wxString logPath(const wxString& name = "sidecar.log");

private:
    enum class State
    {
        Stopped,      // before start() or when no sidecar was found
        Starting,     // spawned, waiting for `ready`
        Ready,
        Restarting,   // exited, the restart timer is running
        ShuttingDown, // shutdown() called
    };

    class Process;
    struct LogFile;
    struct Mailbox;
    struct Run;

    void spawn();
    void shutdown(bool wait);
    // Stops the current process. Without `wait`, a detached thread finishes
    // the job.
    void stopProcess(bool wait = true);
    void onProcessExit(int runId, int status);
    void onReaderEvent(wxThreadEvent& event);
    void onMessage(int runId, const protocol::Outbound& msg);
    void onReady(const protocol::Ready& ready);
    void onReadyTimeout(wxTimerEvent& event);
    void onRestartTimer(wxTimerEvent& event);
    void onWriteTimer(wxTimerEvent& event);
    void goDown(const wxString& reason);
    void writeLine(const std::string& line);
    void flushOutput();
    void retireCurrentRun();
    void reap();
    void logLine(const wxString& text);
    wxString redacted(const wxString& text) const;

    wxEvtHandler* m_sink;
    wxString m_command;
    EnvironmentSource m_envSource;
    std::map<wxString, std::optional<wxString>> m_envChanges;
    wxString m_logName = "sidecar.log";
    State m_state = State::Stopped;

    std::shared_ptr<LogFile> m_log;
    std::shared_ptr<Mailbox> m_mailbox;
    // The newest process. Older ones wait in m_retired until their reader
    // threads finish.
    std::unique_ptr<Run> m_run;
    std::vector<std::unique_ptr<Run>> m_retired;
    int m_currentRunId = 0;

    // Lines sent before `ready`, and bytes the stdin pipe has not taken yet.
    std::vector<std::string> m_queue;
    std::string m_outBuf;

    std::optional<protocol::OpenProject> m_lastOpenProject;
    bool m_replayOpenProject = false;

    std::chrono::steady_clock::time_point m_readyAt;
    std::chrono::seconds m_backoff{1};

    wxTimer m_readyTimer;
    wxTimer m_restartTimer;
    wxTimer m_writeTimer;
};
