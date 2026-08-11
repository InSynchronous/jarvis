#pragma once

#include <curl/curl.h>
#include <string>
#include <functional>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using StreamCallback = std::function<void(const std::string&)>;

class Ollama {
	private:
		std::string url;
		std::string model;
		std::string api_key;
		bool openai_compat = false;
		int max_tokens = 8192;
		int max_context_tokens = 100000;
		json messages;
		CURL* curl = nullptr;
		json tools = json::array();
		json last_tool_calls = json::array();
		std::chrono::steady_clock::time_point last_request_time{};
		int rate_limit_delay_ms = 2000;
		bool summarizing = false;
		bool enable_summarization = true;
		bool enable_prompt_caching = true;

		json doRequest(StreamCallback on_token, StreamCallback on_reasoning = nullptr);
		void trimContext();
		void summarizeContext(int from_index, int to_index);

		std::function<bool()> interrupt_cb;

	public:
		Ollama(std::string url, std::string model, std::string api_key = "", std::string system_prompt_path = "");
		json chat(std::string prompt, StreamCallback on_token = nullptr, StreamCallback on_reasoning = nullptr);
		json chat(json message, StreamCallback on_token = nullptr, StreamCallback on_reasoning = nullptr);

		void addMessage(json message);
		json complete(StreamCallback on_token = nullptr, StreamCallback on_reasoning = nullptr);

	json getMessages();
	void setMessages(json msgs);

	void addTool(json mcp_tool);
	bool hasTool(const std::string& name) const;
	std::vector<std::string> toolNames() const;
	void setMode(const std::string& system_prompt_path);
		void setModel(const std::string& newModel);
		void setRateLimitDelay(int ms);
		void setInterruptCallback(std::function<bool()> cb);
		void setMaxContextTokens(int tokens);
		void setSummarizationEnabled(bool enabled);
		void setPromptCachingEnabled(bool enabled);
};
