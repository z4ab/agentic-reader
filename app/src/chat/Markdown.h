#pragma once

#include <string>
#include <string_view>

// HTML fragments for the chat message view (wxHtmlWindow). UTF-8 in and out.
namespace markdown
{

// Assistant Markdown (CommonMark with GitHub's tables, strikethrough and bare
// URL links) to an HTML fragment through md4c's md_html. Raw HTML in the input
// is escaped and shown as text (MD_FLAG_NOHTML). Unclosed constructs, such as
// a code fence cut off mid-stream, still render.
std::string toHtml(std::string_view markdown);

// User text to an HTML fragment that shows it literally: escapes &, < and >,
// interprets no Markdown, and turns each line break into <br>.
std::string escapeText(std::string_view text);

} // namespace markdown
