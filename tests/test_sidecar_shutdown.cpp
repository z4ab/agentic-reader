// M7.3 (implementer): SidecarClient::shutdownInBackground() returns at once
// and still stops a sidecar that ignores its closed stdin (the KeyTester
// freeze carried from M7.1).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "agent/SidecarClient.h"

#include <wx/app.h>
#include <wx/evtloop.h>
#include <wx/init.h>
#include <wx/utils.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{

// Ready at once. On stdin EOF it logs "eof" and waits 30 s, unless SIGTERM
// arrives, which logs "term". The sleep does not hold stdout.
const char* const kFakeSidecar = R"sh(#!/bin/sh
dir="$(dirname "$0")"
trap 'echo term >> "$dir/log"; exit 0' TERM
echo '{"type":"ready","version":"fake-shutdown"}'
while IFS= read -r line; do :; done
echo eof >> "$dir/log"
sleep 30 >/dev/null 2>&1 &
wait $!
echo done >> "$dir/log"
)sh";

std::string readFile(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
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
            / ("reader-sidecar-shutdown-test-" + std::to_string(wxGetProcessId()));
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

} // namespace

TEST_CASE_FIXTURE(Fixture, "shutdownInBackground returns at once and kills a lingering sidecar")
{
    Sink sink;
    {
        SidecarClient client(&sink);
        REQUIRE(client.start());
        REQUIRE(runUntil([&] { return sink.ready == 1; }, 10s));

        const auto before = std::chrono::steady_clock::now();
        client.shutdownInBackground();
        CHECK(std::chrono::steady_clock::now() - before < 500ms);
        // Destroying the client does not wait either.
    }

    // The sidecar saw EOF, ignored it, and got SIGTERM after about 3 s.
    const auto end = std::chrono::steady_clock::now() + 8s;
    while (readFile(dir / "log").find("term") == std::string::npos
           && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(50ms);
    CHECK(readFile(dir / "log") == "eof\nterm\n");
    CHECK(sink.down == 0);
}

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
