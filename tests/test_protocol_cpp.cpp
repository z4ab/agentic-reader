// M5.1: spec tests for the C++ protocol structs (app/src/agent/Protocol.h).
//
// Every message in sidecar/PROTOCOL.md is parsed from its example line,
// checked field by field, written back with toJson, and compared with the
// parsed example (nlohmann::json equality ignores key order). A second test
// reads PROTOCOL.md itself, so a message added to the document without a C++
// struct fails here. PROTOCOL_MD_PATH is set by tests/CMakeLists.txt.
//
// Interface choices where the spec is silent:
//   - Everything lives in namespace `protocol`, declared in agent/Protocol.h.
//   - One plain struct per message, named after the message in PascalCase:
//       app to sidecar:  OpenProject, Prompt, NewChat, ResumeChat, Interrupt,
//                        PermissionReply
//       sidecar to app:  Ready, ChatStarted, TextDelta, ToolUse,
//                        PermissionRequest, Result, Error
//     plus Context for the `context` object of `prompt`. Structs are default
//     constructible and carry no `type` member. The struct type is the type.
//   - Members are the JSON fields in camelCase: path, chatId, text, context,
//     sessionId, reqId, allow, version, name, input, tool, usage, costUsd,
//     kind, message, seq. Context has file, page, pageCount, selection.
//   - Member types:
//       string fields                    std::string
//       "string or null"                 std::optional<std::string>
//       seq                              std::optional<std::int64_t>
//       Context::page, pageCount         std::optional<int>
//       Prompt::context                  std::optional<Context>
//       allow                            bool
//       costUsd                          double
//       input, usage                     nlohmann::json (an object)
//   - using Inbound = std::variant<OpenProject, Prompt, NewChat, ResumeChat,
//                                  Interrupt, PermissionReply>;
//     using Outbound = std::variant<Ready, ChatStarted, TextDelta, ToolUse,
//                                   PermissionRequest, Result, Error>;
//   - nlohmann::json toJson(const Inbound&) and
//     nlohmann::json toJson(const Outbound&) write the object including
//     "type".
//       Inbound: a std::nullopt field is omitted (seq, context, and each
//       Context field). PROTOCOL.md allows omitting optional app fields.
//       Outbound: every field is written, std::nullopt as null, because
//       PROTOCOL.md says every listed sidecar field is always present.
//   - bool fromJson(const nlohmann::json&, Inbound& out, std::string& error)
//     and bool fromJson(const nlohmann::json&, Outbound& out,
//     std::string& error) never throw. On success they set `out` and return
//     true. They return false with a non-empty `error` when the value is not
//     an object, has no string "type", has a "type" that is not a message of
//     that direction, lacks a required field, or has a field of the wrong
//     JSON type. A missing or null optional field becomes std::nullopt.
//     Unknown keys are ignored.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "agent/Protocol.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using nlohmann::json;

namespace {

const char* const kChatId = "6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10";
const char* const kSessionId = "b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b";
const char* const kReqId = "9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4";

// The example lines from sidecar/PROTOCOL.md, copied verbatim.
const char* const kOpenProjectLine =
    R"({"type":"open_project","path":"/home/u/MyProject","seq":1})";
const char* const kPromptLine =
    R"({"type":"prompt","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","text":"Explain this paragraph.","context":{"file":"papers/attention.pdf","page":12,"page_count":15,"selection":"Scaled dot-product attention"},"seq":4})";
const char* const kNewChatLine =
    R"({"type":"new_chat","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","seq":2})";
const char* const kResumeChatLine =
    R"({"type":"resume_chat","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":"b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b","seq":3})";
const char* const kInterruptLine =
    R"({"type":"interrupt","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","seq":5})";
const char* const kPermissionReplyLine =
    R"({"type":"permission_reply","req_id":"9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4","allow":true,"seq":6})";

const char* const kReadyLine = R"({"type":"ready","version":"0.1.0"})";
const char* const kChatStartedLine =
    R"({"type":"chat_started","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":null})";
const char* const kTextDeltaLine =
    R"({"type":"text_delta","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","text":"Scaled dot-product attention divides by \u221ad_k (p. 4).\n"})";
const char* const kToolUseLine =
    R"({"type":"tool_use","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","name":"Read","input":{"file_path":"papers/attention.pdf","pages":"12"}})";
const char* const kPermissionRequestLine =
    R"({"type":"permission_request","req_id":"9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","tool":"Bash","input":{"command":"ls"}})";
const char* const kResultLine =
    R"({"type":"result","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":"b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b","usage":{"input_tokens":1200,"output_tokens":340},"cost_usd":0.0123})";
const char* const kErrorLine =
    R"({"type":"error","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","kind":"busy","message":"chat is busy","seq":4})";

const std::set<std::string> kInboundTypes = {
    "open_project", "prompt", "new_chat", "resume_chat", "interrupt", "permission_reply",
};
const std::set<std::string> kOutboundTypes = {
    "ready", "chat_started", "text_delta", "tool_use", "permission_request", "result", "error",
};

// Parses `line` as an app message. Fails the test case when fromJson fails.
protocol::Inbound parseInbound(const std::string& line)
{
    protocol::Inbound out;
    std::string error;
    const bool ok = protocol::fromJson(json::parse(line), out, error);
    INFO("line: ", line, " error: ", error);
    REQUIRE(ok);
    return out;
}

// Parses `line` as a sidecar message. Fails the test case when fromJson fails.
protocol::Outbound parseOutbound(const std::string& line)
{
    protocol::Outbound out;
    std::string error;
    const bool ok = protocol::fromJson(json::parse(line), out, error);
    INFO("line: ", line, " error: ", error);
    REQUIRE(ok);
    return out;
}

void checkInboundRoundTrip(const std::string& line)
{
    INFO("line: ", line);
    CHECK(protocol::toJson(parseInbound(line)) == json::parse(line));
}

void checkOutboundRoundTrip(const std::string& line)
{
    INFO("line: ", line);
    CHECK(protocol::toJson(parseOutbound(line)) == json::parse(line));
}

bool inboundFails(const json& value)
{
    protocol::Inbound out;
    std::string error;
    bool ok = true;
    CHECK_NOTHROW(ok = protocol::fromJson(value, out, error));
    INFO("value: ", value.dump(), " error: ", error);
    return !ok && !error.empty();
}

bool outboundFails(const json& value)
{
    protocol::Outbound out;
    std::string error;
    bool ok = true;
    CHECK_NOTHROW(ok = protocol::fromJson(value, out, error));
    INFO("value: ", value.dump(), " error: ", error);
    return !ok && !error.empty();
}

// The bodies of every ```json fenced block in PROTOCOL.md.
std::vector<std::string> protocolExampleLines()
{
    std::ifstream in(PROTOCOL_MD_PATH);
    REQUIRE(in.good());
    std::vector<std::string> lines;
    std::string line;
    bool inBlock = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!inBlock && line == "```json") {
            inBlock = true;
        } else if (inBlock && line == "```") {
            inBlock = false;
        } else if (inBlock && !line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

} // namespace

TEST_CASE("open_project example round-trips")
{
    const auto msg = parseInbound(kOpenProjectLine);
    REQUIRE(std::holds_alternative<protocol::OpenProject>(msg));
    const auto& m = std::get<protocol::OpenProject>(msg);
    CHECK(m.path == "/home/u/MyProject");
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 1);
    checkInboundRoundTrip(kOpenProjectLine);
}

TEST_CASE("prompt example round-trips with its context object")
{
    const auto msg = parseInbound(kPromptLine);
    REQUIRE(std::holds_alternative<protocol::Prompt>(msg));
    const auto& m = std::get<protocol::Prompt>(msg);
    CHECK(m.chatId == kChatId);
    CHECK(m.text == "Explain this paragraph.");
    REQUIRE(m.context.has_value());
    CHECK(m.context->file == std::optional<std::string>("papers/attention.pdf"));
    CHECK(m.context->page == std::optional<int>(12));
    CHECK(m.context->pageCount == std::optional<int>(15));
    CHECK(m.context->selection == std::optional<std::string>("Scaled dot-product attention"));
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 4);
    checkInboundRoundTrip(kPromptLine);
}

TEST_CASE("new_chat example round-trips")
{
    const auto msg = parseInbound(kNewChatLine);
    REQUIRE(std::holds_alternative<protocol::NewChat>(msg));
    const auto& m = std::get<protocol::NewChat>(msg);
    CHECK(m.chatId == kChatId);
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 2);
    checkInboundRoundTrip(kNewChatLine);
}

TEST_CASE("resume_chat example round-trips")
{
    const auto msg = parseInbound(kResumeChatLine);
    REQUIRE(std::holds_alternative<protocol::ResumeChat>(msg));
    const auto& m = std::get<protocol::ResumeChat>(msg);
    CHECK(m.chatId == kChatId);
    CHECK(m.sessionId == kSessionId);
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 3);
    checkInboundRoundTrip(kResumeChatLine);
}

TEST_CASE("interrupt example round-trips")
{
    const auto msg = parseInbound(kInterruptLine);
    REQUIRE(std::holds_alternative<protocol::Interrupt>(msg));
    const auto& m = std::get<protocol::Interrupt>(msg);
    CHECK(m.chatId == kChatId);
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 5);
    checkInboundRoundTrip(kInterruptLine);
}

TEST_CASE("permission_reply example round-trips")
{
    const auto msg = parseInbound(kPermissionReplyLine);
    REQUIRE(std::holds_alternative<protocol::PermissionReply>(msg));
    const auto& m = std::get<protocol::PermissionReply>(msg);
    CHECK(m.reqId == kReqId);
    CHECK(m.allow == true);
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 6);
    checkInboundRoundTrip(kPermissionReplyLine);

    const std::string deny =
        R"({"type":"permission_reply","req_id":"9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4","allow":false})";
    const auto denied = parseInbound(deny);
    REQUIRE(std::holds_alternative<protocol::PermissionReply>(denied));
    CHECK(std::get<protocol::PermissionReply>(denied).allow == false);
    checkInboundRoundTrip(deny);
}

TEST_CASE("ready example round-trips")
{
    const auto msg = parseOutbound(kReadyLine);
    REQUIRE(std::holds_alternative<protocol::Ready>(msg));
    CHECK(std::get<protocol::Ready>(msg).version == "0.1.0");
    checkOutboundRoundTrip(kReadyLine);
}

TEST_CASE("chat_started example round-trips with a null session_id")
{
    const auto msg = parseOutbound(kChatStartedLine);
    REQUIRE(std::holds_alternative<protocol::ChatStarted>(msg));
    const auto& m = std::get<protocol::ChatStarted>(msg);
    CHECK(m.chatId == kChatId);
    CHECK_FALSE(m.sessionId.has_value());
    checkOutboundRoundTrip(kChatStartedLine);

    const std::string resumed =
        R"({"type":"chat_started","chat_id":"6f1c2a52-3b8e-4d1a-9a57-0c2b9a1e4f10","session_id":"b2d4e6f8-1a3c-4e5f-8a9b-0c1d2e3f4a5b"})";
    const auto r = parseOutbound(resumed);
    REQUIRE(std::holds_alternative<protocol::ChatStarted>(r));
    CHECK(std::get<protocol::ChatStarted>(r).sessionId == std::optional<std::string>(kSessionId));
    checkOutboundRoundTrip(resumed);
}

TEST_CASE("text_delta example round-trips and decodes escapes to UTF-8")
{
    const auto msg = parseOutbound(kTextDeltaLine);
    REQUIRE(std::holds_alternative<protocol::TextDelta>(msg));
    const auto& m = std::get<protocol::TextDelta>(msg);
    CHECK(m.chatId == kChatId);
    // U+221A SQUARE ROOT is E2 88 9A in UTF-8. The trailing \n is a real newline.
    CHECK(m.text == "Scaled dot-product attention divides by \xE2\x88\x9A" "d_k (p. 4).\n");
    checkOutboundRoundTrip(kTextDeltaLine);
}

TEST_CASE("tool_use example round-trips and keeps input as an object")
{
    const auto msg = parseOutbound(kToolUseLine);
    REQUIRE(std::holds_alternative<protocol::ToolUse>(msg));
    const auto& m = std::get<protocol::ToolUse>(msg);
    CHECK(m.chatId == kChatId);
    CHECK(m.name == "Read");
    CHECK(m.input == json::parse(R"({"file_path":"papers/attention.pdf","pages":"12"})"));
    checkOutboundRoundTrip(kToolUseLine);
}

TEST_CASE("permission_request example round-trips")
{
    const auto msg = parseOutbound(kPermissionRequestLine);
    REQUIRE(std::holds_alternative<protocol::PermissionRequest>(msg));
    const auto& m = std::get<protocol::PermissionRequest>(msg);
    CHECK(m.reqId == kReqId);
    CHECK(m.chatId == kChatId);
    CHECK(m.tool == "Bash");
    CHECK(m.input == json::parse(R"({"command":"ls"})"));
    checkOutboundRoundTrip(kPermissionRequestLine);
}

TEST_CASE("result example round-trips")
{
    const auto msg = parseOutbound(kResultLine);
    REQUIRE(std::holds_alternative<protocol::Result>(msg));
    const auto& m = std::get<protocol::Result>(msg);
    CHECK(m.chatId == kChatId);
    CHECK(m.sessionId == kSessionId);
    CHECK(m.usage == json::parse(R"({"input_tokens":1200,"output_tokens":340})"));
    CHECK(m.costUsd == doctest::Approx(0.0123));
    checkOutboundRoundTrip(kResultLine);
}

TEST_CASE("error example round-trips")
{
    const auto msg = parseOutbound(kErrorLine);
    REQUIRE(std::holds_alternative<protocol::Error>(msg));
    const auto& m = std::get<protocol::Error>(msg);
    CHECK(m.chatId == std::optional<std::string>(kChatId));
    CHECK(m.kind == "busy");
    CHECK(m.message == "chat is busy");
    REQUIRE(m.seq.has_value());
    CHECK(*m.seq == 4);
    checkOutboundRoundTrip(kErrorLine);
}

TEST_CASE("error with null chat_id and seq round-trips and keeps an unknown kind")
{
    const std::string line =
        R"({"type":"error","chat_id":null,"kind":"something_new","message":"line is not JSON","seq":null})";
    const auto msg = parseOutbound(line);
    REQUIRE(std::holds_alternative<protocol::Error>(msg));
    const auto& m = std::get<protocol::Error>(msg);
    CHECK_FALSE(m.chatId.has_value());
    CHECK(m.kind == "something_new");
    CHECK(m.message == "line is not JSON");
    CHECK_FALSE(m.seq.has_value());
    // Outbound toJson writes every field, null for std::nullopt.
    checkOutboundRoundTrip(line);
}

TEST_CASE("every example line in PROTOCOL.md round-trips and every message is covered")
{
    const auto lines = protocolExampleLines();
    REQUIRE_FALSE(lines.empty());

    std::set<std::string> seenIn;
    std::set<std::string> seenOut;
    for (const auto& line : lines) {
        INFO("line: ", line);
        const json parsed = json::parse(line);
        REQUIRE(parsed.is_object());
        const std::string type = parsed.at("type").get<std::string>();
        if (kInboundTypes.count(type)) {
            seenIn.insert(type);
            checkInboundRoundTrip(line);
        } else {
            REQUIRE(kOutboundTypes.count(type) == 1);
            seenOut.insert(type);
            checkOutboundRoundTrip(line);
        }
    }
    CHECK(seenIn == kInboundTypes);
    CHECK(seenOut == kOutboundTypes);
}

TEST_CASE("the example lines in this file match PROTOCOL.md")
{
    const auto lines = protocolExampleLines();
    const std::set<std::string> doc(lines.begin(), lines.end());
    for (const char* literal : {kOpenProjectLine, kPromptLine, kNewChatLine, kResumeChatLine,
                                kInterruptLine, kPermissionReplyLine, kReadyLine, kChatStartedLine,
                                kTextDeltaLine, kToolUseLine, kPermissionRequestLine, kResultLine,
                                kErrorLine}) {
        const std::string line = literal;
        INFO("line: ", line);
        CHECK(doc.count(line) == 1);
    }
}

TEST_CASE("inbound toJson omits optional fields that are not set")
{
    protocol::OpenProject open;
    open.path = "/p";
    const json openJson = protocol::toJson(protocol::Inbound{open});
    CHECK(openJson == json::parse(R"({"type":"open_project","path":"/p"})"));

    protocol::Prompt bare;
    bare.chatId = kChatId;
    bare.text = "hi";
    const json bareJson = protocol::toJson(protocol::Inbound{bare});
    CHECK(bareJson.at("type") == "prompt");
    CHECK_FALSE(bareJson.contains("context"));
    CHECK_FALSE(bareJson.contains("seq"));

    protocol::Prompt withPage;
    withPage.chatId = kChatId;
    withPage.text = "hi";
    withPage.context = protocol::Context{};
    withPage.context->page = 3;
    withPage.seq = 9;
    const json pageJson = protocol::toJson(protocol::Inbound{withPage});
    CHECK(pageJson.at("context") == json::parse(R"({"page":3})"));
    CHECK(pageJson.at("seq") == 9);
}

TEST_CASE("inbound fromJson treats missing and null optional fields as unset")
{
    const auto absent = parseInbound(R"({"type":"open_project","path":"/p"})");
    REQUIRE(std::holds_alternative<protocol::OpenProject>(absent));
    CHECK_FALSE(std::get<protocol::OpenProject>(absent).seq.has_value());

    const auto nulls = parseInbound(
        R"({"type":"prompt","chat_id":"c","text":"t","context":null,"seq":null})");
    REQUIRE(std::holds_alternative<protocol::Prompt>(nulls));
    CHECK_FALSE(std::get<protocol::Prompt>(nulls).context.has_value());
    CHECK_FALSE(std::get<protocol::Prompt>(nulls).seq.has_value());

    const auto partial = parseInbound(
        R"({"type":"prompt","chat_id":"c","text":"t","context":{"file":null,"page":2}})");
    REQUIRE(std::holds_alternative<protocol::Prompt>(partial));
    const auto& ctx = std::get<protocol::Prompt>(partial).context;
    REQUIRE(ctx.has_value());
    CHECK_FALSE(ctx->file.has_value());
    CHECK(ctx->page == std::optional<int>(2));
    CHECK_FALSE(ctx->pageCount.has_value());
    CHECK_FALSE(ctx->selection.has_value());
}

TEST_CASE("outbound fromJson ignores unknown keys")
{
    const auto msg = parseOutbound(R"({"type":"ready","version":"0.2.0","build":"abc"})");
    REQUIRE(std::holds_alternative<protocol::Ready>(msg));
    CHECK(std::get<protocol::Ready>(msg).version == "0.2.0");
}

TEST_CASE("outbound fromJson rejects invalid messages without throwing")
{
    CHECK(outboundFails(json::parse("[1,2]")));
    CHECK(outboundFails(json::parse(R"("ready")")));
    CHECK(outboundFails(json::parse(R"({"version":"0.1.0"})")));
    CHECK(outboundFails(json::parse(R"({"type":7,"version":"0.1.0"})")));
    CHECK(outboundFails(json::parse(R"({"type":"no_such_message"})")));
    // An app message is not a sidecar message.
    CHECK(outboundFails(json::parse(kOpenProjectLine)));
    // Missing required fields.
    CHECK(outboundFails(json::parse(R"({"type":"ready"})")));
    CHECK(outboundFails(json::parse(R"({"type":"text_delta","chat_id":"c"})")));
    CHECK(outboundFails(
        json::parse(R"({"type":"result","chat_id":"c","session_id":"s","usage":{}})")));
    // Wrong JSON types.
    CHECK(outboundFails(json::parse(R"({"type":"ready","version":5})")));
    CHECK(outboundFails(json::parse(
        R"({"type":"result","chat_id":"c","session_id":"s","usage":{},"cost_usd":"0.1"})")));
}

TEST_CASE("inbound fromJson rejects invalid messages without throwing")
{
    CHECK(inboundFails(json::parse("null")));
    CHECK(inboundFails(json::parse(R"({"path":"/p"})")));
    // A sidecar message is not an app message.
    CHECK(inboundFails(json::parse(kReadyLine)));
    CHECK(inboundFails(json::parse(R"({"type":"open_project"})")));
    CHECK(inboundFails(json::parse(R"({"type":"prompt","chat_id":"c","text":"t","context":"x"})")));
}
