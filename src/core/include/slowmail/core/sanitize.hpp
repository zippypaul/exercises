#pragma once
#include <string>
#include <vector>

#include "slowmail/core/types.hpp"

namespace slowmail::core {

// All sanitisation is pure, bounded and allocation-safe. Nothing here performs I/O.

// Convert an HTML document to readable plain text:
//  - drops <script>, <style>, <head>, <template>, <iframe>, <object>, <embed>, <svg> contents entirely
//  - drops <img> (tracking pixels) and never references remote resources
//  - block-level tags (p, div, br, li, tr, h1-h6, blockquote, pre) become line breaks
//  - <a href="X">text</a> becomes "text <X>" only when X starts with http(s): or mailto:; javascript:/data: dropped
//  - decodes common and numeric entities; strips control characters except \n and \t
//  - collapses excessive blank lines; result is capped at max_chars with truncated flag
std::string html_to_text(const std::string& html, std::size_t max_chars, bool& truncated);

// Plain-text bodies still get control-character stripping and capping.
std::string sanitize_text(const std::string& text, std::size_t max_chars, bool& truncated);

// Single-line fields: strip control chars and newlines, trim, cap length (UTF-8 safe cut).
std::string sanitize_line(const std::string& s, std::size_t max_chars);

// Cuts a UTF-8 string at a code point boundary so we never emit an invalid sequence.
std::string utf8_truncate(const std::string& s, std::size_t max_bytes);

// Validates and bounds attachment metadata; never trusts size or name from the server.
AttachmentMeta sanitize_attachment(const AttachmentMeta& raw);
std::vector<AttachmentMeta> sanitize_attachments(const std::vector<AttachmentMeta>& raw);

// Human readable size for UI ("12 KB", "1.4 MB"); -1 -> "unknown size".
std::string human_size(std::int64_t bytes);

} // namespace slowmail::core
