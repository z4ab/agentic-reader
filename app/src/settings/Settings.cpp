#include "settings/Settings.h"

#include <wx/confbase.h>
#include <wx/log.h>
#include <wx/secretstore.h>

#include <optional>

namespace
{

constexpr const char* kModelEntry = "/Agent/Model";
constexpr const char* kLocalLoginEntry = "/Agent/UseLocalLogin";

#if wxUSE_SECRETSTORE

// Opens the system store on first use, so a session that never needs the
// key (the local login) never loads the secret service. wxSecretStore logs
// its own error when a call fails. Settings reports failures itself, so
// those logs are suppressed (one error box, not two).
class SystemKeyStore : public KeyStore
{
public:
    bool isOk(wxString* why) const override
    {
        return store().IsOk(why);
    }

    bool save(const wxString& service, const wxString& username,
              const wxString& secret) override
    {
        wxLogNull noLog;
        return store().Save(service, username, wxSecretValue(secret));
    }

    bool load(const wxString& service, wxString& username, wxString& secret) const override
    {
        wxLogNull noLog;
        wxSecretValue value;
        if (!store().Load(service, username, value))
            return false;
        secret = value.GetAsString();
        return true;
    }

    bool erase(const wxString& service) override
    {
        wxLogNull noLog;
        return store().Delete(service);
    }

private:
    wxSecretStore& store() const
    {
        if (!m_store)
            m_store.emplace(wxSecretStore::GetDefault());
        return *m_store;
    }

    mutable std::optional<wxSecretStore> m_store;
};

#else

// wx was built without wxSecretStore. Nothing can be stored.
class SystemKeyStore : public KeyStore
{
public:
    bool isOk(wxString* why) const override
    {
        if (why)
            *why = "this build has no secure key storage (wxSecretStore is disabled)";
        return false;
    }

    bool save(const wxString&, const wxString&, const wxString&) override
    {
        return false;
    }

    bool load(const wxString&, wxString&, wxString&) const override
    {
        return false;
    }

    bool erase(const wxString&) override
    {
        return false;
    }
};

#endif

} // namespace

std::unique_ptr<KeyStore> makeSystemKeyStore()
{
    return std::make_unique<SystemKeyStore>();
}

Settings::Settings(wxConfigBase& config, KeyStore& keys) : m_config(config), m_keys(keys) {}

wxString Settings::model() const
{
    return m_config.Read(kModelEntry, wxString());
}

void Settings::setModel(const wxString& model)
{
    m_config.Write(kModelEntry, model);
    m_config.Flush();
}

bool Settings::useLocalLogin() const
{
    return m_config.ReadBool(kLocalLoginEntry, false);
}

void Settings::setUseLocalLogin(bool on)
{
    m_config.Write(kLocalLoginEntry, on);
    m_config.Flush();
}

bool Settings::keyStoreAvailable(wxString* why) const
{
    return m_keys.isOk(why);
}

wxString Settings::apiKey() const
{
    if (!m_keys.isOk(nullptr))
        return wxString();
    wxString username;
    wxString secret;
    if (!m_keys.load(kKeyService, username, secret))
        return wxString();
    return secret;
}

bool Settings::setApiKey(const wxString& key, wxString* error)
{
    wxString why;
    if (!m_keys.isOk(&why)) {
        if (error)
            *error = "The API key cannot be stored securely: " + why;
        return false;
    }

    if (key.empty()) {
        if (apiKey().empty())
            return true;
        if (m_keys.erase(kKeyService))
            return true;
        if (error)
            *error = "The stored API key could not be removed from the system key store.";
        return false;
    }

    if (m_keys.save(kKeyService, kKeyUsername, key))
        return true;
    if (error)
        *error = "The API key could not be saved to the system key store.";
    return false;
}

std::map<wxString, wxString> Settings::sidecarEnv(
    const std::map<wxString, wxString>& inherited) const
{
    const bool local = useLocalLogin();
    auto env = buildSidecarEnv(inherited, local, model(), local ? wxString() : apiKey());
    // The app's assistant never uses the account's claude.ai MCP connectors
    // (Gmail, Drive, ...), which the CLI loads with a local login.
    env["ENABLE_CLAUDEAI_MCP_SERVERS"] = "false";
    return env;
}

std::map<wxString, wxString> buildSidecarEnv(const std::map<wxString, wxString>& inherited,
                                             bool useLocalLogin, const wxString& model,
                                             const wxString& apiKey)
{
    std::map<wxString, wxString> env = inherited;
    env.erase("ANTHROPIC_API_KEY");
    env.erase("CLAUDE_CODE_OAUTH_TOKEN");
    env.erase("ANTHROPIC_AUTH_TOKEN");
    if (!useLocalLogin && !apiKey.empty())
        env["ANTHROPIC_API_KEY"] = apiKey;
    if (!model.empty())
        env["READER_MODEL"] = model;
    return env;
}
