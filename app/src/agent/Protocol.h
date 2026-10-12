#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

// The sidecar protocol from sidecar/PROTOCOL.md, one struct per message.
// Members are the JSON fields in camelCase. The struct type names the message,
// so no struct stores "type".
namespace protocol
{

// What the user is looking at. The `context` field of `prompt`.
struct Context
{
    std::optional<std::string> file;
    std::optional<int> page;
    std::optional<int> pageCount;
    std::optional<std::string> selection;
};

// ---- Messages from the app (sidecar stdin) ----

struct OpenProject
{
    std::string path;
    std::optional<std::int64_t> seq;
};

struct Prompt
{
    std::string chatId;
    std::string text;
    std::optional<Context> context;
    std::optional<std::int64_t> seq;
};

struct NewChat
{
    std::string chatId;
    std::optional<std::int64_t> seq;
};

struct ResumeChat
{
    std::string chatId;
    std::string sessionId;
    std::optional<std::int64_t> seq;
};

struct Interrupt
{
    std::string chatId;
    std::optional<std::int64_t> seq;
};

struct PermissionReply
{
    std::string reqId;
    bool allow = false;
    std::optional<std::int64_t> seq;
};

// ---- Messages from the sidecar (sidecar stdout) ----

struct Ready
{
    std::string version;
};

struct ChatStarted
{
    std::string chatId;
    std::optional<std::string> sessionId;
};

struct TextDelta
{
    std::string chatId;
    std::string text;
};

struct ToolUse
{
    std::string chatId;
    std::string name;
    nlohmann::json input = nlohmann::json::object();
};

struct PermissionRequest
{
    std::string reqId;
    std::string chatId;
    std::string tool;
    nlohmann::json input = nlohmann::json::object();
};

struct Result
{
    std::string chatId;
    std::string sessionId;
    nlohmann::json usage = nlohmann::json::object();
    double costUsd = 0.0;
};

struct Error
{
    std::optional<std::string> chatId;
    std::string kind;
    std::string message;
    std::optional<std::int64_t> seq;
};

using Inbound = std::variant<OpenProject, Prompt, NewChat, ResumeChat, Interrupt, PermissionReply>;
using Outbound =
    std::variant<Ready, ChatStarted, TextDelta, ToolUse, PermissionRequest, Result, Error>;

// The "type" string of a message, such as "open_project".
const char* typeName(const Inbound& msg);
const char* typeName(const Outbound& msg);

// The JSON object for a message, including "type". App messages omit unset
// optional fields. Sidecar messages write every field, unset ones as null.
nlohmann::json toJson(const Inbound& msg);
nlohmann::json toJson(const Outbound& msg);

// Parses one message of the given direction. Never throws. On success sets
// `out` and returns true. Otherwise returns false and sets `error`: the value
// is not an object, has no string "type", names a message of the other
// direction or none, lacks a required field, or has a field of the wrong JSON
// type. A missing or null optional field becomes std::nullopt. Unknown keys
// are ignored.
bool fromJson(const nlohmann::json& value, Inbound& out, std::string& error);
bool fromJson(const nlohmann::json& value, Outbound& out, std::string& error);

} // namespace protocol
