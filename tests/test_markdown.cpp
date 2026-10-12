// M5.2: Markdown to HTML for the chat message view (wxHtmlWindow).
//
// Assumed interface, app/src/chat/Markdown.h (no wx types, plain UTF-8):
//
//   namespace markdown
//   {
//   // Assistant Markdown to an HTML fragment, through md4c's md_html.
//   // Raw HTML in the input is escaped and shown as text, never passed
//   // through (md4c's MD_FLAG_NOHTML). Unclosed constructs, such as a code
//   // fence cut off mid-stream, still render.
//   std::string toHtml(std::string_view markdown);
//
//   // User text to an HTML fragment that shows it literally: escapes
//   // &, < and >, interprets no Markdown, and turns each line break into <br>.
//   std::string escapeText(std::string_view text);
//   }
//
// Checks look for substrings, so the implementation may wrap the fragment or
// add attributes, as long as the named tags and escapes are present.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "chat/Markdown.h"

#include <string>

namespace
{

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("a heading renders as an HTML heading")
{
    const std::string html = markdown::toHtml("# Attention\n\nBody text.\n\n## Details\n");
    CHECK(contains(html, "<h1"));
    CHECK(contains(html, "Attention</h1>"));
    CHECK(contains(html, "<h2"));
    CHECK(contains(html, "Details</h2>"));
    CHECK(contains(html, "Body text."));
    CHECK_FALSE(contains(html, "# Attention"));
}

TEST_CASE("a fenced code block renders as pre/code with its contents escaped and literal")
{
    const std::string html =
        markdown::toHtml("```cpp\nint x = a < b && c;\n**not bold**\n```\n");
    CHECK(contains(html, "<pre"));
    CHECK(contains(html, "<code"));
    CHECK(contains(html, "a &lt; b &amp;&amp; c;"));
    // Markdown inside a code block stays as typed.
    CHECK(contains(html, "**not bold**"));
    CHECK_FALSE(contains(html, "<strong>"));
    CHECK_FALSE(contains(html, "```"));
}

TEST_CASE("an unclosed code fence, as seen mid-stream, still renders as code")
{
    const std::string html = markdown::toHtml("Intro\n\n```python\ndef f(x):\n    return x");
    CHECK(contains(html, "<pre"));
    CHECK(contains(html, "def f(x):"));
    CHECK_FALSE(contains(html, "```"));
}

TEST_CASE("inline formatting and lists render")
{
    const std::string html = markdown::toHtml("**bold** *it* `code`\n\n- one\n- two\n");
    CHECK((contains(html, "<strong>bold</strong>") || contains(html, "<b>bold</b>")));
    CHECK((contains(html, "<em>it</em>") || contains(html, "<i>it</i>")));
    CHECK(contains(html, "<code>code</code>"));
    CHECK(contains(html, "<ul"));
    CHECK(contains(html, "<li>one</li>"));
    CHECK(contains(html, "<li>two</li>"));
}

TEST_CASE("raw HTML in assistant Markdown is escaped, not rendered")
{
    const std::string html = markdown::toHtml(
        "Before\n\n<script>alert(1)</script>\n\n"
        "Inline <b>bold?</b> and <img src=x onerror=y> end.\n\n"
        "<div>block html</div>\n");
    CHECK_FALSE(contains(html, "<script"));
    CHECK_FALSE(contains(html, "<img"));
    CHECK_FALSE(contains(html, "<div"));
    CHECK_FALSE(contains(html, "<b>"));
    CHECK(contains(html, "&lt;script&gt;alert(1)&lt;/script&gt;"));
    CHECK(contains(html, "&lt;b&gt;bold?&lt;/b&gt;"));
    CHECK(contains(html, "&lt;img src=x onerror=y&gt;"));
    CHECK(contains(html, "&lt;div&gt;block html&lt;/div&gt;"));
    CHECK(contains(html, "Before"));
}

TEST_CASE("plain special characters in Markdown are escaped")
{
    const std::string html = markdown::toHtml("AT&T and 1 < 2");
    CHECK(contains(html, "AT&amp;T and 1 &lt; 2"));
}

TEST_CASE("links render as anchors with their URL")
{
    const std::string html =
        markdown::toHtml("See [the paper](https://arxiv.org/abs/1706.03762) now.");
    CHECK(contains(html, "<a href=\"https://arxiv.org/abs/1706.03762\""));
    CHECK(contains(html, ">the paper</a>"));
    CHECK_FALSE(contains(html, "]("));
}

TEST_CASE("UTF-8 text passes through unchanged")
{
    // "é √" as UTF-8 bytes.
    const std::string text = "Unicode: \xc3\xa9 \xe2\x88\x9a";
    CHECK(contains(markdown::toHtml(text), text));
    CHECK(contains(markdown::escapeText(text), text));
}

TEST_CASE("empty input gives no visible content")
{
    CHECK(markdown::toHtml("").find_first_not_of(" \n") == std::string::npos);
    CHECK(markdown::escapeText("").empty());
}

TEST_CASE("user text is escaped as plain text")
{
    const std::string html = markdown::escapeText("a < b & c > d <b>x</b>");
    CHECK(html == "a &lt; b &amp; c &gt; d &lt;b&gt;x&lt;/b&gt;");
}

TEST_CASE("user text interprets no Markdown")
{
    const std::string html =
        markdown::escapeText("# not a heading\n**not bold** [not](a link) `not code`");
    CHECK_FALSE(contains(html, "<h1"));
    CHECK_FALSE(contains(html, "<strong>"));
    CHECK_FALSE(contains(html, "<a "));
    CHECK_FALSE(contains(html, "<code>"));
    CHECK(contains(html, "# not a heading"));
    CHECK(contains(html, "**not bold** [not](a link) `not code`"));
}

TEST_CASE("user text keeps its line breaks")
{
    const std::string html = markdown::escapeText("line one\nline two");
    const auto one = html.find("line one");
    const auto br = html.find("<br");
    const auto two = html.find("line two");
    REQUIRE(one != std::string::npos);
    REQUIRE(br != std::string::npos);
    REQUIRE(two != std::string::npos);
    CHECK(one < br);
    CHECK(br < two);
}
