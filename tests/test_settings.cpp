// M7.1: spec tests for Settings (app/src/settings/Settings.h).
//
// The tests need no window and no secret service. Each case writes a
// wxFileConfig to a fresh temp file and uses an in-memory fake key store.
//
// Interface choices where the spec is silent (all declared in Settings.h):
//   - KeyStore is an abstract class over the system secret store. Its methods
//     mirror wxSecretStore so the real adapter is a thin wrapper:
//         virtual ~KeyStore() = default;
//         virtual bool isOk(wxString* why) const = 0;
//         virtual bool save(const wxString& service, const wxString& username,
//                           const wxString& secret) = 0;
//         virtual bool load(const wxString& service, wxString& username,
//                           wxString& secret) const = 0;
//         virtual bool erase(const wxString& service) = 0;
//     isOk() returns false and sets *why (when not null) to a non-empty reason
//     when keys cannot be stored, for example with no secret service.
//   - std::unique_ptr<KeyStore> makeSystemKeyStore() returns the adapter over
//     wxSecretStore::GetDefault(). The tests do not call it.
//   - Settings(wxConfigBase& config, KeyStore& keys) keeps both references.
//     The app passes *wxConfig::Get() and the system key store. Settings owns
//     neither.
//   - Settings::kKeyService == "agentic-reader" and
//     Settings::kKeyUsername == "anthropic-api-key" (spec item 1), declared as
//     static constexpr const char*.
//   - model() returns wxString, empty by default (the SDK default).
//     setModel(const wxString&) stores it in the config.
//   - useLocalLogin() returns bool, false by default. setUseLocalLogin(bool)
//     stores it in the config.
//   - setModel() and setUseLocalLogin() flush the config before returning, so
//     a killed app keeps them. The tests check this by reading the file with a
//     second wxFileConfig while the first is still alive.
//   - keyStoreAvailable(wxString* why = nullptr) const forwards to
//     KeyStore::isOk().
//   - apiKey() const returns the stored key, or an empty wxString when none is
//     stored or the key store is unavailable.
//   - setApiKey(const wxString& key, wxString* error = nullptr) saves the key
//     with KeyStore::save(kKeyService, kKeyUsername, key) and returns true. An
//     empty key erases the stored key. When the store is unavailable or the
//     save fails it returns false, sets *error (when not null) to a non-empty
//     message, and writes nothing anywhere: no plain-text fallback.
//   - Settings never writes the key to the config, under any entry name.
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "settings/Settings.h"

#include <wx/confbase.h>
#include <wx/fileconf.h>
#include <wx/init.h>
#include <wx/utils.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// Obviously fake. Never a real key.
const wxString kFakeKey = "sk-ant-test-FAKEKEY0123456789";

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// In-memory KeyStore. Records every call.
class FakeKeyStore : public KeyStore
{
public:
    bool ok = true;
    bool failSave = false;
    std::map<wxString, std::pair<wxString, wxString>> entries; // service -> (user, secret)
    int saveCalls = 0;
    int eraseCalls = 0;
    wxString lastService;
    wxString lastUsername;

    bool isOk(wxString* why) const override
    {
        if (!ok && why)
            *why = "No secret service is running";
        return ok;
    }

    bool save(const wxString& service, const wxString& username,
              const wxString& secret) override
    {
        ++saveCalls;
        lastService = service;
        lastUsername = username;
        if (!ok || failSave)
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
        ++eraseCalls;
        if (!ok)
            return false;
        return entries.erase(service) > 0;
    }
};

std::unique_ptr<wxFileConfig> openConfig(const fs::path& file)
{
    return std::make_unique<wxFileConfig>("agentic-reader-test", wxEmptyString,
                                          wxString::FromUTF8(file.string()), wxEmptyString,
                                          wxCONFIG_USE_LOCAL_FILE);
}

// Every value in the config, from every group.
void collectValues(wxConfigBase& config, const wxString& group, std::vector<wxString>& out)
{
    const wxString oldPath = config.GetPath();
    config.SetPath(group);

    wxString name;
    long index = 0;
    for (bool more = config.GetFirstEntry(name, index); more;
         more = config.GetNextEntry(name, index)) {
        out.push_back(name);
        out.push_back(config.Read(name, wxString()));
    }

    std::vector<wxString> groups;
    index = 0;
    for (bool more = config.GetFirstGroup(name, index); more;
         more = config.GetNextGroup(name, index))
        groups.push_back(name);
    for (const wxString& g : groups)
        collectValues(config, g, out);

    config.SetPath(oldPath);
}

bool configMentionsKey(wxConfigBase& config)
{
    std::vector<wxString> values;
    collectValues(config, "/", values);
    for (const wxString& v : values)
        if (v.Contains(kFakeKey.Left(10)))
            return true;
    return false;
}

struct Fixture
{
    fs::path dir;
    fs::path file;
    FakeKeyStore keys;

    Fixture()
    {
        static std::atomic<int> counter{0};
        dir = fs::temp_directory_path()
            / ("reader-settings-test-" + std::to_string(wxGetProcessId()) + "-"
               + std::to_string(counter++));
        fs::remove_all(dir);
        fs::create_directories(dir);
        file = dir / "agentic-reader.conf";
    }

    ~Fixture()
    {
        fs::remove_all(dir);
    }
};

} // namespace

TEST_CASE("key store names match the spec")
{
    CHECK(wxString(Settings::kKeyService) == "agentic-reader");
    CHECK(wxString(Settings::kKeyUsername) == "anthropic-api-key");
}

TEST_CASE_FIXTURE(Fixture, "defaults on an empty config")
{
    auto config = openConfig(file);
    Settings settings(*config, keys);
    CHECK(settings.model().empty());
    CHECK_FALSE(settings.useLocalLogin());
    CHECK(settings.apiKey().empty());
    CHECK(settings.keyStoreAvailable());
}

TEST_CASE_FIXTURE(Fixture, "model and dev login round-trip through the config file")
{
    {
        auto config = openConfig(file);
        Settings settings(*config, keys);
        settings.setModel("claude-test-model");
        settings.setUseLocalLogin(true);
        CHECK(settings.model() == "claude-test-model");
        CHECK(settings.useLocalLogin());

        // Flushed already: a second reader sees both values while the first
        // config object is still alive.
        auto other = openConfig(file);
        Settings reread(*other, keys);
        CHECK(reread.model() == "claude-test-model");
        CHECK(reread.useLocalLogin());
    }

    {
        auto config = openConfig(file);
        Settings settings(*config, keys);
        CHECK(settings.model() == "claude-test-model");
        CHECK(settings.useLocalLogin());

        // Back to the defaults.
        settings.setModel("");
        settings.setUseLocalLogin(false);
    }

    auto config = openConfig(file);
    Settings settings(*config, keys);
    CHECK(settings.model().empty());
    CHECK_FALSE(settings.useLocalLogin());
}

TEST_CASE_FIXTURE(Fixture, "the API key goes to the key store, never to the config")
{
    {
        auto config = openConfig(file);
        Settings settings(*config, keys);
        wxString error;
        REQUIRE(settings.setApiKey(kFakeKey, &error));
        settings.setModel("claude-test-model");
        settings.setUseLocalLogin(true);

        CHECK(keys.saveCalls == 1);
        CHECK(keys.lastService == "agentic-reader");
        CHECK(keys.lastUsername == "anthropic-api-key");
        REQUIRE(keys.entries.count("agentic-reader") == 1);
        CHECK(keys.entries["agentic-reader"].second == kFakeKey);

        CHECK(settings.apiKey() == kFakeKey);
        CHECK_FALSE(configMentionsKey(*config));
    }

    // The key survives a "restart": new config and Settings, same key store.
    auto config = openConfig(file);
    Settings settings(*config, keys);
    CHECK(settings.apiKey() == kFakeKey);
    CHECK_FALSE(configMentionsKey(*config));

    const std::string raw = readFile(file);
    CHECK(raw.find("claude-test-model") != std::string::npos);
    CHECK(raw.find(kFakeKey.Left(10).utf8_string()) == std::string::npos);
}

TEST_CASE_FIXTURE(Fixture, "an empty key erases the stored key")
{
    auto config = openConfig(file);
    Settings settings(*config, keys);
    REQUIRE(settings.setApiKey(kFakeKey));
    REQUIRE(settings.apiKey() == kFakeKey);

    CHECK(settings.setApiKey(""));
    CHECK(keys.eraseCalls >= 1);
    CHECK(keys.entries.count("agentic-reader") == 0);
    CHECK(settings.apiKey().empty());
}

TEST_CASE_FIXTURE(Fixture, "an unavailable key store reports why and stores nothing")
{
    keys.ok = false;
    {
        auto config = openConfig(file);
        Settings settings(*config, keys);

        wxString why;
        CHECK_FALSE(settings.keyStoreAvailable(&why));
        CHECK_FALSE(why.empty());

        wxString error;
        CHECK_FALSE(settings.setApiKey(kFakeKey, &error));
        CHECK_FALSE(error.empty());
        CHECK(keys.entries.empty());
        CHECK(settings.apiKey().empty());

        // Model and dev login still work without a key store.
        settings.setModel("claude-test-model");
        settings.setUseLocalLogin(true);
        CHECK_FALSE(configMentionsKey(*config));
    }

    // No plain-text fallback: the file has the model but not the key, and a
    // later run with a working store still finds no key.
    const std::string raw = readFile(file);
    CHECK(raw.find("claude-test-model") != std::string::npos);
    CHECK(raw.find(kFakeKey.Left(10).utf8_string()) == std::string::npos);

    keys.ok = true;
    auto config = openConfig(file);
    Settings settings(*config, keys);
    CHECK(settings.apiKey().empty());
    CHECK(settings.model() == "claude-test-model");
    CHECK(settings.useLocalLogin());
}

TEST_CASE_FIXTURE(Fixture, "a failed save reports an error and keeps the key out of the config")
{
    keys.failSave = true;
    {
        auto config = openConfig(file);
        Settings settings(*config, keys);
        wxString error;
        CHECK_FALSE(settings.setApiKey(kFakeKey, &error));
        CHECK_FALSE(error.empty());
        CHECK(settings.apiKey().empty());
        CHECK_FALSE(configMentionsKey(*config));
    }
    if (fs::exists(file))
        CHECK(readFile(file).find(kFakeKey.Left(10).utf8_string()) == std::string::npos);
}

int main(int argc, char** argv)
{
    wxInitializer init;
    if (!init.IsOk())
        return 1;
    doctest::Context context(argc, argv);
    return context.run();
}
