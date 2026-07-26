#include "Ollama.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>
using json = nlohmann::json;

static std::string sanitizeToolName(const std::string& name) {
	std::string result = name;
	size_t start = 0;
	while ((start = result.find("<|", start)) != std::string::npos) {
		size_t end = result.find("|>", start);
		if (end != std::string::npos)
			result.erase(start, end - start + 2);
		else
			break;
	}
	auto first = result.find_first_not_of(" \t\n\r");
	auto last = result.find_last_not_of(" \t\n\r");
	if (first == std::string::npos) return "";
	result = result.substr(first, last - first + 1);

	static const std::vector<std::string> suffixes = {
		"commentary", "Commentary", "COMMENTARY",
		"call", "Call", "CALL",
		"function", "Function", "FUNCTION",
		"tool", "Tool", "TOOL"
	};
	for (auto& suffix : suffixes) {
		if (result.size() > suffix.size() &&
			result.compare(result.size() - suffix.size(), suffix.size(), suffix) == 0) {
			result.erase(result.size() - suffix.size());
			break;
		}
	}

	return result;
}

static std::string readFileContents(const std::string& path) {
	std::ifstream file(path);
	if (!file.is_open()) return "";
	std::stringstream buf;
	buf << file.rdbuf();
	return buf.str();
}

static std::string loadPromptWithTools(const std::string& promptPath) {
	std::string content = readFileContents(promptPath);
	std::string tools = readFileContents("mcp/tools.md");

	if (!tools.empty()) {
		std::string marker = "{{TOOLS}}";
		size_t pos = content.find(marker);
		if (pos != std::string::npos)
			content.replace(pos, marker.size(), tools);
	}

	return content;
}

static void loadEnvFile(const std::string& path)
{
	std::ifstream file(path);
	if (!file.is_open()) return;

	std::string line;
	while (std::getline(file, line)) {
		if (line.empty() || line[0] == '#') continue;

		auto eq = line.find('=');
		if (eq == std::string::npos) continue;

		std::string key = line.substr(0, eq);
		std::string val = line.substr(eq + 1);

		while (!key.empty() && key.back() == ' ') key.pop_back();
		while (!val.empty() && val.front() == ' ') val.erase(0, 1);

		if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
			val = val.substr(1, val.size() - 2);
		else if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
			val = val.substr(1, val.size() - 2);

		setenv(key.c_str(), val.c_str(), 1);
	}
}

Ollama::Ollama(std::string url, std::string model, std::string api_key, std::string system_prompt_path)
	: url(url), model(model), api_key(api_key), curl(nullptr)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);

	openai_compat = (url.find("/v1/") != std::string::npos);

	loadEnvFile(".env");

	if (this->api_key.empty()) {
		const char* env_key = std::getenv("api_key");
		if (env_key) this->api_key = env_key;
	}

	std::string promptPath = system_prompt_path.empty() ? "mcp/code.md" : system_prompt_path;
	std::string systemPrompt = loadPromptWithTools(promptPath);
	if (systemPrompt.empty())
		systemPrompt = "Keep all answers as short as possible. Cut to the chase.";

	messages = json::array({
		{
			{"role", "system"},
			{"content", systemPrompt}
		}
	});
}


struct StreamState {
	std::string buffer;
	std::string full_content;
	std::string reasoning_content;
	json tool_calls = json::array();
	bool has_tool_calls = false;
	bool openai_compat = false;
	StreamCallback on_token;
};

static void parseOllamaLine(const std::string& line, StreamState* state)
{
	auto chunk = json::parse(line);

	if (chunk.contains("message")) {
		auto& msg = chunk["message"];

		if (msg.contains("content")) {
			std::string token = msg["content"].get<std::string>();
			if (!token.empty()) {
				state->full_content += token;
				if (state->on_token) state->on_token(token);
			}
		}

		if (msg.contains("tool_calls")) {
			state->has_tool_calls = true;
			state->tool_calls = msg["tool_calls"];
		}
	}
}

static void parseOpenAISSE(const std::string& line, StreamState* state)
{
	if (line == "[DONE]") return;

	std::string json_str = line;
	if (json_str.rfind("data: ", 0) == 0)
		json_str = json_str.substr(6);

	if (json_str.empty() || json_str == "{}") return;

	try {
		auto chunk = json::parse(json_str);
		if (chunk.contains("error")) {
			std::cerr << "\n[API ERROR] " << chunk["error"].dump(2) << std::endl;
			return;
		}
		if (!chunk.contains("choices") || chunk["choices"].empty()) return;

		auto& delta = chunk["choices"][0];

		if (delta.contains("delta")) {
			auto& d = delta["delta"];

			// reasoning/reasoning_content goes into reasoning_content for API context but NOT on_token (display)
			if (d.contains("reasoning_content") && !d["reasoning_content"].is_null()) {
				std::string token = d["reasoning_content"].get<std::string>();
				if (!token.empty()) state->reasoning_content += token;
			}
			if (d.contains("reasoning") && !d["reasoning"].is_null()) {
				std::string token = d["reasoning"].get<std::string>();
				if (!token.empty()) state->reasoning_content += token;
			}
			// content goes into full_content AND on_token (displayed to user)
			if (d.contains("content") && !d["content"].is_null()) {
				std::string token = d["content"].get<std::string>();
				if (!token.empty()) {
					state->full_content += token;
					if (state->on_token) state->on_token(token);
				}
			}

			if (d.contains("tool_calls") && !d["tool_calls"].is_null()) {
				state->has_tool_calls = true;

				for (auto& tc : d["tool_calls"]) {
					if (!tc.contains("index")) continue;
					int idx = tc["index"].get<int>();

					while ((int)state->tool_calls.size() <= idx)
						state->tool_calls.push_back(json::object());

					auto& entry = state->tool_calls[idx];

					if (tc.contains("id") && !tc["id"].is_null())
						entry["id"] = tc["id"].get<std::string>();

					if (tc.contains("type") && !tc["type"].is_null())
						entry["type"] = tc["type"].get<std::string>();

					if (tc.contains("function")) {
						auto& fn = tc["function"];
						if (fn.contains("name") && !fn["name"].is_null())
							entry["function"]["name"] = sanitizeToolName(fn["name"].get<std::string>());

						if (fn.contains("arguments") && !fn["arguments"].is_null()) {
							std::string cur = entry["function"].value("arguments", "");
							cur += fn["arguments"].get<std::string>();
							entry["function"]["arguments"] = cur;
						}
					}
				}
			}
		}
	} catch (...) {}
}

static size_t streamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
	StreamState* state = static_cast<StreamState*>(userp);
	size_t totalSize = size * nmemb;
	state->buffer.append(static_cast<char*>(contents), totalSize);

	size_t pos;
	while ((pos = state->buffer.find('\n')) != std::string::npos) {
		std::string line = state->buffer.substr(0, pos);
		state->buffer.erase(0, pos + 1);

		if (line.empty()) continue;

		try {
			if (state->openai_compat)
				parseOpenAISSE(line, state);
			else
				parseOllamaLine(line, state);
		} catch (...) {}
	}

	return totalSize;
}

static void processRemainingBuffer(StreamState* state)
{
	if (state->buffer.empty()) return;

	try {
		if (state->openai_compat)
			parseOpenAISSE(state->buffer, state);
		else
			parseOllamaLine(state->buffer, state);
	} catch (...) {}
}

static json buildResponse(StreamState& state)
{
	json response_msg;
	response_msg["role"] = "assistant";
	if (state.has_tool_calls && state.full_content.empty())
		response_msg["content"] = nullptr;
	else
		response_msg["content"] = state.full_content;

	// Send reasoning back in reasoning_content for API context (DeepSeek thinking mode)
	if (!state.reasoning_content.empty())
		response_msg["reasoning_content"] = state.reasoning_content;

	if (state.has_tool_calls) {
		json formatted_calls = json::array();

		for (auto& tc : state.tool_calls) {
			json call;
			call["type"] = "function";

			if (tc.contains("id"))
				call["id"] = tc["id"];

			if (tc.contains("function")) {
				auto& fn = tc["function"];
				call["function"]["name"] = sanitizeToolName(fn["name"].get<std::string>());

				if (fn.contains("arguments")) {
					std::string args_str = fn["arguments"].get<std::string>();
					if (json::accept(args_str))
						call["function"]["arguments"] = args_str;
					else
						call["function"]["arguments"] = "{}";
				} else {
					call["function"]["arguments"] = "{}";
				}
			}

			formatted_calls.push_back(call);
		}

		response_msg["tool_calls"] = formatted_calls;
	}

	json parsed_response;
	parsed_response["message"] = response_msg;
	return parsed_response;
}

json Ollama::doRequest(StreamCallback on_token)
{
	trimContext();

	auto now = std::chrono::steady_clock::now();
	auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_request_time).count();
	if (elapsed < rate_limit_delay_ms) {
		std::this_thread::sleep_for(std::chrono::milliseconds(rate_limit_delay_ms - elapsed));
	}

	int max_retries = 3;
	for (int attempt = 0; attempt <= max_retries; attempt++) {
		if (attempt > 0) {
			int backoff_ms = 1000 * (1 << (attempt - 1));
			std::cerr << "\n[RETRY] Rate limited, waiting " << backoff_ms << "ms before attempt " << (attempt + 1) << "/" << (max_retries + 1) << "..." << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
		}

		this->curl = curl_easy_init();
		if (!this->curl)
			return json();

		json request;
		request["model"] = model;
		request["max_tokens"] = max_tokens;
		request["stream"] = true;
		request["messages"] = messages;
		if (!tools.empty()) request["tools"] = tools;

		std::string j = request.dump();

		curl_easy_setopt(this->curl, CURLOPT_URL, this->url.c_str());
		curl_easy_setopt(this->curl, CURLOPT_POST, 1L);
		curl_easy_setopt(this->curl, CURLOPT_POSTFIELDS, j.c_str());

		curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		if (!api_key.empty()) {
			std::string auth = "Authorization: Bearer " + api_key;
			headers = curl_slist_append(headers, auth.c_str());
		}
		curl_easy_setopt(this->curl, CURLOPT_HTTPHEADER, headers);

		StreamState state;
		state.on_token = on_token;
		state.openai_compat = openai_compat;

		curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, streamWriteCallback);
		curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, &state);

		CURLcode result = curl_easy_perform(this->curl);

		long http_code = 0;
		curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &http_code);

		processRemainingBuffer(&state);
		curl_slist_free_all(headers);
		curl_easy_cleanup(this->curl);

		last_request_time = std::chrono::steady_clock::now();

		if (http_code == 429) {
			std::cerr << "\n[HTTP 429] Rate limited." << std::endl;
			if (attempt < max_retries) continue;
		}

		if (http_code >= 400) {
			std::cerr << "\n[HTTP " << http_code << "] Raw body: " << state.buffer << state.full_content << std::endl;
		}

		if (result != CURLE_OK)
			return json();

		json parsed_response = buildResponse(state);

		if (parsed_response.contains("message")) {
			messages.push_back(parsed_response["message"]);
		}

		std::cout << "\n\033[2m" << parsed_response.dump(2) << "\033[0m" << std::endl;
		return parsed_response;
	}

	return json();
}

json Ollama::chat(std::string prompt, StreamCallback on_token)
{
	messages.push_back({
		{"role", "user"},
		{"content", prompt}
	});

	return doRequest(on_token);
}

json Ollama::chat(json message, StreamCallback on_token)
{
	messages.push_back(message);

	return doRequest(on_token);
}

void Ollama::addMessage(json message)
{
	messages.push_back(message);
}

json Ollama::complete(StreamCallback on_token)
{
	return doRequest(on_token);
}

json Ollama::getMessages()
{
	return messages;
}

void Ollama::setMessages(json msgs)
{
	messages = msgs;
}

void Ollama::addTool(json mcp_tool)
{
	json ollama_tool;

	ollama_tool["type"] = "function";

	ollama_tool["function"]["name"] = mcp_tool["name"];
	ollama_tool["function"]["description"] = mcp_tool.value("description", "");

	if (mcp_tool.contains("input_schema")) {
		ollama_tool["function"]["parameters"] = mcp_tool["input_schema"];
	} else {
		ollama_tool["function"]["parameters"] = {
			{"type", "object"},
			{"properties", json::object()}
		};
	}

	this->tools.push_back(ollama_tool);
}

bool Ollama::hasTool(const std::string& name) const
{
	for (const auto& t : tools) {
		if (t["function"]["name"].get<std::string>() == name)
			return true;
	}
	return false;
}

std::vector<std::string> Ollama::toolNames() const
{
	std::vector<std::string> names;
	for (const auto& t : tools) {
		names.push_back(t["function"]["name"].get<std::string>());
	}
	return names;
}

void Ollama::setMode(const std::string& system_prompt_path)
{
	std::string systemPrompt = loadPromptWithTools(system_prompt_path);
	if (systemPrompt.empty())
		systemPrompt = "Keep all answers as short as possible. Cut to the chase.";

	messages[0]["content"] = systemPrompt;
}

void Ollama::setModel(const std::string& newModel)
{
	this->model = newModel;
}

void Ollama::setRateLimitDelay(int ms)
{
	this->rate_limit_delay_ms = ms;
}

void Ollama::setMaxContextTokens(int tokens)
{
	this->max_context_tokens = tokens;
}

void Ollama::setSummarizationEnabled(bool enabled)
{
	this->enable_summarization = enabled;
}

static int estimateTokens(const json& msg)
{
	int tokens = 0;
	if (msg.contains("content") && !msg["content"].is_null() && msg["content"].is_string())
		tokens += msg["content"].get<std::string>().size() / 4;
	if (msg.contains("role"))
		tokens += 4;
	if (msg.contains("tool_call_id"))
		tokens += 4;
	if (msg.contains("tool_calls")) {
		for (auto& tc : msg["tool_calls"]) {
			if (tc.contains("function")) {
				auto& fn = tc["function"];
				if (fn.contains("name"))
					tokens += fn["name"].get<std::string>().size() / 4 + 2;
				if (fn.contains("arguments"))
					tokens += fn["arguments"].get<std::string>().size() / 4;
			}
		}
	}
	return tokens + 4;
}

void Ollama::summarizeContext(int from_index, int to_index)
{
	if (!enable_summarization || summarizing || from_index >= to_index)
		return;

	summarizing = true;

	std::string summary_prompt = "Summarize the following conversation segment concisely. "
		"Preserve key decisions, code changes, tool results, and any pending tasks. "
		"Keep it under 2000 characters:\n\n";

	for (int i = from_index; i < to_index; i++) {
		const auto& msg = messages[i];
		std::string role = msg.contains("role") ? msg["role"].get<std::string>() : "unknown";
		std::string content = msg.contains("content") && msg["content"].is_string() 
			? msg["content"].get<std::string>() 
			: "";
		summary_prompt += "[" + role + "] " + content + "\n\n";
	}

	json old_messages = messages;
	messages = json::array({ old_messages[0] });
	messages.push_back({{"role", "user"}, {"content", summary_prompt}});

	json summary_resp;
	try {
		summary_resp = doRequest(nullptr);
	} catch (...) {
		summarizing = false;
		messages = old_messages;
		return;
	}

	summarizing = false;

	std::string summary_text;
	if (summary_resp.contains("message") && summary_resp["message"].contains("content")) {
		summary_text = summary_resp["message"]["content"].get<std::string>();
	} else {
		messages = old_messages;
		return;
	}

	messages = json::array({ old_messages[0] });
	messages.push_back({{"role", "system"}, {"content", "Context summary of earlier messages: " + summary_text}});

	for (size_t i = 1; i < old_messages.size(); i++) {
		if ((int)i < from_index || (int)i >= to_index) {
			messages.push_back(old_messages[i]);
		}
	}

	std::cerr << "\n[CONTEXT] Summarized " << (to_index - from_index)
			  << " messages into " << summary_text.size() << " chars" << std::endl;
}

void Ollama::trimContext()
{
	if (summarizing) return;

	int total = 0;
	for (auto& m : messages)
		total += estimateTokens(m);

	if (total <= max_context_tokens)
		return;

	int system_tokens = estimateTokens(messages[0]);
	int budget = max_context_tokens - system_tokens - 200;

	if (budget <= 0) return;

	std::vector<int> indices_to_trim;
	int freed = 0;
	for (int i = 2; i < (int)messages.size() - 2; i++) {
		freed += estimateTokens(messages[i]);
		indices_to_trim.push_back(i);
		if (freed >= total - max_context_tokens)
			break;
	}

	if (!indices_to_trim.empty() && freed > 500 && enable_summarization) {
		summarizeContext(indices_to_trim.front(), indices_to_trim.back() + 1);
		return;
	}

	if (!indices_to_trim.empty()) {
		messages.erase(messages.begin() + 2, messages.begin() + 2 + indices_to_trim.size());
		std::cerr << "\n[CONTEXT] Trimmed " << indices_to_trim.size()
				  << " old messages (" << freed << " tokens freed)" << std::endl;
	}
}
