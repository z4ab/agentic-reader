#pragma once

#include <filesystem>
#include <string>

// The project instructions file, <root>/CLAUDE.md. New chats read it when
// they start (the sidecar runs them in the project folder). No wx types, so
// tests can use these without a GUI.

// <root>/CLAUDE.md.
std::filesystem::path instructionsPath(const std::filesystem::path& root);

// Reads <root>/CLAUDE.md byte for byte into `text`. A missing file is not an
// error: returns true with `text` empty. Returns false with `error` set and
// `text` empty when the path exists but cannot be read as a file.
bool loadInstructions(const std::filesystem::path& root, std::string& text, std::string& error);

// Writes `text` byte for byte to <root>/CLAUDE.md through a temporary file in
// the same folder that is renamed over it. When CLAUDE.md is a symlink, its
// target is replaced and the link is kept. The file keeps its permissions.
// Returns false with `error` set on failure, leaving CLAUDE.md as it was and
// no temporary file behind. Never creates
// <root> or <root>/.reader/, so it works for an unsaved project.
bool saveInstructions(const std::filesystem::path& root, const std::string& text,
                      std::string& error);
