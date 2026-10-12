#include "agent/Protocol.h"

#include <limits>
#include <type_traits>
#include <utility>

namespace protocol
{
namespace
{
using nlohmann::json;

// The "type" string of each message struct.
template <typename T>
struct Type;
template <> struct Type<OpenProject> { static constexpr const char* name = "open_project"; };
template <> struct Type<Prompt> { static constexpr const char* name = "prompt"; };
template <> struct Type<NewChat> { static constexpr const char* name = "new_chat"; };
template <> struct Type<ResumeChat> { static constexpr const char* name = "resume_chat"; };
template <> struct Type<Interrupt> { static constexpr const char* name = "interrupt"; };
template <> struct Type<PermissionReply> { static constexpr const char* name = "permission_reply"; };
template <> struct Type<Ready> { static constexpr const char* name = "ready"; };
template <> struct Type<ChatStarted> { static constexpr const char* name = "chat_started"; };
template <> struct Type<TextDelta> { static constexpr const char* name = "text_delta"; };
template <> struct Type<ToolUse> { static constexpr const char* name = "tool_use"; };
template <> struct Type<PermissionRequest> { static constexpr const char* name = "permission_request"; };
template <> struct Type<Result> { static constexpr const char* name = "result"; };
template <> struct Type<Error> { static constexpr const char* name = "error"; };

// ---- Field readers. Each returns false and sets `error` on a bad field. ----

const json* field(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it == obj.end() ? nullptr : &*it;
}

bool missing(const char* key, std::string& error)
{
    error = std::string("missing field \"") + key + "\"";
    return false;
}

bool wrongType(const char* key, const char* expected, std::string& error)
{
    error = std::string("field \"") + key + "\" is not " + expected;
    return false;
}

// Sets `out` when `value` is a JSON integer that fits in Int.
template <typename Int>
bool integerIn(const json& value, Int& out)
{
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(std::numeric_limits<Int>::max()))
            return false;
        out = static_cast<Int>(v);
        return true;
    }
    if (value.is_number_integer()) {
        const auto v = value.get<std::int64_t>();
        if (v < static_cast<std::int64_t>(std::numeric_limits<Int>::min())
            || v > static_cast<std::int64_t>(std::numeric_limits<Int>::max()))
            return false;
        out = static_cast<Int>(v);
        return true;
    }
    return false;
}

bool readString(const json& obj, const char* key, std::string& out, std::string& error)
{
    const json* v = field(obj, key);
    if (!v)
        return missing(key, error);
    if (!v->is_string())
        return wrongType(key, "a string", error);
    out = v->get_ref<const std::string&>();
    return true;
}

bool readOptString(const json& obj, const char* key, std::optional<std::string>& out,
                   std::string& error)
{
    const json* v = field(obj, key);
    if (!v || v->is_null()) {
        out.reset();
        return true;
    }
    if (!v->is_string())
        return wrongType(key, "a string or null", error);
    out = v->get_ref<const std::string&>();
    return true;
}

template <typename Int>
bool readOptInt(const json& obj, const char* key, std::optional<Int>& out, std::string& error)
{
    const json* v = field(obj, key);
    if (!v || v->is_null()) {
        out.reset();
        return true;
    }
    Int value{};
    if (!integerIn(*v, value))
        return wrongType(key, "an integer in range or null", error);
    out = value;
    return true;
}

bool readSeq(const json& obj, std::optional<std::int64_t>& out, std::string& error)
{
    return readOptInt(obj, "seq", out, error);
}

bool readBool(const json& obj, const char* key, bool& out, std::string& error)
{
    const json* v = field(obj, key);
    if (!v)
        return missing(key, error);
    if (!v->is_boolean())
        return wrongType(key, "a boolean", error);
    out = v->get<bool>();
    return true;
}

bool readObject(const json& obj, const char* key, json& out, std::string& error)
{
    const json* v = field(obj, key);
    if (!v)
        return missing(key, error);
    if (!v->is_object())
        return wrongType(key, "an object", error);
    out = *v;
    return true;
}

bool readNumber(const json& obj, const char* key, double& out, std::string& error)
{
    const json* v = field(obj, key);
    if (!v)
        return missing(key, error);
    if (!v->is_number())
        return wrongType(key, "a number", error);
    out = v->get<double>();
    return true;
}

bool readOptContext(const json& obj, std::optional<Context>& out, std::string& error)
{
    const json* v = field(obj, "context");
    if (!v || v->is_null()) {
        out.reset();
        return true;
    }
    if (!v->is_object())
        return wrongType("context", "an object or null", error);
    Context ctx;
    if (!readOptString(*v, "file", ctx.file, error) || !readOptInt(*v, "page", ctx.page, error)
        || !readOptInt(*v, "page_count", ctx.pageCount, error)
        || !readOptString(*v, "selection", ctx.selection, error))
        return false;
    out = std::move(ctx);
    return true;
}

// ---- Message readers ----

bool read(const json& j, OpenProject& m, std::string& e)
{
    return readString(j, "path", m.path, e) && readSeq(j, m.seq, e);
}

bool read(const json& j, Prompt& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readString(j, "text", m.text, e)
        && readOptContext(j, m.context, e) && readSeq(j, m.seq, e);
}

bool read(const json& j, NewChat& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readSeq(j, m.seq, e);
}

bool read(const json& j, ResumeChat& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readString(j, "session_id", m.sessionId, e)
        && readSeq(j, m.seq, e);
}

bool read(const json& j, Interrupt& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readSeq(j, m.seq, e);
}

bool read(const json& j, PermissionReply& m, std::string& e)
{
    return readString(j, "req_id", m.reqId, e) && readBool(j, "allow", m.allow, e)
        && readSeq(j, m.seq, e);
}

bool read(const json& j, Ready& m, std::string& e)
{
    return readString(j, "version", m.version, e);
}

bool read(const json& j, ChatStarted& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readOptString(j, "session_id", m.sessionId, e);
}

bool read(const json& j, TextDelta& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readString(j, "text", m.text, e);
}

bool read(const json& j, ToolUse& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readString(j, "name", m.name, e)
        && readObject(j, "input", m.input, e);
}

bool read(const json& j, PermissionRequest& m, std::string& e)
{
    return readString(j, "req_id", m.reqId, e) && readString(j, "chat_id", m.chatId, e)
        && readString(j, "tool", m.tool, e) && readObject(j, "input", m.input, e);
}

bool read(const json& j, Result& m, std::string& e)
{
    return readString(j, "chat_id", m.chatId, e) && readString(j, "session_id", m.sessionId, e)
        && readObject(j, "usage", m.usage, e) && readNumber(j, "cost_usd", m.costUsd, e);
}

bool read(const json& j, Error& m, std::string& e)
{
    return readOptString(j, "chat_id", m.chatId, e) && readString(j, "kind", m.kind, e)
        && readString(j, "message", m.message, e) && readSeq(j, m.seq, e);
}

// ---- Message writers. App messages omit unset fields, sidecar messages write null. ----

template <typename T>
void putIfSet(json& j, const char* key, const std::optional<T>& value)
{
    if (value)
        j[key] = *value;
}

template <typename T>
void putOrNull(json& j, const char* key, const std::optional<T>& value)
{
    j[key] = value ? json(*value) : json(nullptr);
}

void write(json& j, const OpenProject& m)
{
    j["path"] = m.path;
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const Prompt& m)
{
    j["chat_id"] = m.chatId;
    j["text"] = m.text;
    if (m.context) {
        json ctx = json::object();
        putIfSet(ctx, "file", m.context->file);
        putIfSet(ctx, "page", m.context->page);
        putIfSet(ctx, "page_count", m.context->pageCount);
        putIfSet(ctx, "selection", m.context->selection);
        j["context"] = std::move(ctx);
    }
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const NewChat& m)
{
    j["chat_id"] = m.chatId;
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const ResumeChat& m)
{
    j["chat_id"] = m.chatId;
    j["session_id"] = m.sessionId;
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const Interrupt& m)
{
    j["chat_id"] = m.chatId;
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const PermissionReply& m)
{
    j["req_id"] = m.reqId;
    j["allow"] = m.allow;
    putIfSet(j, "seq", m.seq);
}

void write(json& j, const Ready& m)
{
    j["version"] = m.version;
}

void write(json& j, const ChatStarted& m)
{
    j["chat_id"] = m.chatId;
    putOrNull(j, "session_id", m.sessionId);
}

void write(json& j, const TextDelta& m)
{
    j["chat_id"] = m.chatId;
    j["text"] = m.text;
}

void write(json& j, const ToolUse& m)
{
    j["chat_id"] = m.chatId;
    j["name"] = m.name;
    j["input"] = m.input;
}

void write(json& j, const PermissionRequest& m)
{
    j["req_id"] = m.reqId;
    j["chat_id"] = m.chatId;
    j["tool"] = m.tool;
    j["input"] = m.input;
}

void write(json& j, const Result& m)
{
    j["chat_id"] = m.chatId;
    j["session_id"] = m.sessionId;
    j["usage"] = m.usage;
    j["cost_usd"] = m.costUsd;
}

void write(json& j, const Error& m)
{
    putOrNull(j, "chat_id", m.chatId);
    j["kind"] = m.kind;
    j["message"] = m.message;
    putOrNull(j, "seq", m.seq);
}

// ---- Variant dispatch ----

template <typename Variant>
const char* variantTypeName(const Variant& msg)
{
    return std::visit([](const auto& m) { return Type<std::decay_t<decltype(m)>>::name; }, msg);
}

template <typename Variant>
json variantToJson(const Variant& msg)
{
    return std::visit(
        [](const auto& m) {
            json j = json::object();
            j["type"] = Type<std::decay_t<decltype(m)>>::name;
            write(j, m);
            return j;
        },
        msg);
}

// Parses `value` as T when `type` names T. Sets `matched` when it does.
template <typename T, typename Variant>
bool parseAs(const json& value, const std::string& type, Variant& out, std::string& error,
             bool& matched)
{
    if (matched || type != Type<T>::name)
        return false;
    matched = true;
    T msg;
    if (!read(value, msg, error))
        return false;
    out = std::move(msg);
    return true;
}

template <typename... Ts>
bool variantFromJson(const json& value, std::variant<Ts...>& out, std::string& error,
                     const char* direction)
{
    if (!value.is_object()) {
        error = "message is not a JSON object";
        return false;
    }
    const json* type = field(value, "type");
    if (!type || !type->is_string()) {
        error = "message has no string \"type\"";
        return false;
    }
    const auto& name = type->get_ref<const std::string&>();
    bool matched = false;
    const bool ok = (parseAs<Ts>(value, name, out, error, matched) || ...);
    if (!matched)
        error = "\"" + name + "\" is not " + direction + " message type";
    return ok;
}

} // namespace

const char* typeName(const Inbound& msg)
{
    return variantTypeName(msg);
}

const char* typeName(const Outbound& msg)
{
    return variantTypeName(msg);
}

nlohmann::json toJson(const Inbound& msg)
{
    return variantToJson(msg);
}

nlohmann::json toJson(const Outbound& msg)
{
    return variantToJson(msg);
}

bool fromJson(const nlohmann::json& value, Inbound& out, std::string& error)
{
    return variantFromJson(value, out, error, "an app");
}

bool fromJson(const nlohmann::json& value, Outbound& out, std::string& error)
{
    return variantFromJson(value, out, error, "a sidecar");
}

} // namespace protocol
