#pragma once

#include <wx/string.h>

#include <map>
#include <memory>

class wxConfigBase;

// The system secret store (wxSecretStore), behind an interface so tests can
// use a fake. Methods mirror wxSecretStore.
class KeyStore
{
public:
    virtual ~KeyStore() = default;

    // False when secrets cannot be stored, for example with no secret service
    // on Linux. Sets *why (when not null) to the reason.
    virtual bool isOk(wxString* why) const = 0;
    virtual bool save(const wxString& service, const wxString& username,
                      const wxString& secret) = 0;
    virtual bool load(const wxString& service, wxString& username, wxString& secret) const = 0;
    virtual bool erase(const wxString& service) = 0;
};

// The key store over wxSecretStore::GetDefault(). When wx was built without
// wxSecretStore, its isOk() is false and says so.
std::unique_ptr<KeyStore> makeSystemKeyStore();

// The app's settings: the API key in the key store, the model and the
// developer toggles in wxConfig. The key never goes to wxConfig, and there is no
// plain-text fallback when the key store is unavailable.
class Settings
{
public:
    static constexpr const char* kKeyService = "agentic-reader";
    static constexpr const char* kKeyUsername = "anthropic-api-key";

    // Keeps both references. Owns neither.
    Settings(wxConfigBase& config, KeyStore& keys);

    // Empty means the SDK default.
    wxString model() const;
    void setModel(const wxString& model);

    // "Use my local Claude Code login (developer)". Off by default.
    bool useLocalLogin() const;
    void setUseLocalLogin(bool on);

    // "Use my claude.ai connectors (Gmail, Drive, ...)". Off by default. It
    // takes effect only with the local login on.
    bool useClaudeAiConnectors() const;
    void setUseClaudeAiConnectors(bool on);

    bool keyStoreAvailable(wxString* why = nullptr) const;

    // The stored key, or empty when none is stored or the store is unavailable.
    wxString apiKey() const;

    // Stores `key` in the key store. An empty key erases the stored key.
    // Returns false and sets *error (when not null) when the store is
    // unavailable or the save fails. Nothing is written anywhere else.
    bool setApiKey(const wxString& key, wxString* error = nullptr);

    // The sidecar's environment for the current settings: buildSidecarEnv()
    // over `inherited`, plus ENABLE_CLAUDEAI_MCP_SERVERS. It is "true" only
    // when useLocalLogin() and useClaudeAiConnectors() are both on, else
    // "false". Reads the key store only when the local login is off.
    std::map<wxString, wxString> sidecarEnv(const std::map<wxString, wxString>& inherited) const;

private:
    wxConfigBase& m_config;
    KeyStore& m_keys;
};

// The sidecar's complete environment. Starts from `inherited` (the app's own
// environment) without ANTHROPIC_API_KEY, CLAUDE_CODE_OAUTH_TOKEN and
// ANTHROPIC_AUTH_TOKEN, so a stray shell variable cannot override the
// settings. Then:
//   - useLocalLogin false: ANTHROPIC_API_KEY = apiKey, unless apiKey is empty.
//   - useLocalLogin true: no key, so the CLI uses the user's own login.
//   - model not empty: READER_MODEL = model. Otherwise an inherited
//     READER_MODEL passes through.
// Pure: reads no process environment, config or key store.
std::map<wxString, wxString> buildSidecarEnv(const std::map<wxString, wxString>& inherited,
                                             bool useLocalLogin, const wxString& model,
                                             const wxString& apiKey);
