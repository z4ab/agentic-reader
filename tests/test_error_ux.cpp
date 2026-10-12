// M7.3: spec tests for the chat pane's first-run check and its action for
// each sidecar `error` kind (sidecar/PROTOCOL.md). No window is created.
// tests/acceptance/m7.3.steps checks what the user sees.
//
// Interface choices where the spec is silent, all declared in
// app/src/chat/SetupBanner.h (the spec's new header) next to the SetupBanner
// class, so ChatPanel and the tests share one pure mapping:
//
//   // What the chat pane does with an `error` message, by its kind.
//   enum class ErrorAction
//   {
//       Ignore,      // busy: no notice, a running reply keeps going
//       Stopped,     // interrupted: the grey "Stopped." line, as in M5.2
//       AuthBanner,  // auth: SetupBanner "API key rejected" + Open Settings
//       RetryNotice, // rate_limit: inline notice with a Retry button
//       Reinstall,   // cli_missing: "Installation is incomplete, reinstall the app"
//       Restarting,  // process: "Agent stopped, restarting…"
//       Inline,      // every other kind, unknown kinds included
//   };
//   ErrorAction errorAction(std::string_view kind);
//
//   // True when the sidecar has closed the chat, so the pane reopens it
//   // (the M5.2 reopen path): unknown_chat, cli_missing and process.
//   bool errorClosesChat(std::string_view kind);
//
//   // True when the first-run banner replaces the input: no API key is
//   // stored (Settings::apiKey() is empty, which includes an unavailable key
//   // store) and the developer login is off.
//   bool needsSetup(const Settings& settings);
//
//   class SetupBanner : public wxPanel
//   {
//   public:
//       // Target of the banner's console link.
//       static constexpr const char* kKeysUrl =
//           "https://console.anthropic.com/settings/keys";
//       ...
//   };
//
// Kinds are matched exactly (case-sensitive), as PROTOCOL.md spells them. A
// kind PROTOCOL.md does not list is a generic failure: Inline.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "chat/SetupBanner.h"
#include "settings/Settings.h"

#include <wx/fileconf.h>
#include <wx/init.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace doctest
{
template <>
struct StringMaker<ErrorAction>
{
    static String convert(ErrorAction action)
    {
        switch (action) {
        case ErrorAction::Ignore:
            return "Ignore";
        case ErrorAction::Stopped:
            return "Stopped";
        case ErrorAction::AuthBanner:
            return "AuthBanner";
        case ErrorAction::RetryNotice:
            return "RetryNotice";
        case ErrorAction::Reinstall:
            return "Reinstall";
        case ErrorAction::Restarting:
            return "Restarting";
        case ErrorAction::Inline:
            return "Inline";
        }
        return "ErrorAction(?)";
    }
};
} // namespace doctest

namespace
{

// Obviously fake. Never a real key.
const wxString kFakeKey = "sk-ant-test-FAKEKEY0123456789";

// In-memory KeyStore.
class FakeKeyStore : public KeyStore
{
public:
    bool ok = true;
    std::map<wxString, std::pair<wxString, wxString>> entries; // service -> (user, secret)

    bool isOk(wxString* why) const override
    {
        if (!ok && why)
            *why = "No secret service is running";
        return ok;
    }

    bool save(const wxString& service, const wxString& username,
              const wxString& secret) override
    {
        if (!ok)
            return false;
        entries[service] = {username, secret};
        return true;
    }

    bool load(const wxString& service, wxString& username, wxString& secret) const override
    {
        if (!ok)
            return false;
        const auto it = entries.find(service);
        if (it == entries.end())
            return false;
        username = it->second.first;
        secret = it->second.second;
        return true;
    }

    bool erase(const wxString& service) override
    {
        if (!ok)
            return false;
        return entries.erase(service) > 0;
    }
};

// A wxFileConfig over a fresh temp file, removed with the fixture.
struct ConfigFixture
{
    fs::path dir;
    std::unique_ptr<wxFileConfig> config;
    FakeKeyStore keys;

    ConfigFixture()
    {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path()
              / ("m73-error-ux-" + std::to_string(wxGetProcessId()) + "-"
                 + std::to_string(counter++));
        fs::create_directories(dir);
        config = std::make_unique<wxFileConfig>(
            "agentic-reader-test", wxEmptyString,
            wxString::FromUTF8((dir / "config.ini").string()), wxEmptyString,
            wxCONFIG_USE_LOCAL_FILE);
    }

    ~ConfigFixture()
    {
        config.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

TEST_CASE("auth shows the API key banner")
{
    CHECK(errorAction("auth") == ErrorAction::AuthBanner);
}

TEST_CASE("rate_limit shows an inline notice with Retry")
{
    CHECK(errorAction("rate_limit") == ErrorAction::RetryNotice);
}

TEST_CASE("cli_missing asks for a reinstall")
{
    CHECK(errorAction("cli_missing") == ErrorAction::Reinstall);
}

TEST_CASE("process says the agent is restarting")
{
    CHECK(errorAction("process") == ErrorAction::Restarting);
}

TEST_CASE("busy is ignored")
{
    CHECK(errorAction("busy") == ErrorAction::Ignore);
}

TEST_CASE("interrupted keeps the M5.2 Stopped line")
{
    CHECK(errorAction("interrupted") == ErrorAction::Stopped);
}

TEST_CASE("every other kind is an inline message")
{
    for (const char* kind : {"protocol", "no_project", "chat_exists", "unknown_chat", "result",
                             "internal"}) {
        CAPTURE(std::string(kind));
        CHECK(errorAction(kind) == ErrorAction::Inline);
    }
}

TEST_CASE("unknown kinds are inline messages")
{
    CHECK(errorAction("mystery_kind") == ErrorAction::Inline);
    CHECK(errorAction("") == ErrorAction::Inline);
    // Kinds are matched exactly.
    CHECK(errorAction("AUTH") == ErrorAction::Inline);
    CHECK(errorAction("Rate_Limit") == ErrorAction::Inline);
    CHECK(errorAction("busy ") == ErrorAction::Inline);
    CHECK(errorAction("process_failed") == ErrorAction::Inline);
}

TEST_CASE("the mapping accepts a std::string kind")
{
    const std::string kind = "rate_limit";
    CHECK(errorAction(kind) == ErrorAction::RetryNotice);
}

TEST_CASE("errors that close the chat reopen it")
{
    // PROTOCOL.md: these leave the chat closed in the sidecar.
    CHECK(errorClosesChat("unknown_chat"));
    CHECK(errorClosesChat("cli_missing"));
    CHECK(errorClosesChat("process"));
}

TEST_CASE("errors that leave the chat open do not reopen it")
{
    for (const char* kind : {"busy", "interrupted", "internal", "result", "protocol",
                             "no_project", "chat_exists", "mystery_kind", ""}) {
        CAPTURE(std::string(kind));
        CHECK_FALSE(errorClosesChat(kind));
    }
}

TEST_CASE("the console link goes to the API keys page")
{
    CHECK(std::string(SetupBanner::kKeysUrl) == "https://console.anthropic.com/settings/keys");
}

TEST_CASE("first run: no key and the developer login off")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    CHECK(needsSetup(settings));
}

TEST_CASE("a stored key ends first run")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    REQUIRE(settings.setApiKey(kFakeKey));
    CHECK_FALSE(needsSetup(settings));
}

TEST_CASE("the developer login ends first run without a key")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    settings.setUseLocalLogin(true);
    CHECK_FALSE(needsSetup(settings));
}

TEST_CASE("an erased key brings first run back")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    REQUIRE(settings.setApiKey(kFakeKey));
    REQUIRE(settings.setApiKey(wxString()));
    CHECK(needsSetup(settings));
}

TEST_CASE("turning the developer login off again brings first run back")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    settings.setUseLocalLogin(true);
    settings.setUseLocalLogin(false);
    CHECK(needsSetup(settings));
}

TEST_CASE("an unavailable key store means no key")
{
    ConfigFixture f;
    f.keys.ok = false;
    Settings settings(*f.config, f.keys);
    CHECK(needsSetup(settings));

    settings.setUseLocalLogin(true);
    CHECK_FALSE(needsSetup(settings));
}

TEST_CASE("a key in the store that the store cannot reach does not count")
{
    ConfigFixture f;
    Settings settings(*f.config, f.keys);
    REQUIRE(settings.setApiKey(kFakeKey));
    f.keys.ok = false; // the secret service went away
    CHECK(needsSetup(settings));
}

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
