// M5.1: SidecarClient against a fake sidecar (a POSIX shell script).
//
// The fake writes its PID to a file, prints `ready`, then records each stdin
// line in a file and answers it with a `text_delta`. It exits on stdin EOF.
// The test runs a console event loop, so it needs no display. HOME points at a
// temporary directory so sidecar.log does not land in the real home.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "agent/SidecarClient.h"

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/process.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

const char* const kFakeSidecar = R"sh(#!/bin/sh
dir="$(dirname "$0")"
echo $$ > "$dir/pid"
echo "fake sidecar stderr line" >&2
echo '{"type":"ready","version":"fake-1"}'
while IFS= read -r line; do
  printf '%s\n' "$line" >> "$dir/received"
  echo '{"type":"text_delta","chat_id":"c","text":"got"}'
done
)sh";

std::string readFile(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> readLinesOf(const fs::path& path)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        lines.push_back(line);
    return lines;
}

// Records the events SidecarClient sends to its sink.
struct Sink : wxEvtHandler
{
    int ready = 0;
    int down = 0;
    std::vector<protocol::Outbound> messages;
    std::string version;

    Sink()
    {
        Bind(EVT_SIDECAR_READY, [this](wxThreadEvent& e) {
            ++ready;
            version = e.GetString().utf8_string();
        });
        Bind(EVT_SIDECAR_DOWN, [this](wxThreadEvent&) { ++down; });
        Bind(EVT_SIDECAR_MESSAGE, [this](wxThreadEvent& e) {
            messages.push_back(e.GetPayload<protocol::Outbound>());
        });
    }
};

// Runs the event loop until `done` returns true or `timeout` passes.
bool runUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout)
{
    // wxEventLoop is the GTK loop in this build. A console app needs this one.
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
            / ("reader-sidecar-test-" + std::to_string(wxGetProcessId()));
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

    long fakePid() const
    {
        return std::stol(readFile(dir / "pid"));
    }
};

using namespace std::chrono_literals;

} // namespace

TEST_CASE_FIXTURE(Fixture, "locateCommand prefers READER_SIDECAR_CMD")
{
    CHECK(SidecarClient::locateCommand() == "sh \"" + (dir / "fake.sh").string() + "\"");
}

TEST_CASE_FIXTURE(Fixture, "sends queue until ready, messages arrive, restart replays open_project")
{
    Sink sink;
    SidecarClient client(&sink);
    CHECK_FALSE(client.isReady());
    REQUIRE(client.start());

    // Sent before `ready`: queued, then written once the sidecar is ready.
    protocol::OpenProject open;
    open.path = "/tmp/MyProject";
    open.seq = 1;
    client.send(open);

    REQUIRE(runUntil([&] { return sink.ready == 1 && sink.messages.size() == 1; }, 10s));
    CHECK(client.isReady());
    CHECK(sink.version == "fake-1");
    REQUIRE(std::holds_alternative<protocol::TextDelta>(sink.messages[0]));
    CHECK(std::get<protocol::TextDelta>(sink.messages[0]).text == "got");
    CHECK(readLinesOf(dir / "received")
          == std::vector<std::string>{R"({"path":"/tmp/MyProject","seq":1,"type":"open_project"})"});

    // A message sent while ready goes straight through.
    protocol::NewChat chat;
    chat.chatId = "c";
    client.send(chat);
    REQUIRE(runUntil([&] { return sink.messages.size() == 2; }, 5s));
    CHECK(readLinesOf(dir / "received").size() == 2);

    // Killing the sidecar reports it down, restarts it after 1 s, and
    // re-sends the last open_project to the new process.
    const long pid = fakePid();
    CHECK(wxProcess::Kill(static_cast<int>(pid), wxSIGKILL) == wxKILL_OK);
    REQUIRE(runUntil([&] { return sink.down == 1; }, 5s));
    CHECK_FALSE(client.isReady());
    REQUIRE(runUntil([&] { return sink.ready == 2 && sink.messages.size() == 3; }, 10s));
    CHECK(client.isReady());
    CHECK(fakePid() != pid);
    const auto received = readLinesOf(dir / "received");
    REQUIRE(received.size() == 3);
    CHECK(received[2] == received[0]);

    // Shutdown closes stdin and the fake exits on EOF without a kill.
    const auto start = std::chrono::steady_clock::now();
    client.shutdown();
    CHECK(std::chrono::steady_clock::now() - start < 3s);
    CHECK_FALSE(client.isReady());
    CHECK(sink.down == 1);

    const std::string log = readFile(SidecarClient::logPath().utf8_string());
    CHECK(log.find("fake sidecar stderr line") != std::string::npos);
    CHECK(log.find("[app] ready, sidecar version fake-1") != std::string::npos);
    CHECK(log.find("[app] sidecar exited") != std::string::npos);
    CHECK(log.find("[app] sidecar stopped") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "a sidecar that keeps exiting restarts with doubling backoff")
{
    wxSetEnv("READER_SIDECAR_CMD", "false");
    Sink sink;
    SidecarClient client(&sink);
    REQUIRE(client.start());
    // Exits at 0 s, 1 s and 3 s, with restarts after 1 s, 2 s, then 4 s.
    REQUIRE(runUntil([&] { return sink.down == 3; }, 6s));
    CHECK(sink.ready == 0);
    client.shutdown();

    const std::string log = readFile(SidecarClient::logPath().utf8_string());
    const auto one = log.find("[app] restarting in 1 s");
    const auto two = log.find("[app] restarting in 2 s");
    const auto four = log.find("[app] restarting in 4 s");
    REQUIRE(one != std::string::npos);
    REQUIRE(two != std::string::npos);
    REQUIRE(four != std::string::npos);
    CHECK(one < two);
    CHECK(two < four);
}

TEST_CASE_FIXTURE(Fixture, "a sidecar that sends no ready within 10 s is killed and restarted")
{
    wxSetEnv("READER_SIDECAR_CMD", "sleep 60");
    Sink sink;
    SidecarClient client(&sink);
    REQUIRE(client.start());
    REQUIRE(runUntil([&] { return sink.down == 1; }, 13s));
    CHECK(sink.ready == 0);
    client.shutdown();

    const std::string log = readFile(SidecarClient::logPath().utf8_string());
    CHECK(log.find("[app] no ready within 10 s") != std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "start returns false when no sidecar is found")
{
    wxSetEnv("READER_SIDECAR_CMD", "");
    Sink sink;
    SidecarClient client(&sink);
    // The test binary has no reader-sidecar next to it.
    CHECK_FALSE(client.start());
    CHECK_FALSE(client.isReady());
}

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
