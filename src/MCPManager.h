#pragma once

#include "MCPConnection.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>
#include <map>

using json = nlohmann::json;

struct ServerConfig {
	std::string name;
	std::string type;
	std::string command;
	std::string url;
	std::string tokenFile;
	std::map<std::string, std::string> env;
	std::map<std::string, std::string> headers;
};

class MCPManager {
public:
	bool loadConfig(const std::string& configPath, const std::string& workspace = ".");
	bool connectAll();

	json listAllTools();
	json callTool(const std::string& prefixedName, const json& arguments);

	std::vector<std::string> serverNames() const;
	bool hasTool(const std::string& prefixedName) const;
	std::vector<std::string> toolNames() const;

private:
	std::vector<std::unique_ptr<MCPConnection>> connections;
	std::map<std::string, size_t> toolRouting;

	static std::string prefixTool(const std::string& serverName, const std::string& toolName);
	static std::string parseToolBaseName(const std::string& prefixedName);
	static std::string parseToolServerName(const std::string& prefixedName);
};
