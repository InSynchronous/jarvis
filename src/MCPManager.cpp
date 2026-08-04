#include "MCPManager.h"
#include "StdioTransport.h"
#include "HTTPTransport.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

bool MCPManager::loadConfig(const std::string& configPath, const std::string& workspace) {
	std::ifstream file(configPath);
	if (!file.is_open()) {
		std::cerr << "[MCPManager] Config file not found: " << configPath << "\n";
		return false;
	}

	json config;
	try {
		config = json::parse(file);
	} catch (const json::parse_error& e) {
		std::cerr << "[MCPManager] Failed to parse config: " << e.what() << "\n";
		return false;
	}

	if (!config.contains("servers") || !config["servers"].is_object()) {
		std::cerr << "[MCPManager] Config must have a 'servers' object\n";
		return false;
	}

	connections.clear();
	toolRouting.clear();

	for (auto& [name, serverJson] : config["servers"].items()) {
		ServerConfig sc;
		sc.name = name;

		if (!serverJson.contains("type")) {
			std::cerr << "[MCPManager] Server '" << name << "' missing 'type'\n";
			return false;
		}
		sc.type = serverJson["type"].get<std::string>();

		if (sc.type == "stdio") {
			if (!serverJson.contains("command")) {
				std::cerr << "[MCPManager] Stdio server '" << name << "' missing 'command'\n";
				return false;
			}
			sc.command = serverJson["command"].get<std::string>();

			if (serverJson.contains("env") && serverJson["env"].is_object()) {
				for (auto& [k, v] : serverJson["env"].items()) {
					std::string val = v.get<std::string>();
					if (val == "." || val == "./") {
						val = workspace;
					}
					sc.env[k] = val;
				}
			}
		} else if (sc.type == "http") {
			if (!serverJson.contains("url")) {
				std::cerr << "[MCPManager] HTTP server '" << name << "' missing 'url'\n";
				return false;
			}
			sc.url = serverJson["url"].get<std::string>();

			if (serverJson.contains("headers") && serverJson["headers"].is_object()) {
				for (auto& [k, v] : serverJson["headers"].items()) {
					sc.headers[k] = v.get<std::string>();
				}
			}

			if (serverJson.contains("tokenFile")) {
				sc.tokenFile = serverJson["tokenFile"].get<std::string>();
				std::ifstream tokenStream(sc.tokenFile);
				if (tokenStream.is_open()) {
					try {
						json tokenJson = json::parse(tokenStream);
						if (tokenJson.contains("access_token") &&
							tokenJson["access_token"].is_string()) {
							sc.headers["Authorization"] = "Bearer " +
								tokenJson["access_token"].get<std::string>();
						}
					} catch (const json::parse_error& e) {
						std::cerr << "[MCPManager] Failed to parse token file '"
								  << sc.tokenFile << "': " << e.what() << "\n";
					}
				} else {
					std::cerr << "[MCPManager] Token file not found: " << sc.tokenFile
							  << " (run: python3 mcp/google_auth.py login " << name << ")\n";
				}
			}
		} else {
			std::cerr << "[MCPManager] Server '" << name << "' has unknown type: " << sc.type << "\n";
			return false;
		}

		std::cerr << "[MCPManager] Registered server: " << name << " (" << sc.type << ")\n";

		std::unique_ptr<MCPConnection> conn;
		if (sc.type == "stdio") {
			auto transport = std::make_unique<StdioTransport>(sc.command, sc.env);
			conn = std::make_unique<MCPConnection>(sc.name, std::move(transport));
		} else {
			auto transport = std::make_unique<HTTPTransport>(sc.url, sc.headers);
			conn = std::make_unique<MCPConnection>(sc.name, std::move(transport));
		}

		connections.push_back(std::move(conn));
	}

	return true;
}

bool MCPManager::connectAll() {
	if (connections.empty()) {
		std::cerr << "[MCPManager] No servers configured\n";
		return false;
	}

	int connected = 0;
	for (auto& conn : connections) {
		std::cerr << "[MCPManager] Connecting to " << conn->getName() << "... ";
		if (conn->connect()) {
			std::cerr << "OK\n";
			connected++;
		} else {
			std::cerr << "FAILED\n";
		}
	}

	return connected > 0;
}

json MCPManager::listAllTools() {
	json allTools = json::array();
	toolRouting.clear();

	for (size_t i = 0; i < connections.size(); i++) {
		auto& conn = connections[i];
		if (!conn->is_connected()) continue;

		try {
			json serverTools = conn->listTools();
			for (auto& tool : serverTools) {
				std::string origName = tool["name"].get<std::string>();
				std::string prefixed = prefixTool(conn->getName(), origName);

				tool["name"] = prefixed;
				tool["original_name"] = origName;
				tool["server"] = conn->getName();

				std::string desc = tool.value("description", "");
				tool["description"] = "[" + conn->getName() + "] " + desc;

				toolRouting[prefixed] = i;
				allTools.push_back(std::move(tool));
			}
		} catch (const std::exception& e) {
			std::cerr << "[MCPManager] Failed to list tools from "
					  << conn->getName() << ": " << e.what() << "\n";
		}
	}

	return allTools;
}

json MCPManager::callTool(const std::string& prefixedName, const json& arguments) {
	auto it = toolRouting.find(prefixedName);
	if (it == toolRouting.end()) {
		json error;
		error["content"] = json::array({
			json({{"type", "text"}, {"text", "Error: Unknown tool '" + prefixedName + "'"}})
		});
		return error;
	}

	size_t connIdx = it->second;
	auto& conn = connections[connIdx];

	std::string origName = parseToolBaseName(prefixedName);

	try {
		return conn->callTool(origName, arguments);
	} catch (const std::exception& e) {
		json error;
		error["content"] = json::array({
			json({{"type", "text"}, {"text", "Error calling tool '" + prefixedName + "': " + e.what()}})
		});
		return error;
	}
}

std::vector<std::string> MCPManager::serverNames() const {
	std::vector<std::string> names;
	for (auto& conn : connections) {
		names.push_back(conn->getName());
	}
	return names;
}

bool MCPManager::hasTool(const std::string& prefixedName) const {
	return toolRouting.find(prefixedName) != toolRouting.end();
}

std::vector<std::string> MCPManager::toolNames() const {
	std::vector<std::string> names;
	for (auto& [name, _] : toolRouting) {
		names.push_back(name);
	}
	return names;
}

std::string MCPManager::prefixTool(const std::string& serverName, const std::string& toolName) {
	return serverName + "__" + toolName;
}

std::string MCPManager::parseToolBaseName(const std::string& prefixedName) {
	auto pos = prefixedName.find("__");
	if (pos == std::string::npos) return prefixedName;
	return prefixedName.substr(pos + 2);
}

std::string MCPManager::parseToolServerName(const std::string& prefixedName) {
	auto pos = prefixedName.find("__");
	if (pos == std::string::npos) return "";
	return prefixedName.substr(0, pos);
}
