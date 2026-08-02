#pragma once

#include <string>

// Lightweight GitHub-flavoured markdown renderer for ImGui.
//
// Renders a subset of markdown close to how GitHub displays release notes:
//   - ATX headings (# .. ######) with a size hierarchy, bold weight, and rules under H1/H2
//   - Inline **bold**, *italic*/_italic_, `code`, ~~strike~~ and [text](url) links (clickable)
//   - Bare http(s) autolinks
//   - Unordered/ordered/task lists with nesting by indentation
//   - Blockquotes with a gutter bar, fenced ``` code blocks, and horizontal rules
//
// The renderer emits directly at the current cursor position and wraps to the
// window content width. It is safe to call every frame.
namespace ImGuiMarkdown
{
	void Render(const std::string& markdown);
}
