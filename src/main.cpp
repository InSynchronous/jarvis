#include "MCPManager.h"
#include "Microphone.h"
#include "Whisper.h"
#include "Ollama.h"
#include "SessionManager.h"
#include "termmark.h"
#include "nlohmann/json.hpp"

#include <iostream>
#include <cstdlib>
#include <ostream>
#include <string>
#include <filesystem>
#include <thread>
#include <chrono>

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

int main(int argc, char* argv[])
{
	std::string prog = std::filesystem::path(argv[0]).filename().string();
	std::string model = "big-pickle";
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
	std::string currentSessionId;

	if (resumeFlag) {
		std::string lastId = sessionMgr.getLastSessionId();
		if (!lastId.empty()) {
			json savedMessages = sessionMgr.loadSession(lastId);
			if (!savedMessages.empty()) {
				lama.setMessages(savedMessages);
				currentSessionId = lastId;
				std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
						  << "Resumed session " << color::bold << lastId << color::reset
						  << " (" << savedMessages.size() << " messages)" << std::endl;
			}
		}
		if (currentSessionId.empty()) {
			std::cout << color::yellow << "No previous session found. Starting fresh." << color::reset << std::endl;
			currentSessionId = sessionMgr.generateSessionId();
		}
	} else {
		std::string lastId = sessionMgr.getLastSessionId();
		if (!lastId.empty()) {
			auto sessions = sessionMgr.listSessions();
			for (auto& s : sessions) {
				if (s.id == lastId) {
					std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
							  << "Found previous session " << color::bold << lastId << color::reset
							  << " (" << s.message_count << " messages). Resume? [Y/n] " << std::flush;
					std::string choice;
					std::getline(std::cin, choice);
					if (choice.empty() || choice == "y" || choice == "Y" || choice == "yes") {
						json savedMessages = sessionMgr.loadSession(lastId);
						if (!savedMessages.empty()) {
							lama.setMessages(savedMessages);
							currentSessionId = lastId;
							std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
									  << "Resumed session " << color::bold << lastId << color::reset
									  << " (" << savedMessages.size() << " messages)" << std::endl;
						}
					}
					break;
				}
			}
		}
		if (currentSessionId.empty())
			currentSessionId = sessionMgr.generateSessionId();
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

	std::cout << "Model: " << model << " | Mode: " << color::magenta << modeLabel << color::reset
			  << " | Workspace: " << color::dim << workspace << color::reset << std::endl;
	std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
			  << "Connected to " << color::bold << mcp.serverNames().size() << color::reset
			  << " MCP server(s), " << color::bold << mcp.toolNames().size() << color::reset
			  << " tool(s) loaded." << "\n";

	while (1) {
		std::cout << color::cyan << color::bold << "[" << modeLabel << "] " << color::reset
				  << color::dim << "Type /fast for short answers, /voice to speak, /servers to list tools, or type a prompt:" << color::reset << std::endl;
		std::cout << color::green << color::bold << "> " << color::reset << std::flush;

		std::string input;
		std::getline(std::cin, input);
		if (input.empty()) continue;

		std::string prompt;

		if (input == "/servers") {
			std::cout << color::cyan << color::bold << "[SERVERS] " << color::reset << std::endl;
			for (auto& name : mcp.serverNames()) {
				std::cout << "  " << color::green << name << color::reset << std::endl;
			}
			std::cout << color::cyan << color::bold << "[TOOLS] " << color::reset
					  << "(" << mcp.toolNames().size() << " total)" << std::endl;
			for (auto& tool : mcp.toolNames()) {
				std::cout << "  " << color::dim << tool << color::reset << std::endl;
			}
			std::cout << std::endl;
			continue;
		}

		if (input.rfind("/sessions", 0) == 0) {
			auto sessions = sessionMgr.listSessions();
			if (sessions.empty()) {
				std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
						  << "No saved sessions." << std::endl;
			} else {
				std::cout << color::cyan << color::bold << "[SESSIONS] " << color::reset << std::endl;
				for (auto& s : sessions) {
					std::string marker = (s.id == currentSessionId) ? (std::string(color::green) + " *" + color::reset) : "";
					std::cout << "  " << color::bold << s.id << color::reset << marker
							  << "  " << color::dim << s.message_count << " messages"
							  << "  " << s.updated_at << color::reset << std::endl;
				}
			}
			std::cout << std::endl;
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
				std::cout << color::yellow << "No sessions to resume." << color::reset << std::endl;
				continue;
			}

			json savedMessages = sessionMgr.loadSession(targetId);
			if (savedMessages.empty()) {
				std::cout << color::red << "Session not found: " << targetId << color::reset << std::endl;
				continue;
			}

			lama.setMessages(savedMessages);
			currentSessionId = targetId;
			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
					  << "Resumed session " << color::bold << targetId << color::reset
					  << " (" << savedMessages.size() << " messages)" << std::endl;
			continue;
		}

		if (input == "/exit") {
			sessionMgr.saveSession(currentSessionId, lama.getMessages());
			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
					  << "Session saved. Goodbye!" << std::endl;
			break;
		}

		if (input == "/clear") {
			std::cout << "\033[2J\033[H" << std::flush;
			continue;
		}

		if (input == "/fast") {
			fastMode = !fastMode;
			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
					  << "Fast mode " << (fastMode ? color::green : color::red)
					  << (fastMode ? "enabled" : "disabled") << color::reset << std::endl;
			continue;
		}

		if (input == "/voice" || input.rfind("/voice ", 0) == 0) {
			mic.start();
			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
					  << "Speak now... Press any key to stop";
			std::cin.get();
			mic.stop();

			auto audio = mic.getAudio();
			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
					  << color::dim << "Processing..." << color::reset << std::endl;
			std::string raw = whisper.transcribe(audio);

			std::cout << color::green << color::bold << "[USER] " << color::reset
					  << raw << std::endl;

			std::cout << color::dim << "Mode? [code|hack|plan|ask] (current: " << modeLabel << "): "
					  << color::reset << std::flush;
			std::string modeInput;
			std::getline(std::cin, modeInput);

			if (modeInput == "code" || modeInput == "hack" || modeInput == "plan" || modeInput == "ask") {
				mode = modeInput;
				modeLabel = (mode == "code") ? "Coder" :
							(mode == "hack") ? "Pentester" :
							(mode == "ask") ? "Assistant" : "Planner";
				promptPath = "mcp/" + mode + ".md";
				lama.setMode(promptPath);
				std::cout << color::magenta << color::bold << "[MODE] " << color::reset
						  << "Switched to " << color::magenta << modeLabel << color::reset << std::endl;
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
					std::cout << color::magenta << color::bold << "[MODEL] " << color::reset
							  << "Switched to " << color::magenta << model << color::reset << std::endl;
				} else {
					std::cout << color::yellow << "Current model: " << model << color::reset << std::endl;
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

				if (newMode == "code" || newMode == "hack" || newMode == "plan" || newMode == "ask") {
					mode = newMode;
					modeLabel = (mode == "code") ? "Coder" :
								(mode == "hack") ? "Pentester" :
								(mode == "ask") ? "Assistant" : "Planner";
					promptPath = "mcp/" + mode + ".md";
					lama.setMode(promptPath);
					std::cout << color::magenta << color::bold << "[MODE] " << color::reset
							  << "Switched to " << color::magenta << modeLabel << color::reset
							  << " mode (mcp/" << mode << ".md)" << std::endl;
				} else {
					std::cout << color::red << "Invalid mode: " << newMode
							  << "\nValid modes: code, hack, plan, ask" << color::reset << std::endl;
					std::cout << std::endl;
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
			std::cout << std::endl;
			continue;
		}

		std::string finalPrompt = prompt;
		if (fastMode) {
			finalPrompt = "[FAST MODE: Answer in 1-3 sentences max. No preamble, no formatting, no explanation. Direct answer only.]\n\n" + prompt;
		}

		std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset
				  << color::dim << "Thinking..." << color::reset << std::endl;

		std::string streamed;
		json output = lama.chat(finalPrompt, [&streamed](const std::string& token) {
			streamed += token;
		});

		for (int retry = 0; retry < 3; retry++) {
			auto& content = output["message"]["content"];
			bool contentEmpty = !output["message"].contains("content")
								|| content.is_null()
								|| (content.is_string() && content.get<std::string>().empty());
			bool empty = contentEmpty && !output["message"].contains("tool_calls");
			if (!empty) break;
			std::cout << color::yellow << color::bold << "  [RETRY] " << color::reset
					  << color::dim << "Empty response, waiting 1s and retrying..." << color::reset << std::endl;
			std::this_thread::sleep_for(std::chrono::seconds(1));
			streamed.clear();
			output = lama.chat(std::string("continue"), [&streamed](const std::string& token) {
				streamed += token;
			});
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

				std::cout << color::yellow << color::bold << "  [TOOL] " << color::reset
						  << color::dim << name << color::reset << std::endl;

				std::string tool_text;
				if (!mcp.hasTool(name)) {
					auto valid = mcp.toolNames();
					std::string valid_list;
					for (size_t i = 0; i < valid.size(); i++) {
						if (i > 0) valid_list += ", ";
						valid_list += valid[i];
					}
					tool_text = "Error: Unknown tool '" + name + "'. Valid tools are: " + valid_list;
					std::cout << color::red << color::bold << "  [ERROR] " << color::reset
							  << color::dim << tool_text << color::reset << std::endl;
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
						std::cout << color::red << color::bold << "  [ERROR] " << color::reset
								  << color::dim << tool_text << color::reset << std::endl;
					}
				}

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

			streamed.clear();
			output = lama.complete([&streamed](const std::string& token) {
				streamed += token;
			});

			for (int retry = 0; retry < 3; retry++) {
				auto& content = output["message"]["content"];
				bool contentEmpty = !output["message"].contains("content")
									|| content.is_null()
									|| (content.is_string() && content.get<std::string>().empty());
				bool empty = contentEmpty && !output["message"].contains("tool_calls");
				if (!empty) break;
				std::cout << color::yellow << color::bold << "  [RETRY] " << color::reset
						  << color::dim << "Empty response, waiting 1s and retrying..." << color::reset << std::endl;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				streamed.clear();
				output = lama.chat(std::string("continue"), [&streamed](const std::string& token) {
					streamed += token;
				});
			}
		}

		auto& final_content = output["message"]["content"];
		if (output["message"].contains("content")
			&& !final_content.is_null()
			&& final_content.is_string()
			&& !final_content.get<std::string>().empty()) {
			std::string text = final_content.get<std::string>();

			// Strip inline <think> tags as fallback (models that put thinking in content)
			size_t thinkEnd = text.find("</think>");
			if (thinkEnd != std::string::npos) {
				text = text.substr(thinkEnd + 9);
				size_t start = text.find_first_not_of(" \t\n\r");
				if (start != std::string::npos) {
					text = text.substr(start);
				}
			}

			std::cout << color::cyan << color::bold << "[JARVIS] " << color::reset << std::endl;
			termmark::renderMarkdown(text);
		}
		std::cout << std::endl;

		sessionMgr.saveSession(currentSessionId, lama.getMessages());
	}
}
