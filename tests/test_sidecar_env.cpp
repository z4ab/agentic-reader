// M7.2: spec tests for the sidecar environment built from Settings, and for
// keeping the API key out of every log.
//
// Interface choices where the spec is silent:
//   - One free function, declared in app/src/settings/Settings.h and defined
//     in app/src/settings/Settings.cpp:
//
//         std::map<wxString, wxString> buildSidecarEnv(
//             const std::map<wxString, wxString>& inherited,
//             bool useLocalLogin, const wxString& model, const wxString& apiKey);
//
//     `inherited` is the app's own environment (the app passes the result of
//     wxGetEnvMap()). useLocalLogin, model and apiKey are
//     Settings::useLocalLogin(), Settings::model() and Settings::apiKey(); an
//     empty apiKey means no key is stored or the key store is unavailable.
//     The function returns the complete environment for the sidecar. It is
//     pure: no process environment, config or key store access.
//   - Rules, in order (spec items 1-4):
//       1. Start from `inherited` and remove ANTHROPIC_API_KEY,
//          CLAUDE_CODE_OAUTH_TOKEN and ANTHROPIC_AUTH_TOKEN, in both modes.
//          Every other variable passes through with its value unchanged.
//       2. useLocalLogin false and apiKey not empty: ANTHROPIC_API_KEY = apiKey.
//          useLocalLogin false and apiKey empty: ANTHROPIC_API_KEY stays
//          absent. It is never set to an empty string.
//       3. useLocalLogin true: no ANTHROPIC_API_KEY, even when apiKey is not
//          empty.
//       4. model not empty: READER_MODEL = model, replacing any inherited
//          value. model empty: READER_MODEL is not added, and an inherited
//          READER_MODEL (a developer's shell setting) passes through like any
//          other variable.
//   - How the app feeds this env to SidecarClient is the implementer's
//     choice. The log test below uses only SidecarClient::setEnvironment()
//     (M5.1, also used by KeyTester), which must keep putting the given
//     ANTHROPIC_API_KEY into the spawned sidecar's environment.
//   - Logging (carried M5.1 review note): the API key value never appears in
//     <user data dir>/logs/sidecar.log or in any wxLog message, even when the
//     sidecar command line (READER_SIDECAR_CMD) contains it. Redacting the
//     key or not logging such a line both pass.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "agent/SidecarClient.h"
#include "settings/Settings.h"

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

using Env = std::map<wxString, wxString>;

// Obviously fake. Never a real key.
const wxString kFakeKey = "sk-ant-test-FAKEKEY0123456789";
const wxString kStrayKey = "sk-ant-test-STRAYKEY0123456789";

// A typical inherited environment with stray credentials from the shell.
Env strayEnv()
{
    return {
        {"PATH", "/usr/local/bin:/usr/bin:/bin"},
        {"HOME", "/home/reader"},
        {"LANG", "C.UTF-8"},
        {"READER_SIDECAR_CMD", "/opt/reader/reader-sidecar"},
        {"ANTHROPIC_API_KEY", kStrayKey},
        {"CLAUDE_CODE_OAUTH_TOKEN", "stray-oauth-token"},
        {"ANTHROPIC_AUTH_TOKEN", "stray-auth-token"},
    };
}

// strayEnv() without the three credential variables.
Env cleanEnv()
{
    Env env = strayEnv();
    env.erase("ANTHROPIC_API_KEY");
    env.erase("CLAUDE_CODE_OAUTH_TOKEN");
    env.erase("ANTHROPIC_AUTH_TOKEN");
    return env;
}

bool has(const Env& env, const wxString& name)
{
    return env.count(name) == 1;
}

} // namespace

TEST_CASE("the three credential variables are stripped in both modes")
{
    for (const bool local : {false, true}) {
        CAPTURE(local);
        const Env env = buildSidecarEnv(strayEnv(), local, wxString(), wxString());
        CHECK_FALSE(has(env, "ANTHROPIC_API_KEY"));
        CHECK_FALSE(has(env, "CLAUDE_CODE_OAUTH_TOKEN"));
        CHECK_FALSE(has(env, "ANTHROPIC_AUTH_TOKEN"));
        // Nothing else changes: no model, no key.
        CHECK(env == cleanEnv());
    }
}

TEST_CASE("dev toggle off: ANTHROPIC_API_KEY comes from the key store")
{
    const Env env = buildSidecarEnv(strayEnv(), false, wxString(), kFakeKey);
    REQUIRE(has(env, "ANTHROPIC_API_KEY"));
    CHECK(env.at("ANTHROPIC_API_KEY") == kFakeKey);
    CHECK_FALSE(has(env, "CLAUDE_CODE_OAUTH_TOKEN"));
    CHECK_FALSE(has(env, "ANTHROPIC_AUTH_TOKEN"));
    CHECK_FALSE(has(env, "READER_MODEL"));

    Env expected = cleanEnv();
    expected["ANTHROPIC_API_KEY"] = kFakeKey;
    CHECK(env == expected);
}

TEST_CASE("dev toggle off with no stored key: no ANTHROPIC_API_KEY at all")
{
    const Env env = buildSidecarEnv(strayEnv(), false, wxString(), wxString());
    // Absent, not an empty string, and the stray shell key is gone.
    CHECK_FALSE(has(env, "ANTHROPIC_API_KEY"));
    CHECK(env == cleanEnv());
}

TEST_CASE("dev toggle on: no key, even when one is stored")
{
    const Env env = buildSidecarEnv(strayEnv(), true, wxString(), kFakeKey);
    CHECK_FALSE(has(env, "ANTHROPIC_API_KEY"));
    CHECK_FALSE(has(env, "CLAUDE_CODE_OAUTH_TOKEN"));
    CHECK_FALSE(has(env, "ANTHROPIC_AUTH_TOKEN"));
    CHECK(env == cleanEnv());
    for (const auto& [name, value] : env) {
        CAPTURE(name.utf8_string());
        CHECK_FALSE(value.Contains("FAKEKEY"));
    }
}

TEST_CASE("READER_MODEL is set when a model is configured")
{
    for (const bool local : {false, true}) {
        CAPTURE(local);
        const Env env = buildSidecarEnv(strayEnv(), local, "claude-test-model", kFakeKey);
        REQUIRE(has(env, "READER_MODEL"));
        CHECK(env.at("READER_MODEL") == "claude-test-model");
        CHECK(has(env, "ANTHROPIC_API_KEY") == !local);
    }

    // A configured model replaces an inherited READER_MODEL.
    Env inherited = strayEnv();
    inherited["READER_MODEL"] = "shell-model";
    const Env env = buildSidecarEnv(inherited, false, "claude-test-model", wxString());
    REQUIRE(has(env, "READER_MODEL"));
    CHECK(env.at("READER_MODEL") == "claude-test-model");
}

TEST_CASE("an empty model adds no READER_MODEL and keeps an inherited one")
{
    CHECK_FALSE(has(buildSidecarEnv(strayEnv(), false, wxString(), kFakeKey), "READER_MODEL"));
    CHECK_FALSE(has(buildSidecarEnv(strayEnv(), true, wxString(), wxString()), "READER_MODEL"));

    Env inherited = strayEnv();
    inherited["READER_MODEL"] = "shell-model";
    const Env env = buildSidecarEnv(inherited, true, wxString(), wxString());
    REQUIRE(has(env, "READER_MODEL"));
    CHECK(env.at("READER_MODEL") == "shell-model");
}

TEST_CASE("an empty inherited environment gets only the settings")
{
    CHECK(buildSidecarEnv(Env{}, true, wxString(), kFakeKey).empty());
    CHECK(buildSidecarEnv(Env{}, false, "m", kFakeKey)
          == Env{{"ANTHROPIC_API_KEY", kFakeKey}, {"READER_MODEL", "m"}});
}

#ifndef _WIN32

namespace
{

// Writes whether the key reached it (never the key itself), prints `ready`,
// then waits for stdin EOF.
std::string fakeSidecar()
{
    return "#!/bin/sh\n"
           "dir=\"$(dirname \"$0\")\"\n"
           "if [ \"${ANTHROPIC_API_KEY:-}\" = \""
        + kFakeKey.utf8_string()
        + "\" ]; then echo match > \"$dir/key\"; else echo nomatch > \"$dir/key\"; fi\n"
          "echo 'fake sidecar stderr line' >&2\n"
          "echo '{\"type\":\"ready\",\"version\":\"fake-m7.2\"}'\n"
          "while IFS= read -r line; do :; done\n";
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool mentionsKey(const std::string& text)
{
    return text.find(kFakeKey.utf8_string()) != std::string::npos
        || text.find("FAKEKEY0123456789") != std::string::npos
        || text.find("sk-ant-test-FAKE") != std::string::npos;
}

// Records every wxLog message.
class RecordingLog : public wxLog
{
public:
    std::vector<std::string> messages;

protected:
    void DoLogRecord(wxLogLevel /*level*/, const wxString& msg,
                     const wxLogRecordInfo& /*info*/) override
    {
        messages.push_back(msg.utf8_string());
    }
};

struct Sink : wxEvtHandler
{
    int ready = 0;

    Sink()
    {
        Bind(EVT_SIDECAR_READY, [this](wxThreadEvent&) { ++ready; });
        Bind(EVT_SIDECAR_DOWN, [](wxThreadEvent&) {});
        Bind(EVT_SIDECAR_MESSAGE, [](wxThreadEvent&) {});
    }
};

bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    wxConsoleEventLoop loop;
    wxEventLoopActivator activate(&loop);
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        loop.DispatchTimeout(20);
        wxTheApp->ProcessPendingEvents();
    }
    return true;
}

struct LogFixture
{
    fs::path dir;
    RecordingLog* log = new RecordingLog;
    wxLog* oldLog = nullptr;

    LogFixture()
    {
        dir = fs::temp_directory_path()
            / ("reader-sidecar-env-test-" + std::to_string(wxGetProcessId()));
        fs::remove_all(dir);
        fs::create_directories(dir / "home");
        std::ofstream(dir / "fake.sh") << fakeSidecar();
        wxSetEnv("HOME", (dir / "home").string());
        wxSetEnv("XDG_DATA_HOME", (dir / "home" / ".local" / "share").string());
        wxUnsetEnv("ANTHROPIC_API_KEY");
        oldLog = wxLog::SetActiveTarget(log);
        wxLog::SetLogLevel(wxLOG_Max);
        wxLog::SetVerbose(true);
    }

    ~LogFixture()
    {
        wxLog::SetActiveTarget(oldLog);
        delete log;
        wxUnsetEnv("READER_SIDECAR_CMD");
        fs::remove_all(dir);
    }

    // Starts a client with the key in its environment, waits for `ready`,
    // shuts it down, and checks that the key reached the sidecar but no log.
    void runAndCheck()
    {
        Sink sink;
        {
            SidecarClient client(&sink);
            client.setEnvironment({{"ANTHROPIC_API_KEY", kFakeKey}});
            REQUIRE(client.start());
            REQUIRE(runUntil([&] { return sink.ready == 1; }, std::chrono::seconds(10)));
            client.shutdown();
        }

        // The key reached the sidecar, so the log check below is not vacuous.
        CHECK(readFile(dir / "key") == "match\n");

        const std::string sidecarLog = readFile(SidecarClient::logPath().utf8_string());
        CHECK(sidecarLog.find("fake sidecar stderr line") != std::string::npos);
        CHECK(sidecarLog.find("[app] ready, sidecar version fake-m7.2") != std::string::npos);
        CHECK_FALSE(mentionsKey(sidecarLog));

        for (const std::string& message : log->messages) {
            CAPTURE(message.size());
            CHECK_FALSE(mentionsKey(message));
        }
    }
};

} // namespace

TEST_CASE_FIXTURE(LogFixture, "the key reaches the sidecar env but never the logs")
{
    wxSetEnv("READER_SIDECAR_CMD", "sh \"" + (dir / "fake.sh").string() + "\"");
    runAndCheck();
}

TEST_CASE_FIXTURE(LogFixture, "a sidecar command line holding the key is not logged with it")
{
    // A developer's READER_SIDECAR_CMD that passes the same key on the
    // command line. The logged command line must not show it.
    wxSetEnv("READER_SIDECAR_CMD", "env ANTHROPIC_API_KEY=" + kFakeKey + " sh \""
                                       + (dir / "fake.sh").string() + "\"");
    runAndCheck();
}

#endif // _WIN32

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
