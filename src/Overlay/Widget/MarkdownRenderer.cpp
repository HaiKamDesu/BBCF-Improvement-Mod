#include "MarkdownRenderer.h"

#include <imgui.h>

#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

namespace
{
	// ---- palette (tuned to read like GitHub's dark theme) --------------------
	const ImVec4 kHeadingColor(0.98f, 0.98f, 1.00f, 1.00f);
	const ImVec4 kLinkColor(0.35f, 0.62f, 0.98f, 1.00f);
	const ImVec4 kQuoteColor(0.62f, 0.66f, 0.72f, 1.00f);
	const ImVec4 kCodeTextColor(0.90f, 0.80f, 0.62f, 1.00f);
	const ImU32  kCodeBgColor = IM_COL32(60, 66, 78, 110);
	const ImU32  kBlockBgColor = IM_COL32(26, 28, 33, 200);
	const ImU32  kQuoteBarColor = IM_COL32(96, 102, 112, 255);

	enum class Style
	{
		Normal,
		Bold,
		Italic,
		Strike,
		Code,
		Link,
	};

	struct Run
	{
		std::string text;
		Style       style = Style::Normal;
		std::string url; // only for Style::Link
	};

	// A piece is an atomic layout unit (a single word, or a whole code/link span).
	struct Piece
	{
		std::string text;
		Style       style = Style::Normal;
		std::string url;
		bool        spaceBefore = false;
	};

	std::string Trim(const std::string& value)
	{
		size_t first = 0;
		while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
			++first;
		size_t last = value.size();
		while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
			--last;
		return value.substr(first, last - first);
	}

	bool StartsWith(const std::string& value, const char* prefix)
	{
		const size_t prefixLen = std::strlen(prefix);
		return value.size() >= prefixLen && value.compare(0, prefixLen, prefix) == 0;
	}

	std::vector<std::string> SplitLines(const std::string& text)
	{
		std::vector<std::string> lines;
		std::stringstream stream(text);
		std::string line;
		while (std::getline(stream, line))
		{
			if (!line.empty() && line[line.size() - 1] == '\r')
				line.erase(line.size() - 1);
			lines.push_back(line);
		}
		if (text.empty())
			lines.push_back(std::string());
		return lines;
	}

	void OpenUrl(const std::string& url)
	{
		if (url.empty())
			return;
		const int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
		if (wlen <= 0)
			return;
		std::wstring wide(static_cast<size_t>(wlen), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wide[0], wlen);
		ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	// ---- inline tokenizer ----------------------------------------------------
	bool IsUrlChar(char c)
	{
		return !std::isspace(static_cast<unsigned char>(c)) &&
			c != ')' && c != ']' && c != '<' && c != '>' && c != '"';
	}

	// Trailing punctuation should not be swallowed into a bare autolink.
	size_t TrimUrlTail(const std::string& s, size_t start, size_t end)
	{
		while (end > start)
		{
			const char c = s[end - 1];
			if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')')
				--end;
			else
				break;
		}
		return end;
	}

	std::vector<Run> Tokenize(const std::string& line)
	{
		std::vector<Run> runs;
		std::string cur;
		auto flush = [&]()
		{
			if (!cur.empty())
			{
				Run r;
				r.text = cur;
				runs.push_back(r);
				cur.clear();
			}
		};

		const size_t n = line.size();
		for (size_t i = 0; i < n;)
		{
			const char c = line[i];

			// Escaped character.
			if (c == '\\' && i + 1 < n)
			{
				cur.push_back(line[i + 1]);
				i += 2;
				continue;
			}

			// Inline code `...`
			if (c == '`')
			{
				const size_t close = line.find('`', i + 1);
				if (close != std::string::npos)
				{
					flush();
					Run r;
					r.style = Style::Code;
					r.text = line.substr(i + 1, close - i - 1);
					runs.push_back(r);
					i = close + 1;
					continue;
				}
			}

			// Link [text](url)
			if (c == '[')
			{
				const size_t closeText = line.find(']', i + 1);
				if (closeText != std::string::npos && closeText + 1 < n && line[closeText + 1] == '(')
				{
					const size_t closeUrl = line.find(')', closeText + 2);
					if (closeUrl != std::string::npos)
					{
						flush();
						Run r;
						r.style = Style::Link;
						r.text = line.substr(i + 1, closeText - i - 1);
						r.url = line.substr(closeText + 2, closeUrl - closeText - 2);
						runs.push_back(r);
						i = closeUrl + 1;
						continue;
					}
				}
			}

			// Bare autolink
			if ((c == 'h' && line.compare(i, 7, "http://") == 0) ||
				(c == 'h' && line.compare(i, 8, "https://") == 0))
			{
				size_t end = i;
				while (end < n && IsUrlChar(line[end]))
					++end;
				end = TrimUrlTail(line, i, end);
				if (end > i)
				{
					flush();
					Run r;
					r.style = Style::Link;
					r.text = line.substr(i, end - i);
					r.url = r.text;
					runs.push_back(r);
					i = end;
					continue;
				}
			}

			// Emphasis: ** __ (bold), ~~ (strike), * _ (italic)
			auto emphasisSpan = [&](const char* marker, Style style) -> bool
			{
				const size_t mlen = std::strlen(marker);
				if (line.compare(i, mlen, marker) != 0)
					return false;
				const size_t close = line.find(marker, i + mlen);
				if (close == std::string::npos || close == i + mlen)
					return false;
				flush();
				Run r;
				r.style = style;
				r.text = line.substr(i + mlen, close - i - mlen);
				runs.push_back(r);
				i = close + mlen;
				return true;
			};

			if (emphasisSpan("**", Style::Bold)) continue;
			if (emphasisSpan("__", Style::Bold)) continue;
			if (emphasisSpan("~~", Style::Strike)) continue;
			if (emphasisSpan("*", Style::Italic)) continue;
			if (emphasisSpan("_", Style::Italic)) continue;

			cur.push_back(c);
			++i;
		}
		flush();
		return runs;
	}

	std::vector<Piece> ToPieces(const std::vector<Run>& runs)
	{
		std::vector<Piece> pieces;
		bool pendingSpace = false;
		for (const Run& run : runs)
		{
			if (run.style == Style::Code || run.style == Style::Link)
			{
				Piece p;
				p.text = run.text;
				p.style = run.style;
				p.url = run.url;
				p.spaceBefore = pendingSpace;
				pieces.push_back(p);
				pendingSpace = false;
				continue;
			}

			std::string word;
			auto flushWord = [&]()
			{
				if (!word.empty())
				{
					Piece p;
					p.text = word;
					p.style = run.style;
					p.spaceBefore = pendingSpace;
					pieces.push_back(p);
					word.clear();
					pendingSpace = false;
				}
			};
			for (char c : run.text)
			{
				if (std::isspace(static_cast<unsigned char>(c)))
				{
					flushWord();
					pendingSpace = true;
				}
				else
				{
					word.push_back(c);
				}
			}
			flushWord();
		}
		return pieces;
	}

	// ---- rendering -----------------------------------------------------------
	ImVec2 MeasureRun(const Piece& p)
	{
		ImVec2 sz = ImGui::CalcTextSize(p.text.c_str());
		if (p.style == Style::Bold)
			sz.x += 1.0f; // fake-bold widens by ~1px
		if (p.style == Style::Code)
			sz.x += 6.0f; // horizontal padding inside the code chip
		return sz;
	}

	void RenderRun(const Piece& p)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		if (p.style == Style::Code)
		{
			const ImVec2 pos = ImGui::GetCursorScreenPos();
			const ImVec2 textSize = ImGui::CalcTextSize(p.text.c_str());
			drawList->AddRectFilled(ImVec2(pos.x, pos.y - 1.0f),
				ImVec2(pos.x + textSize.x + 6.0f, pos.y + textSize.y + 1.0f),
				kCodeBgColor, 3.0f);
			ImGui::Dummy(ImVec2(3.0f, textSize.y)); // left padding
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, kCodeTextColor);
			ImGui::TextUnformatted(p.text.c_str());
			ImGui::PopStyleColor();
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::Dummy(ImVec2(3.0f, textSize.y)); // right padding
			return;
		}

		if (p.style == Style::Link)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, kLinkColor);
			ImGui::TextUnformatted(p.text.c_str());
			ImGui::PopStyleColor();
			const ImVec2 mn = ImGui::GetItemRectMin();
			const ImVec2 mx = ImGui::GetItemRectMax();
			drawList->AddLine(ImVec2(mn.x, mx.y - 1.0f), ImVec2(mx.x, mx.y - 1.0f),
				ImGui::GetColorU32(kLinkColor), 1.0f);
			if (ImGui::IsItemHovered())
			{
				ImGui::BeginTooltip();
				ImGui::TextUnformatted(p.url.c_str());
				ImGui::EndTooltip();
			}
			if (ImGui::IsItemClicked())
				OpenUrl(p.url);
			return;
		}

		// Normal / Bold / Italic / Strike all share the base font (no italic face
		// is bundled, so italic reuses the regular weight). Bold is faked by an
		// overdraw; strike gets a line through the middle.
		ImGui::TextUnformatted(p.text.c_str());
		const ImVec2 mn = ImGui::GetItemRectMin();
		const ImVec2 mx = ImGui::GetItemRectMax();

		if (p.style == Style::Bold)
		{
			drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
				ImVec2(mn.x + 1.0f, mn.y), ImGui::GetColorU32(ImGuiCol_Text), p.text.c_str());
		}
		else if (p.style == Style::Strike)
		{
			const float midY = mn.y + (mx.y - mn.y) * 0.5f;
			drawList->AddLine(ImVec2(mn.x, midY), ImVec2(mx.x, midY),
				ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
		}
	}

	// Lays out a single logical line of inline markdown, wrapping to the window
	// content width. Continuation lines fall at the current indent (hanging indent).
	void RenderInline(const std::string& line)
	{
		const std::vector<Piece> pieces = ToPieces(Tokenize(line));
		if (pieces.empty())
		{
			ImGui::NewLine();
			return;
		}

		const float spaceW = ImGui::CalcTextSize(" ").x;
		const float wrapMaxX = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;

		bool firstOnLine = true;
		for (const Piece& p : pieces)
		{
			const float pad = p.spaceBefore ? spaceW : 0.0f;
			const ImVec2 sz = MeasureRun(p);
			if (!firstOnLine)
			{
				const float lastMaxX = ImGui::GetItemRectMax().x;
				if (lastMaxX + pad + sz.x <= wrapMaxX)
					ImGui::SameLine(0.0f, pad);
				// otherwise let the item fall onto the next line at the current indent
			}
			RenderRun(p);
			firstOnLine = false;
		}
	}

	int CountLeadingSpaces(const std::string& line)
	{
		int count = 0;
		for (char c : line)
		{
			if (c == ' ') count += 1;
			else if (c == '\t') count += 4;
			else break;
		}
		return count;
	}
}

namespace ImGuiMarkdown
{
	void Render(const std::string& markdown)
	{
		const std::vector<std::string> lines = SplitLines(markdown);
		bool inCodeBlock = false;

		for (size_t i = 0; i < lines.size(); ++i)
		{
			const std::string& raw = lines[i];
			const std::string trimmed = Trim(raw);

			// Fenced code block toggle.
			if (StartsWith(trimmed, "```"))
			{
				inCodeBlock = !inCodeBlock;
				if (!inCodeBlock)
					ImGui::Spacing();
				continue;
			}

			if (inCodeBlock)
			{
				ImDrawList* drawList = ImGui::GetWindowDrawList();
				const ImVec2 pos = ImGui::GetCursorScreenPos();
				const float width = ImGui::GetWindowContentRegionMax().x - ImGui::GetWindowContentRegionMin().x;
				const ImVec2 sz = ImGui::CalcTextSize(raw.empty() ? " " : raw.c_str());
				drawList->AddRectFilled(ImVec2(pos.x - 2.0f, pos.y - 1.0f),
					ImVec2(pos.x + width, pos.y + sz.y + 1.0f), kBlockBgColor);
				ImGui::PushStyleColor(ImGuiCol_Text, kCodeTextColor);
				ImGui::TextUnformatted(raw.empty() ? "" : raw.c_str());
				ImGui::PopStyleColor();
				continue;
			}

			if (trimmed.empty())
			{
				ImGui::Spacing();
				continue;
			}

			// Headings.
			int headingLevel = 0;
			while (headingLevel < static_cast<int>(trimmed.size()) && headingLevel < 6 && trimmed[headingLevel] == '#')
				++headingLevel;
			if (headingLevel > 0 && headingLevel < static_cast<int>(trimmed.size()) && trimmed[headingLevel] == ' ')
			{
				const std::string content = Trim(trimmed.substr(headingLevel + 1));
				const float scale =
					headingLevel == 1 ? 1.50f :
					headingLevel == 2 ? 1.30f :
					headingLevel == 3 ? 1.15f : 1.05f;

				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, kHeadingColor);
				ImGui::SetWindowFontScale(scale);
				RenderInline(content);
				ImGui::SetWindowFontScale(1.0f);
				ImGui::PopStyleColor();
				if (headingLevel <= 2)
					ImGui::Separator();
				ImGui::Spacing();
				continue;
			}

			// Horizontal rule.
			if (trimmed == "---" || trimmed == "***" || trimmed == "___")
			{
				ImGui::Separator();
				continue;
			}

			// Blockquote.
			if (StartsWith(trimmed, ">"))
			{
				const ImVec2 start = ImGui::GetCursorScreenPos();
				ImGui::Indent(10.0f);
				ImGui::PushStyleColor(ImGuiCol_Text, kQuoteColor);
				RenderInline(Trim(trimmed.substr(1)));
				ImGui::PopStyleColor();
				ImGui::Unindent(10.0f);
				const ImVec2 end = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(start.x, start.y), ImVec2(start.x + 3.0f, end.y - 2.0f), kQuoteBarColor, 1.0f);
				continue;
			}

			// Lists (unordered, ordered, task) with nesting by indentation.
			const int leading = CountLeadingSpaces(raw);
			const std::string body = trimmed;
			const bool unordered = StartsWith(body, "- ") || StartsWith(body, "* ") || StartsWith(body, "+ ");
			const bool ordered =
				body.size() > 2 &&
				std::isdigit(static_cast<unsigned char>(body[0])) &&
				(body[1] == '.' || body[1] == ')') &&
				body.size() > 2 && body[2] == ' ';

			if (unordered || ordered)
			{
				const float indent = 8.0f + (leading / 2) * 14.0f;
				ImGui::Indent(indent);

				std::string itemText = unordered ? body.substr(2) : body.substr(3);

				// Task-list checkbox.
				if (StartsWith(itemText, "[ ] ") || StartsWith(itemText, "[x] ") || StartsWith(itemText, "[X] "))
				{
					const bool checked = itemText[1] != ' ';
					ImGui::TextUnformatted(checked ? "[x]" : "[ ]");
					ImGui::SameLine();
					itemText = itemText.substr(4);
				}
				else
				{
					ImGui::Bullet();
					ImGui::SameLine();
				}

				RenderInline(Trim(itemText));
				ImGui::Unindent(indent);
				continue;
			}

			// Plain paragraph.
			RenderInline(trimmed);
		}
	}
}
