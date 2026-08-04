#include "MCPManager.h"
#include "Microphone.h"
#include "Whisper.h"
#include "Ollama.h"
#include "SessionManager.h"
#include "Tui.h"
#include "termmark.h"
#include "nlohmann/json.hpp"

#include <iostream>
#include <cstdlib>
#include <ostream>
#include <string>
#include <functional>
#include <filesystem>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/ioctl.h>

namespace color {
	const char* reset   = "\033[0m";
	const char* bold    = "\033[1m";
	const char* dim     = "\033[2m";
	const char* cyan    = "\033[36m";
	const char* green   = "\033[32m";
	const char* yellow  = "\033[33m";
	const char* magenta = "\033[35m";
	const char* red     = "\033[31m";
	const char* blue    = "\033[34m";
}

static int terminalWidth() {
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
		return w.ws_col;
	return 80;
}

static int terminalRows() {
	struct winsize w;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0)
		return w.ws_row;
	return 24;
}

static size_t computeScreenLines(const std::string& text, int width) {
	if (text.empty()) return 0;
	size_t lines = 0;
	int col = 0;
	for (char c : text) {
		if (c == '\n') {
			lines++;
			col = 0;
		} else {
			col++;
			if (col >= width) {
				lines++;
				col = 0;
			}
		}
	}
	if (col > 0) lines++;
	return lines;
}

// Display abstraction shared by the TUI and the plain-stdout fallback.
struct Sink {
	std::function<void(const std::string&)> user;
	std::function<void(const std::string&)> info;
	std::function<void(const std::string&)> error;
	std::function<void(const std::string&)> tool;
	std::function<void(const std::string&)> toolout;
	std::function<void()> begin;
	std::function<void(const std::string&)> reasoning;
	std::function<void(const std::string&)> token;
	std::function<void(const std::string&)> end;
	std::function<void()> clear;
};

int main(int argc, char* argv[])
{
	std::string prog = std::filesystem::path(argv[0]).filename().string();
	std::string model = "deepseek-v4-flash-free";
	std::string mode = "code";
	std::string workspace = std::filesystem::current_path().string();
	bool resumeFlag = false;

	auto expandTilde = [](const std::string& path) -> std::string {
		if (!path.empty() && path[0] == '~') {
			const char* home = std::getenv("HOME");
			if (home) return std::string(home) + path.substr(1);
		}
		return path;
	};

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			std::cout << "Usage: " << prog << " [OPTIONS] [FOLDER] [MODEL]\n\n"
					  << "  FOLDER  Project directory to work in (default: current directory)\n"
					  << "  MODEL   OpenRouter model ID (default: openai/gpt-oss-20b)\n\n"
					  << "Options:\n"
					  << "  -m, --mode <mode>   Agent mode: code, hack, plan, ask (default: code)\n"
					  << "  --resume            Resume the last session\n"
					  << "  -h, --help          Show this help message\n"
					  << "\nExamples:\n"
					  << "  " << prog << "\n"
					  << "  " << prog << " /path/to/project\n"
					  << "  " << prog << " /path/to/project --mode hack\n"
					  << "  " << prog << " -m plan /path/to/project nvidia/nemotron-3-super:free\n";
			return 0;
		}
		if (arg == "-m" || arg == "--mode") {
			if (i + 1 < argc) {
				mode = argv[++i];
			if (mode != "code" && mode != "hack" && mode != "plan" && mode != "ask") {
				std::cerr << "Invalid mode: " << mode << "\n"
						  << "Valid modes: code, hack, plan, ask\n";
					return 1;
				}
			} else {
				std::cerr << "Error: --mode requires an argument\n";
				return 1;
			}
	} else if (arg == "--resume") {
		resumeFlag = true;
	} else if (arg[0] == '/' || arg[0] == '.' || arg[0] == '~') {
		workspace = std::filesystem::absolute(expandTilde(arg)).string();
		} else {
			model = arg;
		}
	}

	std::string modeLabel = (mode == "code") ? "Coder" :
							(mode == "hack") ? "Pentester" :
							(mode == "ask") ? "Assistant" : "Planner";
	std::string promptPath = "mcp/" + mode + ".md";
	bool fastMode = false;

	Microphone mic;
	Whisper whisper("models/ggml-base.en.bin");
	//Ollama lama("https://ai.hackclub.com/proxy/v1/chat/completions", model, "", promptPath);
	//Ollama lama("https://openrouter.ai/api/v1/chat/completions", model, "", promptPath);
	Ollama lama("https://opencode.ai/zen/v1/chat/completions", model, "", promptPath);

	SessionManager sessionMgr(workspace);
	std::string currentSessionId = sessionMgr.generateSessionId();
	std::string resumeSessionId;
	bool loadedResume = false;

	if (resumeFlag) {
		std::string lastId = sessionMgr.getLastSessionId();
		if (!lastId.empty()) {
			json savedMessages = sessionMgr.loadSession(lastId);
			if (!savedMessages.empty()) {
				lama.setMessages(savedMessages);
				currentSessionId = lastId;
				loadedResume = true;
			}
		}
	} else {
		resumeSessionId = sessionMgr.getLastSessionId();
	}

	MCPManager mcp;
	if (!mcp.loadConfig("mcp-servers.json", workspace)) {
		std::cerr << color::red << "Failed to load mcp-servers.json" << color::reset << "\n";
		return 1;
	}
	if (!mcp.connectAll()) {
		std::cerr << color::red << "Failed to connect to MCP servers" << color::reset << "\n";
		return 1;
	}

	for (auto& tool : mcp.listAllTools()) {
		lama.addTool(tool);
	}

	const std::string serverInfo = std::to_string(mcp.serverNames().size()) + " server(s), " +
								   std::to_string(mcp.toolNames().size()) + " tool(s)";

	bool isTty = isatty(STDOUT_FILENO);

	// ---- Read input for the plain-stdout fallback ----
	auto stdoutRead = [&](std::string& out) -> bool {
		std::cout << color::green << color::bold << "> " << color::reset << std::flush;
		std::string line;
		if (!std::getline(std::cin, line)) return false;
		out = line;
		return true;
	};

	// ---- Display sinks ----
	bool stdoutReasoningActive = false;
	char stdoutLastChar = '\n';
	Sink stdoutSink;
	stdoutSink.user = [](const std::string& m) {
		std::cout << color::green << color::bold << "[USER] " << color::reset
				  << m << std::endl;
	};
	stdoutSink.info = [](const std::string& m) {
		std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
				  << m << std::endl;
	};
	stdoutSink.error = [](const std::string& m) {
		std::cout << color::red << color::bold << "[ERROR] " << color::reset
				  << m << std::endl;
	};
	stdoutSink.tool = [](const std::string& m) {
		std::cout << color::yellow << color::bold << "[TOOL] " << color::reset
				  << color::dim << m << color::reset << std::endl;
	};
	stdoutSink.toolout = [](const std::string& m) {
		std::cout << color::yellow << color::bold << "[TOOL OUTPUT] " << color::reset
				  << color::dim << m << color::reset << std::endl;
	};
	stdoutSink.begin = [&stdoutReasoningActive]() { stdoutReasoningActive = false; };
	stdoutSink.reasoning = [&stdoutReasoningActive, &stdoutLastChar](const std::string& token) {
		if (!stdoutReasoningActive) {
			std::cout << color::dim;
			stdoutReasoningActive = true;
		}
		std::cout << token << std::flush;
		stdoutLastChar = token.back();
	};
	stdoutSink.token = [&stdoutReasoningActive, &stdoutLastChar](const std::string& token) {
		if (stdoutReasoningActive) {
			std::cout << color::reset << "\n";
			stdoutReasoningActive = false;
		}
		std::cout << token << std::flush;
		stdoutLastChar = token.back();
	};
	stdoutSink.end = [&stdoutReasoningActive, &stdoutLastChar](const std::string& text) {
		if (stdoutReasoningActive) {
			std::cout << color::reset;
			stdoutReasoningActive = false;
		}
		if (text.empty()) {
			if (stdoutLastChar != '\n') std::cout << std::endl;
			return;
		}
		size_t lines = computeScreenLines(text, terminalWidth());
		if (lines > 0 && (int)lines < terminalRows()) {
			if (text.back() == '\n') {
				std::cout << "\033[" << lines << "A";
			} else {
				std::cout << "\033[" << (lines - 1) << "A\r";
			}
			std::cout << "\033[J";
			termmark::renderMarkdown(text);
		} else {
			std::cout << "\n" << color::dim << "────────────────────────────"
					  << color::reset << "\n";
			termmark::renderMarkdown(text);
		}
	};
	stdoutSink.clear = []() { std::cout << "\033[2J\033[H" << std::flush; };

	// ---- Agent turn: chat, retry, tool loop ----
	auto handlePrompt = [&](const std::string& finalPrompt, Sink& d) {
		d.info("Thinking...");
		d.begin();

		auto streamReasoning = [&](const std::string& token) {
			if (!token.empty()) d.reasoning(token);
		};
		auto streamToken = [&](const std::string& token) {
			if (!token.empty()) d.token(token);
		};

		auto extractContent = [](const json& output) -> std::string {
			const auto& msg = output["message"];
			if (msg.contains("content") && !msg["content"].is_null() &&
				msg["content"].is_string()) {
				return msg["content"].get<std::string>();
			}
			return "";
		};
		auto isEmptyResponse = [](const json& output) -> bool {
			const auto& msg = output["message"];
			bool contentEmpty = !msg.contains("content")
								|| msg["content"].is_null()
								|| (msg["content"].is_string() && msg["content"].get<std::string>().empty());
			return contentEmpty && !msg.contains("tool_calls");
		};

		json output = lama.chat(finalPrompt, streamToken, streamReasoning);
		d.end(extractContent(output));

		for (int retry = 0; retry < 3; retry++) {
			if (!isEmptyResponse(output)) break;
			d.info("Empty response, waiting 1s and retrying...");
			std::this_thread::sleep_for(std::chrono::seconds(1));
			d.begin();
			output = lama.chat(std::string("continue"), streamToken, streamReasoning);
			d.end(extractContent(output));
		}

		while (output["message"].contains("tool_calls")) {
			json tool_calls = output["message"]["tool_calls"];

			for (auto& call : tool_calls) {
				std::string name = call["function"]["name"];
				std::string args_str = call["function"].value("arguments", "{}");
				json arguments;
				try {
					arguments = json::parse(args_str);
				} catch (...) {
					arguments = json::object();
				}

				d.tool(name);

				std::string tool_text;
				if (!mcp.hasTool(name)) {
					auto valid = mcp.toolNames();
					std::string valid_list;
					for (size_t i = 0; i < valid.size(); i++) {
						if (i > 0) valid_list += ", ";
						valid_list += valid[i];
					}
					tool_text = "Error: Unknown tool '" + name + "'. Valid tools are: " + valid_list;
					d.error(tool_text);
				} else {
					try {
						json result = mcp.callTool(name, arguments);

						if (result.contains("content") && result["content"].is_array()) {
							for (auto& item : result["content"]) {
								if (item.contains("text")) {
									tool_text += item["text"].get<std::string>();
								}
							}
						} else {
							tool_text = result.dump();
						}
					} catch (const std::exception& e) {
						tool_text = std::string("Error calling tool '") + name + "': " + e.what();
						d.error(tool_text);
					}
				}

				d.toolout(tool_text.substr(0, 2000));

				const int max_tool_chars = 15000;
				if ((int)tool_text.size() > max_tool_chars) {
					std::string truncated = tool_text.substr(0, max_tool_chars);
					truncated += "\n... [truncated, total " + std::to_string(tool_text.size()) + " chars]";
					tool_text = truncated;
				}

				json tool_response = {
					{"role", "tool"},
					{"content", tool_text}
				};

				if (call.contains("id") && !call["id"].is_null()) {
					tool_response["tool_call_id"] = call["id"];
				}

				lama.addMessage(tool_response);
			}

			d.begin();
			output = lama.complete(streamToken, streamReasoning);
			d.end(extractContent(output));

			for (int retry = 0; retry < 3; retry++) {
				if (!isEmptyResponse(output)) break;
				d.info("Empty response, waiting 1s and retrying...");
				std::this_thread::sleep_for(std::chrono::seconds(1));
				d.begin();
				output = lama.chat(std::string("continue"), streamToken, streamReasoning);
				d.end(extractContent(output));
			}
		}
	};

	// ---- Shared agent loop ----
	auto runAgentLoop = [&](const std::function<bool(std::string&)>& readInput, Sink& d) {
		bool pendingResume = !resumeSessionId.empty() && !loadedResume;

		if (pendingResume) {
			d.info("Found previous session " + resumeSessionId +
				   ". Press Enter to resume, or type a prompt to start fresh.");
		} else if (resumeFlag && !loadedResume) {
			d.info("No previous session found. Starting fresh.");
		}

		d.info("Model: " + model + " | Mode: " + modeLabel + " | Workspace: " + workspace);
		d.info("Connected to " + serverInfo + ".");

		std::string input;
		while (readInput(input)) {
			if (pendingResume) {
				pendingResume = false;
				if (input.empty()) {
					json savedMessages = sessionMgr.loadSession(resumeSessionId);
					if (!savedMessages.empty()) {
						lama.setMessages(savedMessages);
						currentSessionId = resumeSessionId;
						d.info("Resumed session " + resumeSessionId + " (" +
							   std::to_string(savedMessages.size()) + " messages).");
					} else {
						d.error("Session not found: " + resumeSessionId);
					}
					continue;
				}
				if (input == "n" || input == "N" || input == "no" || input == "No") {
					continue;
				}
			}

			std::string prompt;

			if (input == "/servers") {
				std::string list;
				for (auto& n : mcp.serverNames()) {
					if (!list.empty()) list += ", ";
					list += n;
				}
				d.info("[SERVERS] " + list);
				list.clear();
				for (auto& t : mcp.toolNames()) {
					if (!list.empty()) list += ", ";
					list += t;
				}
				d.info("[TOOLS] (" + std::to_string(mcp.toolNames().size()) + " total) " + list);
				continue;
			}

			if (input.rfind("/sessions", 0) == 0) {
				auto sessions = sessionMgr.listSessions();
				if (sessions.empty()) {
					d.info("No saved sessions.");
				} else {
					d.info("[SESSIONS]");
					for (auto& s : sessions) {
						std::string marker = (s.id == currentSessionId) ? " *" : "";
						d.info("  " + s.id + marker + "  " +
							   std::to_string(s.message_count) + " messages  " + s.updated_at);
					}
				}
				continue;
			}

			if (input.rfind("/resume", 0) == 0) {
				std::string targetId;
				size_t spacePos = input.find(' ');
				if (spacePos != std::string::npos)
					targetId = input.substr(spacePos + 1);

				if (targetId.empty()) {
					targetId = sessionMgr.getLastSessionId();
				}

				if (targetId.empty()) {
					d.info("No sessions to resume.");
					continue;
				}

				json savedMessages = sessionMgr.loadSession(targetId);
				if (savedMessages.empty()) {
					d.error("Session not found: " + targetId);
					continue;
				}

				lama.setMessages(savedMessages);
				currentSessionId = targetId;
				d.info("Resumed session " + targetId + " (" +
					   std::to_string(savedMessages.size()) + " messages).");
				continue;
			}

			if (input == "/exit") {
				sessionMgr.saveSession(currentSessionId, lama.getMessages());
				d.info("Session saved. Goodbye!");
				break;
			}

			if (input == "/clear") {
				d.clear();
				continue;
			}

			if (input == "/fast") {
				fastMode = !fastMode;
				d.info("Fast mode " + std::string(fastMode ? "enabled" : "disabled") + ".");
				continue;
			}

			if (input == "/voice" || input.rfind("/voice ", 0) == 0) {
				mic.start();
				d.info("Listening... type anything and press Enter to stop.");
				std::string stop;
				readInput(stop);
				mic.stop();

				auto audio = mic.getAudio();
				d.info("Processing...");
				std::string raw = whisper.transcribe(audio);
				d.user(raw);

				d.info("Mode? [code|hack|plan|ask] (Enter keeps " + modeLabel + "):");
				std::string modeInput;
				readInput(modeInput);

				if (modeInput == "code" || modeInput == "hack" ||
					modeInput == "plan" || modeInput == "ask") {
					mode = modeInput;
					modeLabel = (mode == "code") ? "Coder" :
								(mode == "hack") ? "Pentester" :
								(mode == "ask") ? "Assistant" : "Planner";
					promptPath = "mcp/" + mode + ".md";
					lama.setMode(promptPath);
					d.info("Switched to " + modeLabel + " mode (mcp/" + mode + ".md).");
				}

				prompt = raw;
			} else {
				// check for /model in text input
				size_t modelPos = input.find("/model");
				if (modelPos != std::string::npos) {
					std::string afterModel = input.substr(modelPos + 6);
					std::string newModel;
					auto space = afterModel.find_first_not_of(' ');
					if (space != std::string::npos) {
						auto end = afterModel.find_first_of(" \n", space);
						newModel = (end != std::string::npos)
							? afterModel.substr(space, end - space)
							: afterModel.substr(space);
					}

					if (!newModel.empty()) {
						model = newModel;
						lama.setModel(model);
						d.info("Switched to model " + model + ".");
					} else {
						d.info("Current model: " + model);
					}

					input.erase(modelPos, 6 + afterModel.size());
					auto start = input.find_first_not_of(" \t\n");
					auto end = input.find_last_not_of(" \t\n");
					input = (start != std::string::npos) ? input.substr(start, end - start + 1) : "";
				}

				// check for /mode in text input
				size_t modePos = input.find("/mode");
				if (modePos != std::string::npos) {
					std::string afterMode = input.substr(modePos + 5);
					std::string newMode;
					auto space = afterMode.find_first_not_of(' ');
					if (space != std::string::npos) {
						auto end = afterMode.find_first_of(" \n", space);
						newMode = (end != std::string::npos)
							? afterMode.substr(space, end - space)
							: afterMode.substr(space);
					}

					if (newMode == "code" || newMode == "hack" ||
						newMode == "plan" || newMode == "ask") {
						mode = newMode;
						modeLabel = (mode == "code") ? "Coder" :
									(mode == "hack") ? "Pentester" :
									(mode == "ask") ? "Assistant" : "Planner";
						promptPath = "mcp/" + mode + ".md";
						lama.setMode(promptPath);
						d.info("Switched to " + modeLabel + " mode (mcp/" + mode + ".md).");
					} else {
						d.error("Invalid mode: " + newMode);
						continue;
					}

					prompt = input;
					prompt.erase(modePos, 5 + afterMode.size());
					auto start = prompt.find_first_not_of(" \t\n");
					auto end = prompt.find_last_not_of(" \t\n");
					prompt = (start != std::string::npos) ? prompt.substr(start, end - start + 1) : "";
				} else {
					prompt = input;
				}
			}

			if (prompt.empty()) {
				continue;
			}

			d.user(prompt);

			std::string finalPrompt = prompt;
			if (fastMode) {
				finalPrompt = "[FAST MODE: Answer in 1-3 sentences max. No preamble, no formatting, no explanation. Direct answer only.]\n\n" + prompt;
			}

			handlePrompt(finalPrompt, d);
			sessionMgr.saveSession(currentSessionId, lama.getMessages());
		}
	};

	if (!isTty) {
		// Piped / non-interactive: plain stdout streaming, no TUI.
		runAgentLoop(stdoutRead, stdoutSink);
		return 0;
	}

	// ---- Full-screen TUI path ----
	JarvisTui tui;
	tui.setModel(model);
	tui.setMode(modeLabel);
	tui.setWorkspace(workspace);
	tui.setServerInfo(serverInfo);
	tui.setFastMode(fastMode);

	Sink tuiSink;
	tuiSink.user = [&tui](const std::string& m) { tui.addUserMessage(m); };
	tuiSink.info = [&tui](const std::string& m) { tui.addInfoMessage(m); };
	tuiSink.error = [&tui](const std::string& m) { tui.addErrorMessage(m); };
	tuiSink.tool = [&tui](const std::string& m) { tui.addToolCall(m); };
	tuiSink.toolout = [&tui](const std::string& m) { tui.addToolOutput(m); };
	tuiSink.begin = [&tui]() { tui.beginAssistant(); };
	tuiSink.reasoning = [&tui](const std::string& t) { tui.streamReasoning(t); };
	tuiSink.token = [&tui](const std::string& t) { tui.streamToken(t); };
	tuiSink.end = [&tui](const std::string& m) { tui.endAssistant(m); };
	tuiSink.clear = [&tui]() { tui.clearConversation(); };

	auto tuiRead = [&tui](std::string& out) -> bool { return tui.popPrompt(out); };

	std::thread worker([&] {
		runAgentLoop(tuiRead, tuiSink);
		tui.stop();
	});

	tui.run();
	tui.stop();
	if (worker.joinable()) worker.join();

	return 0;
}
