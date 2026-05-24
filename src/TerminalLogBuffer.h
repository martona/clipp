#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class TerminalLogBuffer {
public:
    enum class Color {
        Default,
        Gray,
        DimCyan,
        Cyan,
        Green,
        Yellow,
        Red,
    };

    struct Run {
        std::wstring text;
        Color color = Color::Default;
    };

    struct Line {
        std::vector<Run> runs;
        std::wstring plainText;
    };

    struct AppendResult {
        std::vector<Line> addedLines;
        std::size_t removedLineCount = 0;
    };

    explicit TerminalLogBuffer(std::size_t maxLines = 1000)
        : maxLines_(maxLines) {
    }

    void Clear() {
        lines_.clear();
    }

    AppendResult AppendAnsiLogText(const std::wstring& text) {
        AppendResult result;

        std::wstring trimmedText = text;
        while (!trimmedText.empty() && (trimmedText.back() == L'\r' || trimmedText.back() == L'\n')) {
            trimmedText.pop_back();
        }

        if (trimmedText.empty()) {
            return result;
        }

        std::size_t lineStart = 0;
        const std::wstring_view trimmedView(trimmedText.data(), trimmedText.size());
        while (lineStart <= trimmedText.size()) {
            const std::size_t lineEnd = trimmedText.find_first_of(L"\r\n", lineStart);
            Line line = ParseAnsiLine(lineEnd == std::wstring::npos
                ? trimmedView.substr(lineStart)
                : trimmedView.substr(lineStart, lineEnd - lineStart));

            lines_.push_back(line);
            result.addedLines.push_back(std::move(line));

            if (lineEnd == std::wstring::npos) {
                break;
            }

            lineStart = lineEnd + 1;
            if (trimmedText[lineEnd] == L'\r' && lineStart < trimmedText.size() && trimmedText[lineStart] == L'\n') {
                ++lineStart;
            }
        }

        while (lines_.size() > maxLines_) {
            lines_.pop_front();
            ++result.removedLineCount;
        }

        return result;
    }

    void SetAnsiLogText(const std::vector<std::wstring>& lines) {
        Clear();
        for (const auto& line : lines) {
            AppendAnsiLogText(line);
        }
    }

    std::size_t LineCount() const {
        return lines_.size();
    }

    const std::deque<Line>& Lines() const {
        return lines_;
    }

    std::wstring PlainText() const {
        std::wstring text;
        for (const auto& line : lines_) {
            text += line.plainText;
            text.push_back(L'\n');
        }
        return text;
    }

    static Line ParseAnsiLine(std::wstring_view line) {
        Line parsed;
        Color currentColor = Color::Default;
        std::wstring currentText;

        auto flushText = [&]() {
            if (currentText.empty()) {
                return;
            }

            parsed.plainText += currentText;
            parsed.runs.push_back(Run{ std::move(currentText), currentColor });
            currentText.clear();
        };

        std::size_t i = 0;
        while (i < line.length()) {
            if (line[i] == L'\x1b' && i + 1 < line.length() && line[i + 1] == L'[') {
                flushText();

                i += 2;
                std::wstring code;
                while (i < line.length() && line[i] != L'm') {
                    code += line[i];
                    ++i;
                }
                if (i < line.length() && line[i] == L'm') {
                    ++i;
                    currentColor = ColorForAnsiCode(code);
                }
            } else {
                currentText += line[i];
                ++i;
            }
        }

        flushText();
        return parsed;
    }

private:
    static Color ColorForAnsiCode(std::wstring_view code) {
        if (code.empty()) {
            return Color::Default;
        }

        bool dim = false;
        bool bold = false;
        int foreground = -1;
        std::size_t start = 0;

        while (start <= code.size()) {
            const std::size_t end = code.find(L';', start);
            const std::wstring_view part = end == std::wstring_view::npos
                ? code.substr(start)
                : code.substr(start, end - start);
            int value = 0;
            bool parsed = !part.empty();
            for (wchar_t ch : part) {
                if (ch < L'0' || ch > L'9') {
                    parsed = false;
                    break;
                }
                value = (value * 10) + static_cast<int>(ch - L'0');
            }

            if (parsed) {
                if (value == 0) {
                    dim = false;
                    bold = false;
                    foreground = -1;
                } else if (value == 1) {
                    bold = true;
                } else if (value == 2) {
                    dim = true;
                } else if (value == 22) {
                    dim = false;
                    bold = false;
                } else if (value == 36 || value == 90 || (value >= 30 && value <= 37)) {
                    foreground = value;
                } else if (value == 39) {
                    foreground = -1;
                }
            }

            if (end == std::wstring_view::npos) {
                break;
            }
            start = end + 1;
        }

        if (foreground == 90) {
            return Color::Gray;
        }
        if (foreground == 36) {
            return dim ? Color::DimCyan : Color::Cyan;
        }
        if (bold && foreground == 32) {
            return Color::Green;
        }
        if (bold && foreground == 33) {
            return Color::Yellow;
        }
        if (bold && foreground == 31) {
            return Color::Red;
        }

        return Color::Default;
    }

    std::size_t maxLines_ = 1000;
    std::deque<Line> lines_;
};
