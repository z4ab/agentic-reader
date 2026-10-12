#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

// Text for Claude's tool calls: the grey activity line in the chat and the
// input shown in the permission dialog. Plain UTF-8, not HTML. Never throws.
namespace toolformat
{

// One line for a `tool_use`, such as "Read papers/attention.pdf pages 10-12".
// Formats Read, Glob, Grep, Write, Edit, Bash, WebFetch and WebSearch. Any
// other tool, or a known tool whose input lacks the fields its format needs,
// gives `name`, a space and the input as compact JSON (just `name` for an
// empty input). Never contains a line break.
std::string line(std::string_view name, const nlohmann::json& input);

// The input a permission request asks the user to approve, possibly over
// several lines and never truncated. Bash shows the command, Write the path
// and content, Edit the path and both strings. Other tools show the input as
// indented JSON.
std::string details(std::string_view name, const nlohmann::json& input);

} // namespace toolformat
