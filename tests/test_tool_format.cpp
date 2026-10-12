// M5.4: text for tool activity in the chat and for the permission dialog.
//
// The spec names no function for formatting tool calls. Assumed interface,
// app/src/chat/ToolFormat.h (no wx types, plain UTF-8, never throws):
//
//   #include <nlohmann/json.hpp>
//   #include <string>
//   #include <string_view>
//
//   namespace toolformat
//   {
//   // The single grey line a `tool_use` shows in the message view (spec 1),
//   // such as "Read papers/attention.pdf pages 10-12". Formats Read, Glob,
//   // Grep, Write, Edit, Bash, WebFetch and WebSearch. Any other tool, or a
//   // known tool whose input lacks the fields its format needs, falls back to
//   // `name`, a space, and input.dump() (compact JSON). Plain text, not HTML:
//   // the caller escapes it. Never contains a line break.
//   std::string line(std::string_view name, const nlohmann::json& input);
//
//   // The "formatted input" the PermissionDialog shows under the tool name
//   // (spec 2). Plain text, may span lines. Shows every value the user is
//   // approving in full, never truncated: Bash shows the whole command, Write
//   // the path and the whole content, Edit the path and both strings. Other
//   // tools show the input as indented JSON (input.dump(2) or similar).
//   std::string details(std::string_view name, const nlohmann::json& input);
//   }
//
// Checks use substrings where the spec leaves the wording open. The exact
// checks are the spec's own example and the compact-JSON fallback.
//
// The dialog itself (modal, Deny default, Esc denies, closes on sidecar
// restart or when the reply ends) is checked by tests/acceptance/m5.4.steps,
// since a GUI test needs a display that ctest does not have.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "chat/ToolFormat.h"

#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool hasLineBreak(const std::string& text)
{
    return text.find('\n') != std::string::npos || text.find('\r') != std::string::npos;
}

} // namespace

TEST_CASE("Read with pages renders as the spec's example")
{
    const json input = {{"file_path", "papers/attention.pdf"}, {"pages", "10-12"}};
    CHECK(toolformat::line("Read", input) == "Read papers/attention.pdf pages 10-12");
}

TEST_CASE("Read without pages names the tool and the file")
{
    const std::string text = toolformat::line("Read", json{{"file_path", "notes/summary.md"}});
    CHECK(startsWith(text, "Read notes/summary.md"));
    CHECK_FALSE(contains(text, "{"));
    CHECK_FALSE(hasLineBreak(text));
}

TEST_CASE("Glob shows the pattern")
{
    const std::string text = toolformat::line("Glob", json{{"pattern", "**/*.pdf"}});
    CHECK(startsWith(text, "Glob "));
    CHECK(contains(text, "**/*.pdf"));
    CHECK_FALSE(contains(text, "{"));
}

TEST_CASE("Grep shows the pattern and the path")
{
    const std::string text =
        toolformat::line("Grep", json{{"pattern", "softmax"}, {"path", "papers"}});
    CHECK(startsWith(text, "Grep "));
    CHECK(contains(text, "softmax"));
    CHECK(contains(text, "papers"));
    CHECK_FALSE(contains(text, "{"));
}

TEST_CASE("Write shows the path but not the content")
{
    const json input = {{"file_path", "notes.md"},
                        {"content", "# Summary\n\nA long body that belongs in the file."}};
    const std::string text = toolformat::line("Write", input);
    CHECK(startsWith(text, "Write notes.md"));
    CHECK_FALSE(contains(text, "Summary"));
    CHECK_FALSE(contains(text, "long body"));
    CHECK_FALSE(hasLineBreak(text));
}

TEST_CASE("Edit shows the path but not the strings")
{
    const json input = {{"file_path", "notes.md"},
                        {"old_string", "first draft"},
                        {"new_string", "second draft"}};
    const std::string text = toolformat::line("Edit", input);
    CHECK(startsWith(text, "Edit notes.md"));
    CHECK_FALSE(contains(text, "first draft"));
    CHECK_FALSE(contains(text, "second draft"));
}

TEST_CASE("Bash shows the command as plain text")
{
    const json input = {{"command", "grep -c \"<b>\" notes.md && wc -l notes.md"},
                        {"description", "Count bold tags"}};
    const std::string text = toolformat::line("Bash", input);
    CHECK(startsWith(text, "Bash "));
    // Not HTML-escaped: ChatPanel escapes the line when it renders it.
    CHECK(contains(text, "grep -c \"<b>\" notes.md && wc -l notes.md"));
    CHECK_FALSE(contains(text, "&lt;"));
    CHECK_FALSE(contains(text, "&amp;"));
}

TEST_CASE("Bash with a multi-line command still gives a single line")
{
    const json input = {{"command", "cd papers\nls -la\r\npwd"}};
    const std::string text = toolformat::line("Bash", input);
    CHECK(startsWith(text, "Bash "));
    CHECK(contains(text, "cd papers"));
    CHECK_FALSE(hasLineBreak(text));
}

TEST_CASE("WebFetch shows the URL")
{
    const json input = {{"url", "https://arxiv.org/abs/1706.03762"},
                        {"prompt", "Summarize the abstract"}};
    const std::string text = toolformat::line("WebFetch", input);
    CHECK(startsWith(text, "WebFetch "));
    CHECK(contains(text, "https://arxiv.org/abs/1706.03762"));
    CHECK_FALSE(hasLineBreak(text));
}

TEST_CASE("WebSearch shows the query")
{
    const std::string text =
        toolformat::line("WebSearch", json{{"query", "transformer positional encoding"}});
    CHECK(startsWith(text, "WebSearch "));
    CHECK(contains(text, "transformer positional encoding"));
}

TEST_CASE("an unknown tool falls back to its name and compact JSON")
{
    const json input = json::parse(R"({"title":"Draft","tags":["a","b"]})");
    CHECK(toolformat::line("mcp__notes__save", input)
          == std::string("mcp__notes__save ") + input.dump());
    CHECK(toolformat::line("mcp__notes__save", input)
          == R"(mcp__notes__save {"tags":["a","b"],"title":"Draft"})");
}

TEST_CASE("the fallback stays on one line when a value holds a line break")
{
    const json input = {{"text", "line one\nline two"}};
    const std::string text = toolformat::line("TodoWrite", input);
    CHECK(startsWith(text, "TodoWrite "));
    CHECK(contains(text, "line one"));
    CHECK_FALSE(hasLineBreak(text));
}

TEST_CASE("the fallback for an empty input is the name, alone or with {}")
{
    const std::string text = toolformat::line("ExitPlanMode", json::object());
    CHECK((text == "ExitPlanMode" || text == "ExitPlanMode {}"));
}

TEST_CASE("a known tool with missing or mistyped fields does not throw")
{
    std::string text;
    CHECK_NOTHROW(text = toolformat::line("Read", json::object()));
    CHECK(startsWith(text, "Read"));
    CHECK_FALSE(hasLineBreak(text));

    CHECK_NOTHROW(text = toolformat::line("Read", json{{"file_path", 42}}));
    CHECK(startsWith(text, "Read"));

    CHECK_NOTHROW(text = toolformat::line("Bash", json{{"command", json::array({1, 2})}}));
    CHECK(startsWith(text, "Bash"));

    CHECK_NOTHROW(text = toolformat::line("Write", json::array({"not", "an", "object"})));
    CHECK(startsWith(text, "Write"));

    CHECK_NOTHROW(text = toolformat::details("Write", json{{"file_path", nullptr}}));
    CHECK_NOTHROW(text = toolformat::details("Edit", json::object()));
}

TEST_CASE("UTF-8 in paths passes through unchanged")
{
    const std::string path = "papers/r\xc3\xa9sum\xc3\xa9.pdf"; // "résumé"
    const std::string text = toolformat::line("Read", json{{"file_path", path}});
    CHECK(contains(text, path));
}

TEST_CASE("details for Bash shows the whole command, however long")
{
    std::string command = "echo start";
    for (int i = 0; i < 200; ++i)
        command += " && echo step" + std::to_string(i);
    command += " && rm -rf build-END-MARKER";
    const std::string text = toolformat::details("Bash", json{{"command", command}});
    CHECK(contains(text, command));
}

TEST_CASE("details for Write shows the path and the whole content")
{
    std::string content = "# Summary\n\n";
    for (int i = 0; i < 100; ++i)
        content += "Paragraph " + std::to_string(i) + " of the summary.\n";
    content += "LAST-LINE-MARKER\n";
    const std::string text =
        toolformat::details("Write", json{{"file_path", "/tmp/project/notes.md"}, {"content", content}});
    CHECK(contains(text, "/tmp/project/notes.md"));
    CHECK(contains(text, "Paragraph 0 of the summary."));
    CHECK(contains(text, "Paragraph 99 of the summary."));
    CHECK(contains(text, "LAST-LINE-MARKER"));
}

TEST_CASE("details for Edit shows the path and both strings")
{
    const json input = {{"file_path", "notes.md"},
                        {"old_string", "first draft"},
                        {"new_string", "second draft"}};
    const std::string text = toolformat::details("Edit", input);
    CHECK(contains(text, "notes.md"));
    CHECK(contains(text, "first draft"));
    CHECK(contains(text, "second draft"));
}

TEST_CASE("details for an unknown tool shows every key and value")
{
    const json input = json::parse(R"({"title":"Draft","tags":["alpha","beta"],"count":3})");
    const std::string text = toolformat::details("mcp__notes__save", input);
    for (const char* needle : {"title", "Draft", "tags", "alpha", "beta", "count", "3"})
        CHECK(contains(text, needle));
    // Indented, so several keys span several lines.
    CHECK(contains(text, "\n"));
}
