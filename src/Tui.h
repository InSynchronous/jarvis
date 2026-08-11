#pragma once

#include <memory>
#include <string>
#include <functional>

// Full-screen terminal UI for jarvis, built on FTXUI.
//
// The UI runs on the thread that calls run(). All display methods are
// thread-safe and may be called from the agent worker thread; they post
// events to the FTXUI loop. Submitted prompts are drained via popPrompt().
class JarvisTui {
	public:
		JarvisTui();
		~JarvisTui();

		JarvisTui(const JarvisTui&) = delete;
		JarvisTui& operator=(const JarvisTui&) = delete;

		// Runs the FTXUI event loop until exitLoop() is called. Blocks.
		void run();
		// Exit the FTXUI loop. Thread-safe.
		void exitLoop();
		// Blocking read of the next submitted prompt. Returns false when the
		// TUI is shutting down.
		bool popPrompt(std::string& out);
		// Signal the prompt queue that no more prompts will be processed.
		void stop();

		// --- Display (thread-safe) ---

		void addUserMessage(const std::string& text);
		void addInfoMessage(const std::string& text);
		void addErrorMessage(const std::string& text);
		void addToolCall(const std::string& name);
		void addToolOutput(const std::string& text);
		void clearConversation();

		// Assistant streaming block: begin -> stream{Reasoning,Token}* -> end.
		void beginAssistant();
		void streamReasoning(const std::string& token);
		void streamToken(const std::string& token);
		// Ends the streaming block and replaces it with rendered markdown.
		void endAssistant(const std::string& markdown);

		// Status bar.
		void setModel(const std::string& model);
		void setMode(const std::string& mode);
		void setWorkspace(const std::string& workspace);
		void setServerInfo(const std::string& info);
		void setFastMode(bool on);
		void setThinking(bool on);
		void setListening(bool on);
		// Called when the user presses ESC. Used to interrupt a running agent.
		void setOnInterrupt(std::function<void()> cb);

	private:
		struct Impl;
		std::unique_ptr<Impl> impl_;
};
