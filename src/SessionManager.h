#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct SessionInfo {
	std::string id;
	std::string path;
	std::string updated_at;
	size_t message_count;
};

class SessionManager {
	private:
		std::string sessions_dir;

	public:
		SessionManager(const std::string& project_root);

		std::string getSessionsDir() const;
		std::string generateSessionId() const;

		void saveSession(const std::string& id, const json& messages);
		json loadSession(const std::string& id);

		std::vector<SessionInfo> listSessions();
		std::string getLastSessionId();
};
