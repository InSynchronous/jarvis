#include "SessionManager.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>

namespace fs = std::filesystem;

SessionManager::SessionManager(const std::string& project_root)
	: sessions_dir(project_root + "/.jarvis_sessions")
{
	fs::create_directories(sessions_dir);
}

std::string SessionManager::getSessionsDir() const
{
	return sessions_dir;
}

std::string SessionManager::generateSessionId() const
{
	auto now = std::time(nullptr);
	auto* t = std::localtime(&now);
	char buf[32];
	std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H%M%S", t);
	return std::string(buf);
}

void SessionManager::saveSession(const std::string& id, const json& messages)
{
	std::string path = sessions_dir + "/" + id + ".json";

	auto now = std::time(nullptr);
	auto* t = std::localtime(&now);
	char time_buf[64];
	std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", t);

	json session;
	session["id"] = id;
	session["updated_at"] = time_buf;
	session["message_count"] = messages.size();
	session["messages"] = messages;

	// Preserve created_at if file already exists
	if (fs::exists(path)) {
		try {
			std::ifstream existing(path);
			json prev = json::parse(existing);
			if (prev.contains("created_at"))
				session["created_at"] = prev["created_at"];
		} catch (...) {
			session["created_at"] = time_buf;
		}
	} else {
		session["created_at"] = time_buf;
	}

	std::ofstream out(path);
	out << session.dump(2) << std::endl;
}

json SessionManager::loadSession(const std::string& id)
{
	std::string path = sessions_dir + "/" + id + ".json";
	std::ifstream file(path);
	if (!file.is_open()) return json();

	json session = json::parse(file);
	return session.value("messages", json::array());
}

std::vector<SessionInfo> SessionManager::listSessions()
{
	std::vector<SessionInfo> sessions;

	if (!fs::exists(sessions_dir))
		return sessions;

	for (auto& entry : fs::directory_iterator(sessions_dir)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension() != ".json") continue;

		try {
			std::ifstream file(entry.path());
			json session = json::parse(file);

			SessionInfo info;
			info.id = session.value("id", entry.path().stem().string());
			info.path = entry.path().string();
			info.updated_at = session.value("updated_at", "");
			info.message_count = session.value("message_count", (size_t)0);
			sessions.push_back(info);
		} catch (...) {
			continue;
		}
	}

	std::sort(sessions.begin(), sessions.end(), [](const SessionInfo& a, const SessionInfo& b) {
		return a.id > b.id;
	});

	return sessions;
}

std::string SessionManager::getLastSessionId()
{
	auto sessions = listSessions();
	if (sessions.empty()) return "";
	return sessions.front().id;
}
