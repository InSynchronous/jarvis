#include "Tui.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/string.hpp"
#include "ftxui/screen/terminal.hpp"
#include "termmark.h"

namespace {

// ---- ANSI -> FTXUI element conversion --------------------------------
// termmark renders markdown by writing ANSI escape sequences to std::cout.
// In a full-screen TUI we capture that output and re-render it as FTXUI
// elements, preserving colors, bold, underline, etc.

struct Style {
	bool bold = false;
	bool dim = false;
	bool italic = false;
	bool underline = false;
	bool inverse = false;
	bool strike = false;
	bool has_fg = false;
	bool has_bg = false;
	ftxui::Color fg{};
	ftxui::Color bg{};
	bool has_link = false;
	std::string link;

	void reset() { *this = Style{}; }
};

ftxui::Color ansiColor(int base, bool bright) {
	switch (base) {
		case 0: return bright ? ftxui::Color::GrayDark : ftxui::Color::Black;
		case 1: return bright ? ftxui::Color::RedLight : ftxui::Color::Red;
		case 2: return bright ? ftxui::Color::GreenLight : ftxui::Color::Green;
		case 3: return bright ? ftxui::Color::YellowLight : ftxui::Color::Yellow;
		case 4: return bright ? ftxui::Color::BlueLight : ftxui::Color::Blue;
		case 5: return bright ? ftxui::Color::MagentaLight : ftxui::Color::Magenta;
		case 6: return bright ? ftxui::Color::CyanLight : ftxui::Color::Cyan;
		case 7: return bright ? ftxui::Color::White : ftxui::Color::GrayLight;
	}
	return ftxui::Color::Default;
}

std::vector<int> parseSgrCodes(const std::string& params) {
	std::vector<int> out;
	std::string cur;
	for (char c : params) {
		if (c == ';') {
			if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
	return out;
}

void applySgr(Style& style, const std::vector<int>& codes) {
	for (size_t i = 0; i < codes.size(); i++) {
		int n = codes[i];
		switch (n) {
			case 0: style.reset(); break;
			case 1: style.bold = true; break;
			case 2: style.dim = true; break;
			case 3: style.italic = true; break;
			case 4: style.underline = true; break;
			case 7: style.inverse = true; break;
			case 9: style.strike = true; break;
			case 30: case 31: case 32: case 33:
			case 34: case 35: case 36: case 37:
				style.fg = ansiColor(n - 30, false);
				style.has_fg = true;
				break;
			case 90: case 91: case 92: case 93:
			case 94: case 95: case 96: case 97:
				style.fg = ansiColor(n - 90, true);
				style.has_fg = true;
				break;
			case 40: case 41: case 42: case 43:
			case 44: case 45: case 46: case 47:
				style.bg = ansiColor(n - 40, false);
				style.has_bg = true;
				break;
			case 100: case 101: case 102: case 103:
			case 104: case 105: case 106: case 107:
				style.bg = ansiColor(n - 100, true);
				style.has_bg = true;
				break;
			case 38:
				if (i + 2 < codes.size() && codes[i + 1] == 5) {
					style.fg = ftxui::Color(
						static_cast<ftxui::Color::Palette256>(codes[i + 2]));
					style.has_fg = true;
					i += 2;
				} else if (i + 4 < codes.size() && codes[i + 1] == 2) {
					style.fg = ftxui::Color::RGB(
						static_cast<uint8_t>(codes[i + 2]),
						static_cast<uint8_t>(codes[i + 3]),
						static_cast<uint8_t>(codes[i + 4]));
					style.has_fg = true;
					i += 4;
				}
				break;
			case 48:
				if (i + 2 < codes.size() && codes[i + 1] == 5) {
					style.bg = ftxui::Color(
						static_cast<ftxui::Color::Palette256>(codes[i + 2]));
					style.has_bg = true;
					i += 2;
				} else if (i + 4 < codes.size() && codes[i + 1] == 2) {
					style.bg = ftxui::Color::RGB(
						static_cast<uint8_t>(codes[i + 2]),
						static_cast<uint8_t>(codes[i + 3]),
						static_cast<uint8_t>(codes[i + 4]));
					style.has_bg = true;
					i += 4;
				}
				break;
			default: break;
		}
	}
}

void parseOsc(Style& style, const std::string& body) {
	if (body.rfind("8;", 0) != 0) return;
	size_t first = body.find(';', 2);
	if (first == std::string::npos) return;
	std::string url = body.substr(first + 1);
	if (url.empty()) {
		style.has_link = false;
		style.link.clear();
	} else {
		style.link = url;
		style.has_link = true;
	}
}

ftxui::Element styleElement(const std::string& text, const Style& style) {
	if (text.empty()) return ftxui::emptyElement();
	ftxui::Element e = ftxui::text(text);
	if (style.bold) e |= ftxui::bold;
	if (style.dim) e |= ftxui::dim;
	if (style.italic) e |= ftxui::italic;
	if (style.underline) e |= ftxui::underlined;
	if (style.inverse) e |= ftxui::inverted;
	if (style.strike) e |= ftxui::strikethrough;
	if (style.has_fg) e |= ftxui::color(style.fg);
	if (style.has_bg) e |= ftxui::bgcolor(style.bg);
	if (style.has_link) e |= ftxui::hyperlink(style.link);
	return e;
}

struct Run {
	std::string text;
	Style style;
};

void parseAnsiRuns(const std::string& line, std::vector<Run>& out) {
	Style style;
	std::string current;

	auto flush = [&]() {
		if (!current.empty()) {
			out.push_back({current, style});
			current.clear();
		}
	};

	size_t i = 0;
	while (i < line.size()) {
		char c = line[i];
		if (c == '\033' && i + 1 < line.size()) {
			if (line[i + 1] == '[') {
				flush();
				i += 2;
				std::string params;
				while (i < line.size()) {
					char cc = line[i];
					if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z'))
						break;
					params += cc;
					i++;
				}
				char final = (i < line.size()) ? line[i] : 0;
				if (final == 'm') applySgr(style, parseSgrCodes(params));
				i++;  // skip the final byte
			} else if (line[i + 1] == ']') {
				flush();
				i += 2;
				std::string body;
				while (i < line.size()) {
					if (line[i] == '\033' && i + 1 < line.size() &&
						line[i + 1] == '\\') {
						i += 2;
						break;
					}
					body += line[i];
					i++;
				}
				parseOsc(style, body);
			} else {
				current += c;
				i++;
			}
		} else {
			if (c != '\r') current += c;
			i++;
		}
	}
	flush();
}

ftxui::Element parseAnsiLineAsIs(const std::string& line) {
	std::vector<Run> runs;
	parseAnsiRuns(line, runs);
	if (runs.empty()) return ftxui::emptyElement();
	std::vector<ftxui::Element> parts;
	for (const auto& r : runs) parts.push_back(styleElement(r.text, r.style));
	if (parts.size() == 1) return parts[0];
	return ftxui::hbox(std::move(parts));
}

std::vector<std::vector<Run>> wrapRuns(const std::vector<Run>& runs, int width) {
	std::vector<std::vector<Run>> lines;
	std::vector<Run> line;
	int line_width = 0;

	auto flush = [&]() {
		if (!line.empty()) {
			lines.push_back(std::move(line));
			line.clear();
		}
		line_width = 0;
	};

	for (const auto& run : runs) {
		size_t i = 0;
		while (i < run.text.size()) {
			size_t j = run.text.find_first_of(" \t", i);
			if (j == std::string::npos) j = run.text.size();
			std::string word = run.text.substr(i, j - i);
			if (!word.empty()) {
				int ww = ftxui::string_width(word);
				if (line_width > 0 && line_width + 1 + ww > width) flush();
				if (line_width > 0) {
					line.push_back({" ", run.style});
					line_width += 1;
				}
				line.push_back({word, run.style});
				line_width += ww;
			}
			i = j;
			while (i < run.text.size() && (run.text[i] == ' ' || run.text[i] == '\t'))
				i++;
		}
	}
	flush();
	return lines;
}

std::string stripAnsi(const std::string& line) {
	std::string out;
	size_t i = 0;
	while (i < line.size()) {
		char c = line[i];
		if (c == '\033' && i + 1 < line.size()) {
			if (line[i + 1] == '[') {
				i += 2;
				while (i < line.size() &&
					   !((line[i] >= 'a' && line[i] <= 'z') ||
						 (line[i] >= 'A' && line[i] <= 'Z')))
					i++;
				if (i < line.size()) i++;
			} else if (line[i + 1] == ']') {
				i += 2;
				while (i + 1 < line.size() &&
					   !(line[i] == '\033' && line[i + 1] == '\\'))
					i++;
				i += 2;
			} else {
				i += 2;
			}
		} else {
			if (c != '\r') out += c;
			i++;
		}
	}
	return out;
}

bool isBoxLine(const std::string& s) {
	static const char* box[] = {"│", "┌", "└", "├", "┐", "┘", "┴", "┤"};
	for (const char* b : box)
		if (s.rfind(b, 0) == 0) return true;
	return false;
}

bool isHrLine(const std::string& s) {
	if (s.empty()) return false;
	for (char c : s)
		if (c != '_') return false;
	return true;
}

ftxui::Element ansiToElement(const std::string& ansi, int width) {
	std::vector<std::string> lines;
	std::string cur;
	for (char c : ansi) {
		if (c == '\n') {
			lines.push_back(cur);
			cur.clear();
		} else {
			cur += c;
		}
	}
	if (!cur.empty()) lines.push_back(cur);

	std::vector<ftxui::Element> elems;
	for (const auto& line : lines) {
		if (line.empty()) {
			elems.push_back(ftxui::emptyElement());
			continue;
		}
		// termmark lays out code/table boxes and horizontal rules itself:
		// don't re-wrap them, just pass those lines through.
		std::string visible = stripAnsi(line);
		if (isBoxLine(visible) || isHrLine(visible)) {
			elems.push_back(parseAnsiLineAsIs(line));
			continue;
		}
		std::vector<Run> runs;
		parseAnsiRuns(line, runs);
		if (runs.empty()) {
			elems.push_back(ftxui::emptyElement());
			continue;
		}
		for (auto& wrapped : wrapRuns(runs, width)) {
			std::vector<ftxui::Element> parts;
			for (const auto& r : wrapped)
				parts.push_back(styleElement(r.text, r.style));
			elems.push_back(ftxui::hbox(std::move(parts)));
		}
	}
	if (elems.empty()) return ftxui::emptyElement();
	if (elems.size() == 1) return elems[0];
	return ftxui::vbox(std::move(elems));
}

std::string renderMarkdownToString(const std::string& markdown) {
	std::ostringstream oss;
	auto* old_buf = std::cout.rdbuf(oss.rdbuf());
	termmark::renderMarkdown(markdown);
	std::cout.rdbuf(old_buf);
	return oss.str();
}

}  // namespace

struct JarvisTui::Impl {
	struct Segment {
		bool reasoning = false;
		std::string text;
	};

	struct Message {
		std::string role;              // user | assistant | info | error | tool | toolout
		std::string text;              // plain text for non-assistant messages
		std::vector<Segment> segments; // assistant streaming content
		std::string rendered;          // ANSI markdown for a finished assistant message
		bool streaming = false;
	};

	// ---- shared display state ----
	std::mutex mutex;
	std::vector<Message> messages;
	std::string model = "model";
	std::string mode = "Coder";
	std::string workspace = ".";
	std::string server_info = "";
	bool fast_mode = false;
	bool thinking = false;
	bool listening = false;

	// ---- scroll state ----
	float scroll_y = 1.f;
	bool scrolled_by_user = false;

	// ---- prompt queue ----
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::deque<std::string> prompt_queue;
	bool exiting = false;

	// ---- FTXUI ----
	ftxui::ScreenInteractive screen{ftxui::ScreenInteractive::Fullscreen()};
	std::string input_text;

	Impl() = default;

	// ---- rendering ----
	ftxui::Element statusBarElement() {
		std::lock_guard<std::mutex> lock(mutex);
		auto dot = ftxui::text(" · ") | ftxui::color(ftxui::Color::GrayDark);
		std::vector<ftxui::Element> parts = {
			ftxui::text("JARVIS") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
			dot,
			ftxui::text(mode) | ftxui::bold | ftxui::color(ftxui::Color::BlueLight),
			dot,
			ftxui::text(model) | ftxui::dim,
			ftxui::text(" ") | ftxui::flex,
			ftxui::text(workspace) | ftxui::dim,
		};
		if (!server_info.empty()) {
			parts.push_back(dot);
			parts.push_back(ftxui::text(server_info) | ftxui::dim);
		}
		if (fast_mode) {
			parts.push_back(dot);
			parts.push_back(ftxui::text("fast") | ftxui::color(ftxui::Color::Yellow) | ftxui::bold);
		}
		if (listening) {
			parts.push_back(dot);
			parts.push_back(ftxui::text("● listening") | ftxui::color(ftxui::Color::Green) | ftxui::bold);
		}
		if (thinking) {
			parts.push_back(dot);
			parts.push_back(ftxui::text("● thinking") | ftxui::color(ftxui::Color::Yellow) | ftxui::bold);
		}
		return ftxui::hbox(std::move(parts));
	}

	int paneWidth() const {
		return std::max(20, ftxui::Terminal::Size().dimx - 3);
	}

	ftxui::Element bubble(ftxui::Element content, ftxui::Color border_color) {
		return ftxui::vbox({
			       ftxui::text(" "),
			       ftxui::hbox({
				       ftxui::text("  "),
				       content | ftxui::flex,
				       ftxui::text("  "),
			       }),
			       ftxui::text(" "),
		       })
		       | ftxui::bgcolor(ftxui::Color::RGB(30, 30, 36))
		       | ftxui::borderStyled(ftxui::ROUNDED, border_color);
	}

	ftxui::Element messageElement(const Message& m) {
		if (m.role == "user") {
			return ftxui::vbox({
				ftxui::text("You") | ftxui::bold | ftxui::color(ftxui::Color::GreenLight),
				ftxui::text(" "),
				bubble(ftxui::paragraph(m.text), ftxui::Color::Green),
			});
		}
		if (m.role == "assistant") {
			ftxui::Elements body;
			if (!m.rendered.empty()) {
				body.push_back(ansiToElement(m.rendered, paneWidth()));
			} else {
				for (const auto& seg : m.segments) {
					if (seg.text.empty()) continue;
					ftxui::Element e;
					if (seg.reasoning) {
						e = ftxui::hbox({
							ftxui::text("▸ ") | ftxui::color(ftxui::Color::GrayDark),
							ftxui::paragraph(seg.text),
						}) | ftxui::dim | ftxui::italic | ftxui::color(ftxui::Color::GrayDark);
					} else {
						e = ftxui::paragraph(seg.text);
					}
					body.push_back(e);
				}
			}
			if (m.streaming) body.push_back(ftxui::text("▊") | ftxui::color(ftxui::Color::Cyan));
			if (body.empty()) body.push_back(ftxui::emptyElement());
			return ftxui::vbox({
				ftxui::text("Jarvis") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
				ftxui::text(" "),
				bubble(ftxui::vbox(std::move(body)), ftxui::Color::Cyan),
			});
		}
		if (m.role == "error") {
			return ftxui::hbox({
				ftxui::text("⚠ ") | ftxui::bold | ftxui::color(ftxui::Color::Red),
				ftxui::paragraph(m.text) | ftxui::color(ftxui::Color::Red),
			});
		}
		if (m.role == "tool") {
			return ftxui::hbox({
				ftxui::text("▸ ") | ftxui::dim | ftxui::color(ftxui::Color::Yellow),
				ftxui::paragraph(m.text) | ftxui::dim | ftxui::color(ftxui::Color::Yellow),
			});
		}
		if (m.role == "toolout") {
			return ftxui::hbox({
				ftxui::text("    "),
				ftxui::paragraph(m.text) | ftxui::dim,
			});
		}
		// info
		return ftxui::hbox({
			ftxui::text("• ") | ftxui::dim | ftxui::color(ftxui::Color::Cyan),
			ftxui::paragraph(m.text) | ftxui::dim | ftxui::color(ftxui::Color::Cyan),
		});
	}

	ftxui::Element conversationElementLocked() {
		if (messages.empty()) {
			auto line = [](ftxui::Element e) {
				return ftxui::hbox({ftxui::emptyElement() | ftxui::flex, e,
				                    ftxui::emptyElement() | ftxui::flex});
			};
			return ftxui::vbox({
				ftxui::emptyElement() | ftxui::flex,
				line(ftxui::text("JARVIS") | ftxui::bold | ftxui::color(ftxui::Color::Cyan)),
				ftxui::text(" "),
				line(ftxui::text("Ready. Type a message below.") | ftxui::dim),
				ftxui::emptyElement() | ftxui::flex,
			});
		}
		ftxui::Elements items;
		for (const auto& m : messages) {
			items.push_back(messageElement(m));
			items.push_back(ftxui::text(""));
		}
		return ftxui::vbox(std::move(items));
	}

	bool handleScrollEvent(ftxui::Event event) {
		if (event.is_mouse()) {
			auto mouse = event.mouse();
			std::lock_guard<std::mutex> lock(mutex);
			if (mouse.button == ftxui::Mouse::WheelUp) {
				scroll_y += 0.1f;
				if (scroll_y > 1.f) scroll_y = 1.f;
				scrolled_by_user = true;
				return true;
			}
			if (mouse.button == ftxui::Mouse::WheelDown) {
				scroll_y -= 0.1f;
				if (scroll_y < 0.f) scroll_y = 0.f;
				scrolled_by_user = true;
				return true;
			}
		}
		return false;
	}

	ftxui::Component buildLayout() {
		auto conversation = ftxui::Renderer([this] {
			std::lock_guard<std::mutex> lock(mutex);
			if (!messages.empty() && messages.back().streaming && !scrolled_by_user)
				scroll_y = 1.f;
			return conversationElementLocked() |
			       ftxui::focusPositionRelative(0.f, scroll_y) |
			       ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex;
		});
		auto scrollable = ftxui::CatchEvent(conversation, [this](ftxui::Event event) {
			return handleScrollEvent(event);
		});

		auto status = ftxui::Renderer([this] { return statusBarElement(); });

		auto hint = ftxui::Renderer([] {
			auto item = [](const std::string& s) {
				return ftxui::text(s) | ftxui::dim;
			};
			return ftxui::hbox({
				item("/commands"),
				ftxui::text("  ·  ") | ftxui::dim,
				item("/exit"),
			}) | ftxui::center;
		});

		ftxui::InputOption input_option;
		input_option.multiline = true;
		input_option.placeholder = "Type a prompt (Enter to submit)...";
		input_option.transform = [](ftxui::InputState state) {
			if (state.is_placeholder) state.element |= ftxui::dim;
			return state.element;
		};
		input_option.on_enter = [this] { submit(); };
		auto input = ftxui::Input(&input_text, input_option);
		auto input_box = ftxui::Renderer(input, [input] {
			auto content = ftxui::hbox({
				ftxui::text("❯ ") | ftxui::bold | ftxui::color(ftxui::Color::Cyan),
				input->Render() | ftxui::flex,
				ftxui::text("  ↵ send") | ftxui::dim,
			});
			return ftxui::vbox({
				       ftxui::text(" "),
				       content,
				       ftxui::text(" "),
			       })
			       | ftxui::bgcolor(ftxui::Color::RGB(30, 30, 36))
			       | ftxui::borderStyled(ftxui::ROUNDED, ftxui::Color::Cyan)
			       | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 5);
		});

		auto layout = ftxui::Container::Vertical({
			status,
			scrollable,
			input_box,
			hint,
		});
		input->TakeFocus();
		return layout;
	}

	void submit() {
		std::string prompt;
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			prompt = input_text;
			while (!prompt.empty() && prompt.back() == '\n') prompt.pop_back();
			if (prompt.empty()) return;
			input_text.clear();
			prompt_queue.push_back(prompt);
		}
		queue_cv.notify_one();
		{
			std::lock_guard<std::mutex> lock(mutex);
			scroll_y = 1.f;
			scrolled_by_user = false;
		}
		screen.PostEvent(ftxui::Event::Custom);
	}

	void postRefresh() { screen.PostEvent(ftxui::Event::Custom); }

	void addMessage(const std::string& role, const std::string& text) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			messages.push_back(Message{role, text, {}, "", false});
		}
		postRefresh();
	}

	void setStatus(const std::string& field, const std::string& value) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (field == "model") model = value;
			else if (field == "mode") mode = value;
			else if (field == "workspace") workspace = value;
			else if (field == "server") server_info = value;
		}
		postRefresh();
	}
};

JarvisTui::JarvisTui() : impl_(std::make_unique<Impl>()) {}
JarvisTui::~JarvisTui() = default;

void JarvisTui::run() {
	impl_->screen.TrackMouse(true);
	auto root = impl_->buildLayout();
	impl_->screen.Loop(root);
}

void JarvisTui::exitLoop() {
	impl_->screen.ExitLoopClosure()();
}

bool JarvisTui::popPrompt(std::string& out) {
	std::unique_lock<std::mutex> lock(impl_->queue_mutex);
	impl_->queue_cv.wait(lock, [this] {
		return !impl_->prompt_queue.empty() || impl_->exiting;
	});
	if (impl_->exiting && impl_->prompt_queue.empty()) return false;
	out = impl_->prompt_queue.front();
	impl_->prompt_queue.pop_front();
	return true;
}

void JarvisTui::stop() {
	{
		std::lock_guard<std::mutex> lock(impl_->queue_mutex);
		impl_->exiting = true;
	}
	impl_->queue_cv.notify_all();
}

void JarvisTui::addUserMessage(const std::string& text) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->messages.push_back(Impl::Message{"user", text, {}, "", false});
		impl_->scroll_y = 1.f;
		impl_->scrolled_by_user = false;
	}
	impl_->postRefresh();
}

void JarvisTui::addInfoMessage(const std::string& text) {
	impl_->addMessage("info", text);
}

void JarvisTui::addErrorMessage(const std::string& text) {
	impl_->addMessage("error", text);
}

void JarvisTui::addToolCall(const std::string& name) {
	impl_->addMessage("tool", name);
}

void JarvisTui::addToolOutput(const std::string& text) {
	impl_->addMessage("toolout", text);
}

void JarvisTui::clearConversation() {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->messages.clear();
		impl_->scroll_y = 1.f;
		impl_->scrolled_by_user = false;
	}
	impl_->postRefresh();
}

void JarvisTui::beginAssistant() {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->messages.push_back(
			Impl::Message{"assistant", "", {}, "", true});
		impl_->scroll_y = 1.f;
		impl_->scrolled_by_user = false;
	}
	impl_->postRefresh();
}

void JarvisTui::streamReasoning(const std::string& token) {
	if (token.empty()) return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		auto& m = impl_->messages.back();
		if (m.role != "assistant") {
			m = Impl::Message{"assistant", "", {}, "", true};
		}
		if (m.segments.empty() || !m.segments.back().reasoning)
			m.segments.push_back({Impl::Segment{true, ""}});
		m.segments.back().text += token;
	}
	impl_->postRefresh();
}

void JarvisTui::streamToken(const std::string& token) {
	if (token.empty()) return;
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		auto& m = impl_->messages.back();
		if (m.role != "assistant") {
			m = Impl::Message{"assistant", "", {}, "", true};
		}
		if (m.segments.empty() || m.segments.back().reasoning)
			m.segments.push_back({Impl::Segment{false, ""}});
		m.segments.back().text += token;
	}
	impl_->postRefresh();
}

void JarvisTui::endAssistant(const std::string& markdown) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		if (impl_->messages.empty() || impl_->messages.back().role != "assistant") {
			impl_->messages.push_back(Impl::Message{"assistant", "", {}, "", false});
		}
		auto& m = impl_->messages.back();
		m.streaming = false;
		if (!markdown.empty()) {
			m.segments.clear();
			m.rendered = renderMarkdownToString(markdown);
		} else if (m.segments.empty()) {
			impl_->messages.pop_back();
		}
		impl_->scroll_y = 1.f;
		impl_->scrolled_by_user = false;
	}
	impl_->postRefresh();
}

void JarvisTui::setModel(const std::string& model) {
	impl_->setStatus("model", model);
}
void JarvisTui::setMode(const std::string& mode) {
	impl_->setStatus("mode", mode);
}
void JarvisTui::setWorkspace(const std::string& workspace) {
	impl_->setStatus("workspace", workspace);
}
void JarvisTui::setServerInfo(const std::string& info) {
	impl_->setStatus("server", info);
}

void JarvisTui::setFastMode(bool on) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->fast_mode = on;
	}
	impl_->postRefresh();
}
void JarvisTui::setThinking(bool on) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->thinking = on;
	}
	impl_->postRefresh();
}
void JarvisTui::setListening(bool on) {
	{
		std::lock_guard<std::mutex> lock(impl_->mutex);
		impl_->listening = on;
	}
	impl_->postRefresh();
}
