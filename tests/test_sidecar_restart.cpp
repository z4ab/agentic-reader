// M7.2 (implementer): SidecarClient::restart() against a fake sidecar, and
// Settings::sidecarEnv() over an in-memory key store.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "agent/SidecarClient.h"
#include "settings/Settings.h"

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/fileconf.h>
#include <wx/init.h>
#include <wx/sstream.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Env = std::map<wxString, wxString>;

namespace
{

// Appends "start <READER_MODEL>" to `log` at start, records stdin lines in
// `received`, and appends "exit" on stdin EOF.
const char* const kFakeSidecar = R"sh(#!/bin/sh
dir="$(dirname "$0")"
echo "start ${READER_MODEL:-unset}" >> "$dir/log"
echo '{"type":"ready","version":"fake-restart"}'
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$dir/received"
done
echo exit >> "$dir/log"
)sh";

std::vector<std::string> readLinesOf(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

struct Sink : wxEvtHandler
{
    int ready = 0;
    int down = 0;

    Sink()
    {
        Bind(EVT_SIDECAR_READY, [this](wxThreadEvent&) { ++ready; });
        Bind(EVT_SIDECAR_DOWN, [this](wxThreadEvent&) { ++down; });
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

struct Fixture
{
    fs::path dir;

    Fixture()
    {
        dir = fs::temp_directory_path()
            / ("reader-sidecar-restart-test-" + std::to_string(wxGetProcessId()));
        fs::remove_all(dir);
        fs::create_directories(dir / "home");
        std::ofstream(dir / "fake.sh") << kFakeSidecar;
        wxSetEnv("HOME", (dir / "home").string());
        wxSetEnv("XDG_DATA_HOME", (dir / "home" / ".local" / "share").string());
        wxSetEnv("READER_SIDECAR_CMD", "sh \"" + (dir / "fake.sh").string() + "\"");
    }

    ~Fixture()
    {
        wxUnsetEnv("READER_SIDECAR_CMD");
        fs::remove_all(dir);
    }
};

// In-memory key store that counts loads.
class FakeKeyStore : public KeyStore
{
public:
    wxString secret;
    mutable int loads = 0;

    bool isOk(wxString*) const override { return true; }
    bool save(const wxString&, const wxString&, const wxString& s) override
    {
        secret = s;
        return true;
    }
    bool load(const wxString&, wxString& username, wxString& s) const override
    {
        ++loads;
        if (secret.empty())
            return false;
        username = Settings::kKeyUsername;
        s = secret;
        return true;
    }
    bool erase(const wxString&) override
    {
        secret.clear();
        return true;
    }
};

using namespace std::chrono_literals;

} // namespace

TEST_CASE_FIXTURE(Fixture, "restart stops the sidecar and starts one with a fresh environment")
{
    Sink sink;
    SidecarClient client(&sink);
    int builds = 0;
    wxString model = "first-model";
    client.setEnvironmentSource([&](const Env& inherited) {
        ++builds;
        Env env = inherited;
        env["READER_MODEL"] = model;
        return env;
    });
    REQUIRE(client.start());
    REQUIRE(runUntil([&] { return sink.ready == 1; }, 10s));
    client.send(protocol::OpenProject{"/tmp/project", std::nullopt});

    model = "second-model";
    REQUIRE(client.restart());
    // Sent while the new sidecar starts: queued until it is ready.
    client.send(protocol::NewChat{"c1", std::nullopt});
    REQUIRE(runUntil([&] { return sink.ready == 2; }, 10s));
    CHECK(runUntil([&] { return readLinesOf(dir / "received").size() == 3; }, 5s));

    CHECK(builds == 2);
    CHECK(sink.down == 0);
    CHECK(readLinesOf(dir / "log")
          == std::vector<std::string>{"start first-model", "exit", "start second-model"});

    // The old sidecar got open_project; the new one gets it again first.
    const auto received = readLinesOf(dir / "received");
    REQUIRE(received.size() == 3);
    CHECK(received[0].find("\"open_project\"") != std::string::npos);
    CHECK(received[1].find("\"open_project\"") != std::string::npos);
    CHECK(received[2].find("\"new_chat\"") != std::string::npos);
    client.shutdown();
}

TEST_CASE_FIXTURE(Fixture, "restart before start starts the sidecar, after shutdown it does nothing")
{
    Sink sink;
    SidecarClient client(&sink);
    REQUIRE(client.restart());
    REQUIRE(runUntil([&] { return sink.ready == 1; }, 10s));
    client.shutdown();
    CHECK_FALSE(client.restart());
    CHECK(readLinesOf(dir / "log") == std::vector<std::string>{"start unset", "exit"});
}

TEST_CASE("Settings::sidecarEnv applies the settings and disables claude.ai connectors")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    const Env inherited{{"PATH", "/bin"}, {"ANTHROPIC_API_KEY", "sk-ant-test-STRAY0123456789"}};

    Env env = settings.sidecarEnv(inherited);
    CHECK(env == Env{{"PATH", "/bin"},
                     {"ANTHROPIC_API_KEY", keys.secret},
                     {"ENABLE_CLAUDEAI_MCP_SERVERS", "false"}});

    settings.setUseLocalLogin(true);
    settings.setModel("claude-test-model");
    keys.loads = 0;
    env = settings.sidecarEnv(inherited);
    CHECK(env == Env{{"PATH", "/bin"},
                     {"READER_MODEL", "claude-test-model"},
                     {"ENABLE_CLAUDEAI_MCP_SERVERS", "false"}});
    // The local login never reads the key store.
    CHECK(keys.loads == 0);
}

// M7.5 (tester): the claude.ai connectors setting.
//
// Interface choices (the spec names only the two methods):
//   - Settings::useClaudeAiConnectors() const and
//     Settings::setUseClaudeAiConnectors(bool on). The setter flushes the
//     config, like setUseLocalLogin().
//   - The config key is not named here. The round-trip case reads the value
//     back through a new wxFileConfig on the same file, so any key works.
//   - sidecarEnv() sets ENABLE_CLAUDEAI_MCP_SERVERS to "true" only when the
//     local login and the setting are both on. Otherwise it is "false".
//   - Only ENABLE_CLAUDEAI_MCP_SERVERS depends on the setting. The other
//     entries of sidecarEnv() are the same with it on or off.

TEST_CASE("claude.ai connectors setting is off by default")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    Settings settings(config, keys);
    CHECK_FALSE(settings.useClaudeAiConnectors());
}

TEST_CASE_FIXTURE(Fixture, "claude.ai connectors setting round-trips through the config file")
{
    const std::string path = (dir / "reader.conf").string();
    auto open = [&] {
        return std::make_unique<wxFileConfig>(wxEmptyString, wxEmptyString, path, wxEmptyString,
                                              wxCONFIG_USE_LOCAL_FILE);
    };
    FakeKeyStore keys;
    {
        auto config = open();
        Settings settings(*config, keys);
        settings.setUseClaudeAiConnectors(true);
    }
    {
        auto config = open();
        Settings settings(*config, keys);
        CHECK(settings.useClaudeAiConnectors());
        settings.setUseClaudeAiConnectors(false);
    }
    {
        auto config = open();
        Settings settings(*config, keys);
        CHECK_FALSE(settings.useClaudeAiConnectors());
    }
}

TEST_CASE("sidecarEnv in API-key mode gives ENABLE_CLAUDEAI_MCP_SERVERS=false with the setting on or off")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    const Env inherited{{"PATH", "/bin"}};
    const Env expected{{"PATH", "/bin"},
                       {"ANTHROPIC_API_KEY", keys.secret},
                       {"ENABLE_CLAUDEAI_MCP_SERVERS", "false"}};

    settings.setUseClaudeAiConnectors(false);
    CHECK(settings.sidecarEnv(inherited) == expected);
    settings.setUseClaudeAiConnectors(true);
    CHECK(settings.sidecarEnv(inherited) == expected);
}

TEST_CASE("sidecarEnv with local login and the setting off gives ENABLE_CLAUDEAI_MCP_SERVERS=false")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    settings.setUseLocalLogin(true);
    settings.setUseClaudeAiConnectors(false);

    const Env env = settings.sidecarEnv(Env{{"PATH", "/bin"}});
    CHECK(env == Env{{"PATH", "/bin"}, {"ENABLE_CLAUDEAI_MCP_SERVERS", "false"}});
}

TEST_CASE("sidecarEnv with local login and the setting on gives ENABLE_CLAUDEAI_MCP_SERVERS=true")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    settings.setUseLocalLogin(true);
    settings.setModel("claude-test-model");
    settings.setUseClaudeAiConnectors(true);

    const Env env = settings.sidecarEnv(Env{{"PATH", "/bin"}});
    CHECK(env == Env{{"PATH", "/bin"},
                     {"READER_MODEL", "claude-test-model"},
                     {"ENABLE_CLAUDEAI_MCP_SERVERS", "true"}});
    // The local login never reads the key store, with the setting on too.
    CHECK(keys.loads == 0);
}

TEST_CASE("the connectors setting changes only ENABLE_CLAUDEAI_MCP_SERVERS in sidecarEnv")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    const Env inherited{{"PATH", "/bin"},
                        {"HOME", "/home/tester"},
                        {"LANG", "C"},
                        {"READER_MODEL", "inherited-model"},
                        {"ANTHROPIC_API_KEY", "sk-ant-test-STRAY0123456789"},
                        {"CLAUDE_CODE_OAUTH_TOKEN", "stray-oauth-token"}};

    for (const bool local : {false, true}) {
        settings.setUseLocalLogin(local);
        settings.setUseClaudeAiConnectors(false);
        Env off = settings.sidecarEnv(inherited);
        settings.setUseClaudeAiConnectors(true);
        Env on = settings.sidecarEnv(inherited);

        CHECK(off["ENABLE_CLAUDEAI_MCP_SERVERS"] == "false");
        CHECK(on["ENABLE_CLAUDEAI_MCP_SERVERS"] == (local ? "true" : "false"));
        off.erase("ENABLE_CLAUDEAI_MCP_SERVERS");
        on.erase("ENABLE_CLAUDEAI_MCP_SERVERS");
        CHECK(on == off);
        CHECK(on["PATH"] == "/bin");
        CHECK(on["HOME"] == "/home/tester");
        CHECK(on["LANG"] == "C");
        CHECK(on["READER_MODEL"] == "inherited-model");
        CHECK(on.count("CLAUDE_CODE_OAUTH_TOKEN") == 0);
        if (local)
            CHECK(on.count("ANTHROPIC_API_KEY") == 0);
        else
            CHECK(on["ANTHROPIC_API_KEY"] == keys.secret);
    }
}

TEST_CASE("the connectors setting is kept when the local login is turned off")
{
    wxStringInputStream empty(wxString{});
    wxFileConfig config(empty);
    FakeKeyStore keys;
    keys.secret = "sk-ant-test-FAKEKEY0123456789";
    Settings settings(config, keys);
    settings.setUseClaudeAiConnectors(true);
    settings.setUseLocalLogin(true);
    settings.setUseLocalLogin(false);

    CHECK(settings.useClaudeAiConnectors());
    CHECK(settings.sidecarEnv(Env{{"PATH", "/bin"}})["ENABLE_CLAUDEAI_MCP_SERVERS"] == "false");
}

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
