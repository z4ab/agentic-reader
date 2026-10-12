#include "chat/Markdown.h"

#include <md4c-html.h>

namespace markdown
{

namespace
{

void appendOutput(const MD_CHAR* text, MD_SIZE size, void* userdata)
{
    static_cast<std::string*>(userdata)->append(text, size);
}

} // namespace

std::string toHtml(std::string_view markdown)
{
    std::string html;
    if (markdown.empty())
        return html;
    html.reserve(markdown.size() * 3 / 2);
    md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()), appendOutput, &html,
            MD_DIALECT_GITHUB | MD_FLAG_NOHTML, 0);
    return html;
}

std::string escapeText(std::string_view text)
{
    std::string html;
    html.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        switch (c) {
        case '&':
            html += "&amp;";
            break;
        case '<':
            html += "&lt;";
            break;
        case '>':
            html += "&gt;";
            break;
        case '\r':
            // "\r\n" is one line break. The "\n" adds it.
            if (i + 1 >= text.size() || text[i + 1] != '\n')
                html += "<br>";
            break;
        case '\n':
            html += "<br>";
            break;
        default:
            html += c;
            break;
        }
    }
    return html;
}

} // namespace markdown
