#include "agent/SidecarClient.h"

#include <wx/datetime.h>
#include <wx/ffile.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/process.h>
#include <wx/stdpaths.h>
#include <wx/stream.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>

wxDEFINE_EVENT(EVT_SIDECAR_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_SIDECAR_MESSAGE, wxThreadEvent);
wxDEFINE_EVENT(EVT_SIDECAR_DOWN, wxThreadEvent);

namespace
{
using namespace std::chrono_literals;

constexpr auto kReadyTimeout = 10s;
constexpr auto kMaxBackoff = 30s;
// A run that stayed ready this long resets the backoff to 1 s.
constexpr auto kStableUptime = 30s;
constexpr auto kExitWait = 3s;
constexpr auto kTermWait = 1s;
constexpr int kWriteRetryMs = 10;

// Reader thread event IDs (wxEVT_THREAD posted to SidecarClient).
enum
{
    kLineEvent = 1,   // payload protocol::Outbound, GetInt() run ID
    kReaderDoneEvent, // a reader thread ended, GetInt() run ID
};

// Timer IDs.
enum
{
    kReadyTimer = 1,
    kRestartTimer,
    kWriteTimer,
};

int toMs(std::chrono::milliseconds d)
{
    return static_cast<int>(d.count());
}

// A log line written by the app, as opposed to the sidecar's own stderr.
std::string appLogLine(const wxString& text)
{
    return (wxDateTime::Now().Format("%Y-%m-%d %H:%M:%S") + " [app] " + text).utf8_string();
}

// Variables that hold a credential. Their values never reach a log.
constexpr const char* kCredentialVars[] = {
    "ANTHROPIC_API_KEY",
    "ANTHROPIC_AUTH_TOKEN",
    "CLAUDE_CODE_OAUTH_TOKEN",
};

// The sidecar's environment: the app's own environment, passed through
// `source` when set, with `changes` applied.
wxExecuteEnv sidecarEnvironment(const SidecarClient::EnvironmentSource& source,
                                const std::map<wxString, std::optional<wxString>>& changes)
{
    wxEnvVariableHashMap own;
    wxGetEnvMap(&own);
    std::map<wxString, wxString> env(own.begin(), own.end());
    if (source)
        env = source(env);
    for (const auto& [name, value] : changes) {
        if (value)
            env[name] = *value;
        else
            env.erase(name);
    }
    wxExecuteEnv result;
    for (const auto& [name, value] : env)
        result.env[name] = value;
    return result;
}

// Calls `onLine` for every complete line in `buffer` and keeps the rest.
template <typename F>
void takeLines(std::string& buffer, F&& onLine)
{
    std::size_t start = 0;
    for (std::size_t nl; (nl = buffer.find('\n', start)) != std::string::npos; start = nl + 1) {
        std::string line = buffer.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        onLine(line);
    }
    buffer.erase(0, start);
}

// Reads `in` until EOF and calls `onLine` per line, including a last line
// with no trailing newline.
template <typename F>
void readLines(wxInputStream& in, F&& onLine)
{
    std::string buffer;
    char chunk[4096];
    for (;;) {
        in.Read(chunk, sizeof chunk);
        const std::size_t n = in.LastRead();
        if (n == 0)
            break;
        buffer.append(chunk, n);
        takeLines(buffer, onLine);
    }
    if (!buffer.empty())
        onLine(buffer);
}

} // namespace

// The sidecar log file, shared by the stderr thread and the UI thread.
struct SidecarClient::LogFile
{
    std::mutex mutex;
    wxFFile file;

    void write(const std::string& line)
    {
        std::lock_guard lock(mutex);
        if (!file.IsOpened())
            return;
        file.Write(line.data(), line.size());
        file.Write("\n", 1);
        file.Flush();
    }
};

// Where reader threads post events. close() makes later posts a no-op, so a
// thread that outlives the client never touches it.
struct SidecarClient::Mailbox
{
    std::mutex mutex;
    wxEvtHandler* target = nullptr;

    void post(wxEvent* event)
    {
        std::lock_guard lock(mutex);
        if (target)
            wxQueueEvent(target, event);
        else
            delete event;
    }

    void close()
    {
        std::lock_guard lock(mutex);
        target = nullptr;
    }
};

// The child process. wx calls OnTerminate on the UI thread when it exits.
class SidecarClient::Process : public wxProcess
{
public:
    Process(SidecarClient* owner, int runId) : m_owner(owner), m_runId(runId)
    {
        Redirect();
    }

    // Detaches from the client. With deleteOnExit the object deletes itself
    // when the process exits. Otherwise it leaks, because a reader thread
    // still reads its streams.
    void orphan(bool deleteOnExit)
    {
        m_owner = nullptr;
        m_deleteOnExit = deleteOnExit;
    }

    // Set once wx has seen the process exit, from any later owner.
    std::shared_ptr<std::atomic<bool>> exitedFlag() const { return m_exited; }

    void OnTerminate(int /*pid*/, int status) override
    {
        *m_exited = true;
        if (m_owner)
            m_owner->onProcessExit(m_runId, status);
        else if (m_deleteOnExit)
            delete this;
    }

private:
    SidecarClient* m_owner;
    int m_runId;
    bool m_deleteOnExit = false;
    std::shared_ptr<std::atomic<bool>> m_exited = std::make_shared<std::atomic<bool>>(false);
};

// One spawned sidecar process and its two reader threads.
struct SidecarClient::Run
{
    struct Done
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool out = false;
        bool err = false;
    };

    int id = 0;
    long pid = 0;
    Process* process = nullptr;
    bool exited = false;
    std::thread outThread;
    std::thread errThread;
    std::shared_ptr<Done> done = std::make_shared<Done>();

    bool readersDone() const
    {
        std::lock_guard lock(done->mutex);
        return done->out && done->err;
    }

    // Waits up to `timeout` for stdout to reach EOF, which happens when the
    // process exits.
    bool waitStdoutEof(std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock(done->mutex);
        return done->cv.wait_for(lock, timeout, [this] { return done->out; });
    }

    ~Run()
    {
        bool outDone = false;
        bool errDone = false;
        {
            std::lock_guard lock(done->mutex);
            outDone = done->out;
            errDone = done->err;
        }
        // A thread still blocked in a read (a grandchild may hold stderr open)
        // is detached. It only touches the shared state and its stream.
        if (outThread.joinable())
            outDone ? outThread.join() : outThread.detach();
        if (errThread.joinable())
            errDone ? errThread.join() : errThread.detach();
        if (!process)
            return;
        if (!exited)
            process->orphan(outDone && errDone);
        else if (outDone && errDone)
            delete process;
        else
            process->orphan(false);
    }
};

SidecarClient::SidecarClient(wxEvtHandler* sink)
    : m_sink(sink),
      m_log(std::make_shared<LogFile>()),
      m_mailbox(std::make_shared<Mailbox>()),
      m_readyTimer(this, kReadyTimer),
      m_restartTimer(this, kRestartTimer),
      m_writeTimer(this, kWriteTimer)
{
#ifdef SIGPIPE
    // A write to a dead sidecar's stdin must fail with EPIPE, not kill the app.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    m_mailbox->target = this;
    Bind(wxEVT_THREAD, &SidecarClient::onReaderEvent, this);
    Bind(wxEVT_TIMER, &SidecarClient::onReadyTimeout, this, kReadyTimer);
    Bind(wxEVT_TIMER, &SidecarClient::onRestartTimer, this, kRestartTimer);
    Bind(wxEVT_TIMER, &SidecarClient::onWriteTimer, this, kWriteTimer);
}

SidecarClient::~SidecarClient()
{
    shutdown();
}

wxString SidecarClient::locateCommand()
{
    wxString cmd;
    if (wxGetEnv("READER_SIDECAR_CMD", &cmd)) {
        cmd.Trim(true).Trim(false);
        if (!cmd.empty())
            return cmd;
    }
#ifdef __WXOSX__
    wxFileName exe(wxStandardPaths::Get().GetResourcesDir(), "reader-sidecar");
#else
    wxFileName exe(wxStandardPaths::Get().GetExecutablePath());
    exe.SetName("reader-sidecar");
#ifdef __WINDOWS__
    exe.SetExt("exe");
#else
    exe.ClearExt();
#endif
#endif
    if (exe.FileExists())
        return "\"" + exe.GetFullPath() + "\"";
    return wxString();
}

void SidecarClient::setEnvironmentSource(EnvironmentSource source)
{
    m_envSource = std::move(source);
}

void SidecarClient::setEnvironment(std::map<wxString, std::optional<wxString>> changes)
{
    m_envChanges = std::move(changes);
}

void SidecarClient::setLogName(const wxString& name)
{
    m_logName = name;
}

wxString SidecarClient::logPath(const wxString& name)
{
    wxFileName path(wxStandardPaths::Get().GetUserDataDir(), name);
    path.AppendDir("logs");
    return path.GetFullPath();
}

bool SidecarClient::start()
{
    wxASSERT(wxIsMainThread());
    if (m_state != State::Stopped)
        return true;

    const wxFileName path(logPath(m_logName));
    path.Mkdir(wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    {
        std::lock_guard lock(m_log->mutex);
        if (!m_log->file.IsOpened()) {
            wxLogNull noLog;
            m_log->file.Open(path.GetFullPath(), "w");
        }
    }

    m_command = locateCommand();
    if (m_command.empty()) {
        logLine("no sidecar found: set READER_SIDECAR_CMD or install reader-sidecar next to the app");
        return false;
    }
    logLine("sidecar command: " + redacted(m_command));
    spawn();
    return true;
}

bool SidecarClient::restart()
{
    wxASSERT(wxIsMainThread());
    if (m_state == State::ShuttingDown)
        return false;
    if (m_state == State::Stopped)
        return start();

    logLine("restarting the sidecar with new settings");
    stopProcess();
    m_backoff = 1s;
    m_replayOpenProject = m_lastOpenProject.has_value();
    m_state = State::Stopped;
    m_command = locateCommand();
    if (m_command.empty()) {
        logLine("no sidecar found: set READER_SIDECAR_CMD or install reader-sidecar next to the app");
        return false;
    }
    spawn();
    return true;
}

void SidecarClient::spawn()
{
    auto run = std::make_unique<Run>();
    run->id = ++m_currentRunId;
    run->process = new Process(this, run->id);
    Run* r = run.get();
    m_run = std::move(run);
    m_state = State::Starting;

    // wx may report an immediate exit from inside wxExecute, which retires
    // `r` (still alive: reap() runs later).
    wxExecuteEnv env = sidecarEnvironment(m_envSource, m_envChanges);
    long pid = 0;
    {
        // wx errors may quote the command line, which can hold a key.
        wxLogNull noLog;
        pid = wxExecute(m_command, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE, r->process, &env);
    }
    if (pid == 0) {
        delete r->process;
        r->process = nullptr;
        {
            std::lock_guard lock(r->done->mutex);
            r->done->out = r->done->err = true;
        }
        r->exited = true;
        retireCurrentRun();
        goDown("could not start the sidecar");
        CallAfter(&SidecarClient::reap);
        return;
    }
    r->pid = pid;
    logLine(wxString::Format("started sidecar, pid %ld", pid));

    const int id = r->id;
    wxInputStream* out = r->process->GetInputStream();
    wxInputStream* err = r->process->GetErrorStream();
    auto mailbox = m_mailbox;
    auto log = m_log;
    auto done = r->done;

    auto finish = [mailbox, done, id](bool stdoutThread) {
        {
            std::lock_guard lock(done->mutex);
            (stdoutThread ? done->out : done->err) = true;
        }
        done->cv.notify_all();
        auto* event = new wxThreadEvent(wxEVT_THREAD, kReaderDoneEvent);
        event->SetInt(id);
        mailbox->post(event);
    };

    r->outThread = std::thread([out, mailbox, log, id, finish] {
        readLines(*out, [&](const std::string& line) {
            if (line.empty())
                return;
            const auto value = nlohmann::json::parse(line, nullptr, false);
            protocol::Outbound msg;
            std::string error;
            if (value.is_discarded()) {
                log->write(appLogLine(wxString::Format(
                    "dropped a sidecar stdout line that is not JSON (%zu bytes)", line.size())));
                return;
            }
            if (!protocol::fromJson(value, msg, error)) {
                log->write(
                    appLogLine("dropped an invalid sidecar message: " + wxString::FromUTF8(error)));
                return;
            }
            auto* event = new wxThreadEvent(wxEVT_THREAD, kLineEvent);
            event->SetInt(id);
            event->SetPayload(std::move(msg));
            mailbox->post(event);
        });
        finish(true);
    });
    r->errThread = std::thread([err, log, finish] {
        readLines(*err, [&](const std::string& line) { log->write(line); });
        finish(false);
    });

    if (m_run.get() == r)
        m_readyTimer.StartOnce(toMs(kReadyTimeout));
}

void SidecarClient::send(const protocol::Inbound& msg)
{
    wxASSERT(wxIsMainThread());
    if (const auto* open = std::get_if<protocol::OpenProject>(&msg))
        m_lastOpenProject = *open;

    const std::string line =
        protocol::toJson(msg).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    switch (m_state) {
    case State::Ready:
        writeLine(line);
        break;
    case State::Starting:
    case State::Restarting:
        m_queue.push_back(line);
        break;
    case State::Stopped:
    case State::ShuttingDown:
        logLine(wxString::Format("dropped %s: sidecar is not running", protocol::typeName(msg)));
        break;
    }
}

void SidecarClient::shutdown()
{
    shutdown(true);
}

void SidecarClient::shutdownInBackground()
{
    shutdown(false);
}

void SidecarClient::shutdown(bool wait)
{
    wxASSERT(wxIsMainThread());
    if (m_state == State::ShuttingDown)
        return;
    const State previous = m_state;
    m_state = State::ShuttingDown;
    if (previous != State::Stopped)
        logLine(wait ? "shutting down" : "shutting down in the background");
    stopProcess(wait);
    // Run destructors join finished threads and hand unfinished processes
    // to wx. The client no longer hears about them.
    m_retired.clear();
    m_mailbox->close();
}

void SidecarClient::stopProcess(bool wait)
{
    m_readyTimer.Stop();
    m_restartTimer.Stop();
    m_writeTimer.Stop();
    m_queue.clear();
    m_outBuf.clear();

    if (m_run && m_run->process && !m_run->exited) {
        logLine("closing sidecar stdin");
        m_run->process->CloseOutput();
        if (wait) {
            if (!m_run->waitStdoutEof(kExitWait)) {
                logLine("sidecar still running after 3 s, sending SIGTERM");
                wxProcess::Kill(static_cast<int>(m_run->pid), wxSIGTERM);
                if (!m_run->waitStdoutEof(kTermWait)) {
                    logLine("sidecar still running, sending SIGKILL");
                    wxProcess::Kill(static_cast<int>(m_run->pid), wxSIGKILL);
                    m_run->waitStdoutEof(kTermWait);
                }
            }
            logLine("sidecar stopped");
        } else {
            // The thread holds only shared state, never the client or the Run.
            const int pid = static_cast<int>(m_run->pid);
            std::thread([pid, done = m_run->done, exited = m_run->process->exitedFlag(),
                         log = m_log] {
                auto stdoutEof = [&](std::chrono::milliseconds timeout) {
                    std::unique_lock lock(done->mutex);
                    return done->cv.wait_for(lock, timeout, [&] { return done->out; });
                };
                // Once wx has seen the exit, the pid may belong to another process.
                if (stdoutEof(kExitWait) || *exited)
                    return;
                log->write(appLogLine("sidecar still running after 3 s, sending SIGTERM"));
                wxProcess::Kill(pid, wxSIGTERM);
                if (stdoutEof(kTermWait) || *exited)
                    return;
                log->write(appLogLine("sidecar still running, sending SIGKILL"));
                wxProcess::Kill(pid, wxSIGKILL);
            }).detach();
        }
    }
    retireCurrentRun();
}

void SidecarClient::onProcessExit(int runId, int status)
{
    Run* run = nullptr;
    if (m_run && m_run->id == runId)
        run = m_run.get();
    for (auto& retired : m_retired) {
        if (retired->id == runId)
            run = retired.get();
    }
    if (!run)
        return;
    run->exited = true;

    if (run == m_run.get()) {
        retireCurrentRun();
        if (m_state != State::ShuttingDown)
            goDown(wxString::Format("sidecar exited with status %d", status));
    }
    // Deleting the process now would happen inside its own OnTerminate.
    CallAfter(&SidecarClient::reap);
}

void SidecarClient::onReaderEvent(wxThreadEvent& event)
{
    switch (event.GetId()) {
    case kLineEvent:
        onMessage(event.GetInt(), event.GetPayload<protocol::Outbound>());
        break;
    case kReaderDoneEvent:
        reap();
        break;
    default:
        event.Skip();
        break;
    }
}

void SidecarClient::onMessage(int runId, const protocol::Outbound& msg)
{
    // Lines from a replaced process, or after shutdown, are stale.
    if (runId != m_currentRunId || m_state == State::ShuttingDown)
        return;

    if (const auto* ready = std::get_if<protocol::Ready>(&msg)) {
        onReady(*ready);
        return;
    }
    wxThreadEvent forward(EVT_SIDECAR_MESSAGE);
    forward.SetPayload(msg);
    m_sink->SafelyProcessEvent(forward);
}

void SidecarClient::onReady(const protocol::Ready& ready)
{
    if (m_state != State::Starting) {
        logLine("ignored a second ready");
        return;
    }
    m_readyTimer.Stop();
    m_state = State::Ready;
    m_readyAt = std::chrono::steady_clock::now();
    logLine("ready, sidecar version " + wxString::FromUTF8(ready.version));

    if (m_replayOpenProject && m_lastOpenProject) {
        logLine("re-sending open_project after restart");
        writeLine(protocol::toJson(protocol::Inbound{*m_lastOpenProject})
                      .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
                  + "\n");
    }
    m_replayOpenProject = false;
    for (const auto& line : m_queue)
        writeLine(line);
    m_queue.clear();

    wxThreadEvent forward(EVT_SIDECAR_READY);
    forward.SetString(wxString::FromUTF8(ready.version));
    m_sink->SafelyProcessEvent(forward);
}

void SidecarClient::onReadyTimeout(wxTimerEvent& /*event*/)
{
    if (m_state != State::Starting || !m_run)
        return;
    logLine("no ready within 10 s, killing the sidecar");
    // The exit then takes the normal restart path.
    wxProcess::Kill(static_cast<int>(m_run->pid), wxSIGKILL);
}

void SidecarClient::onRestartTimer(wxTimerEvent& /*event*/)
{
    if (m_state != State::Restarting)
        return;
    logLine("restarting the sidecar");
    spawn();
}

void SidecarClient::onWriteTimer(wxTimerEvent& /*event*/)
{
    flushOutput();
}

void SidecarClient::goDown(const wxString& reason)
{
    logLine(reason);
    const auto now = std::chrono::steady_clock::now();
    if (m_state == State::Ready && now - m_readyAt >= kStableUptime)
        m_backoff = 1s;

    m_state = State::Restarting;
    m_readyTimer.Stop();
    m_writeTimer.Stop();
    m_outBuf.clear();
    // Queued lines belong to the old process's chats.
    m_queue.clear();
    m_replayOpenProject = m_lastOpenProject.has_value();

    const auto delay = m_backoff;
    m_backoff = std::min<std::chrono::seconds>(m_backoff * 2, kMaxBackoff);
    logLine(wxString::Format("restarting in %d s", static_cast<int>(delay.count())));
    m_restartTimer.StartOnce(toMs(delay));

    wxThreadEvent forward(EVT_SIDECAR_DOWN);
    forward.SetString(reason);
    m_sink->SafelyProcessEvent(forward);
}

void SidecarClient::writeLine(const std::string& line)
{
    m_outBuf += line;
    flushOutput();
}

void SidecarClient::flushOutput()
{
    wxOutputStream* out = (m_run && m_run->process) ? m_run->process->GetOutputStream() : nullptr;
    if (!out) {
        m_outBuf.clear();
        return;
    }
    // The pipe is non-blocking: a full pipe takes part of the data or none.
    wxLogNull noLog;
    while (!m_outBuf.empty()) {
        out->Write(m_outBuf.data(), m_outBuf.size());
        const std::size_t n = out->LastWrite();
        if (n == 0) {
            if (out->GetLastError() == wxSTREAM_NO_ERROR) {
                m_writeTimer.StartOnce(kWriteRetryMs);
            } else {
                // The process exit that follows reports the failure.
                logLine("write to sidecar stdin failed");
                m_outBuf.clear();
            }
            return;
        }
        m_outBuf.erase(0, n);
    }
}

void SidecarClient::retireCurrentRun()
{
    if (m_run)
        m_retired.push_back(std::move(m_run));
}

void SidecarClient::reap()
{
    std::erase_if(m_retired, [](const std::unique_ptr<Run>& run) {
        return run->exited && run->readersDone();
    });
}

void SidecarClient::logLine(const wxString& text)
{
    m_log->write(appLogLine(text));
}

// `text` with credentials replaced by "[redacted]": the values of the
// credential variables in the app's environment and in the changes, any
// sk-ant-... token, and the value of any NAME=value word whose NAME holds
// KEY, TOKEN or SECRET.
wxString SidecarClient::redacted(const wxString& text) const
{
    constexpr std::size_t kMinSecret = 8; // shorter values would hit ordinary words
    std::vector<wxString> secrets;
    for (const char* name : kCredentialVars) {
        wxString value;
        if (wxGetEnv(name, &value))
            secrets.push_back(value);
        if (const auto it = m_envChanges.find(name); it != m_envChanges.end() && it->second)
            secrets.push_back(*it->second);
    }
    wxString out = text;
    for (const wxString& secret : secrets) {
        if (secret.length() >= kMinSecret)
            out.Replace(secret, "[redacted]");
    }

    static const std::regex assignment(
        R"(([A-Za-z_][A-Za-z0-9_]*(KEY|TOKEN|SECRET)[A-Za-z0-9_]*)=("[^"]*"|'[^']*'|[^\s"']+))",
        std::regex::icase);
    static const std::regex anthropicKey(R"(sk-ant-[A-Za-z0-9_\-]+)");
    std::string utf8 = out.utf8_string();
    utf8 = std::regex_replace(utf8, assignment, "$1=[redacted]");
    utf8 = std::regex_replace(utf8, anthropicKey, "[redacted]");
    return wxString::FromUTF8(utf8);
}
