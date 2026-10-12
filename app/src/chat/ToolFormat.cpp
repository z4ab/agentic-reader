#include "chat/ToolFormat.h"

#include <cstdint>
#include <optional>

using nlohmann::json;

namespace toolformat
{
namespace
{

// The string value of `key`, or nothing when the input is not an object or
// the value is not a non-empty string.
std::optional<std::string> text(const json& input, const char* key)
{
    if (!input.is_object())
        return std::nullopt;
    const auto it = input.find(key);
    if (it == input.end() || !it->is_string())
        return std::nullopt;
    std::string value = it->get<std::string>();
    if (value.empty())
        return std::nullopt;
    return value;
}

// A string or integer value as text, such as Read's "pages".
std::optional<std::string> textOrNumber(const json& input, const char* key)
{
    if (const auto value = text(input, key))
        return value;
    if (!input.is_object())
        return std::nullopt;
    const auto it = input.find(key);
    if (it != input.end() && it->is_number_integer())
        return std::to_string(it->get<std::int64_t>());
    return std::nullopt;
}

std::optional<std::int64_t> integer(const json& input, const char* key)
{
    if (!input.is_object())
        return std::nullopt;
    const auto it = input.find(key);
    if (it == input.end() || !it->is_number_integer())
        return std::nullopt;
    return it->get<std::int64_t>();
}

std::string dump(const json& input, int indent)
{
    return input.dump(indent, ' ', false, json::error_handler_t::replace);
}

// Replaces each line break with a return symbol so the text fits one line.
std::string oneLine(std::string_view value)
{
    static constexpr std::string_view kBreak = " \xe2\x86\xb5 "; // " ↵ "
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < value.size() && value[i + 1] == '\n')
                ++i;
            out += kBreak;
        } else {
            out += c;
        }
    }
    return out;
}

std::string fallback(std::string_view name, const json& input)
{
    std::string out(name);
    if (input.is_null() || (input.is_object() && input.empty()))
        return oneLine(out);
    out += ' ';
    out += dump(input, -1);
    return oneLine(out);
}

// "lines 20-29" for Read's offset and limit, or nothing.
std::string readRange(const json& input)
{
    const auto offset = integer(input, "offset");
    const auto limit = integer(input, "limit");
    if (offset && limit && *limit > 0)
        return " lines " + std::to_string(*offset) + "-" + std::to_string(*offset + *limit - 1);
    if (offset)
        return " from line " + std::to_string(*offset);
    if (limit && *limit > 0)
        return " lines 1-" + std::to_string(*limit);
    return {};
}

std::optional<std::string> known(std::string_view name, const json& input)
{
    if (name == "Read") {
        const auto path = text(input, "file_path");
        if (!path)
            return std::nullopt;
        std::string out = "Read " + *path;
        if (const auto pages = textOrNumber(input, "pages"))
            out += " pages " + *pages;
        else
            out += readRange(input);
        return out;
    }
    if (name == "Glob") {
        const auto pattern = text(input, "pattern");
        if (!pattern)
            return std::nullopt;
        std::string out = "Glob " + *pattern;
        if (const auto path = text(input, "path"))
            out += " in " + *path;
        return out;
    }
    if (name == "Grep") {
        const auto pattern = text(input, "pattern");
        if (!pattern)
            return std::nullopt;
        std::string out = "Grep \"" + *pattern + "\"";
        if (const auto path = text(input, "path"))
            out += " in " + *path;
        if (const auto glob = text(input, "glob"))
            out += " (" + *glob + ")";
        return out;
    }
    if (name == "Write" || name == "Edit") {
        const auto path = text(input, "file_path");
        if (!path)
            return std::nullopt;
        return std::string(name) + " " + *path;
    }
    if (name == "Bash") {
        const auto command = text(input, "command");
        if (!command)
            return std::nullopt;
        return "Bash " + *command;
    }
    if (name == "WebFetch") {
        const auto url = text(input, "url");
        if (!url)
            return std::nullopt;
        return "WebFetch " + *url;
    }
    if (name == "WebSearch") {
        const auto query = text(input, "query");
        if (!query)
            return std::nullopt;
        return "WebSearch " + *query;
    }
    return std::nullopt;
}

} // namespace

std::string line(std::string_view name, const json& input)
{
    if (auto out = known(name, input))
        return oneLine(*out);
    return fallback(name, input);
}

std::string details(std::string_view name, const json& input)
{
    if (name == "Bash") {
        if (const auto command = text(input, "command")) {
            std::string out = *command;
            if (const auto description = text(input, "description"))
                out += "\n\n(" + *description + ")";
            return out;
        }
    } else if (name == "Write") {
        const auto path = text(input, "file_path");
        if (path && input.contains("content") && input["content"].is_string())
            return "File: " + *path + "\n\nContent:\n" + input["content"].get<std::string>();
    } else if (name == "Edit") {
        const auto path = text(input, "file_path");
        if (path && input.contains("old_string") && input["old_string"].is_string()
            && input.contains("new_string") && input["new_string"].is_string()) {
            std::string out = "File: " + *path;
            const auto all = input.find("replace_all");
            if (all != input.end() && all->is_boolean() && all->get<bool>())
                out += "\n(every occurrence)";
            return out + "\n\nReplace:\n" + input["old_string"].get<std::string>()
                   + "\n\nWith:\n" + input["new_string"].get<std::string>();
        }
    }
    return dump(input, 2);
}

} // namespace toolformat
