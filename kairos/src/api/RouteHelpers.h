#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace route {

inline void ok(httplib::Response& res, const std::string& body) {
	res.set_content(body, "application/json");
}

inline void err(httplib::Response& res, int status, const std::string& msg) {
	res.status = status;
	res.set_content(nlohmann::json{{"error", msg}}.dump(), "application/json");
}

inline void logErr(const std::string& ctx, const std::exception& e) {
	std::cerr << "[error] " << ctx << ": " << e.what() << "\n";
}

// Split on ';', trim whitespace, skip empty tokens.
inline std::vector<std::string> splitSemicolon(const std::string& s) {
	std::vector<std::string> parts;
	std::stringstream ss(s);
	std::string token;
	while (std::getline(ss, token, ';')) {
		auto b = token.find_first_not_of(" \t");
		auto e = token.find_last_not_of(" \t");
		if (b != std::string::npos) parts.push_back(token.substr(b, e - b + 1));
	}
	return parts;
}

// Append "AND col = ?" or "AND col IN (?,?,...)" to extras; returns true if non-empty.
inline bool appendInClause(const std::string& col, const std::string& raw,
                            std::string& extras, std::vector<std::string>& vals) {
	auto parts = splitSemicolon(raw);
	if (parts.empty()) return false;
	if (parts.size() == 1) {
		extras += " AND " + col + " = ?";
	} else {
		std::string ph;
		for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
		extras += " AND " + col + " IN (" + ph + ")";
	}
	for (auto& p : parts) vals.push_back(p);
	return true;
}

// Append an EXISTS(json_each IN ...) clause for a JSON-array column.
inline bool appendJsonInClause(const std::string& tbl, const std::string& col,
                                const std::string& raw,
                                std::string& extras, std::vector<std::string>& vals) {
	auto parts = splitSemicolon(raw);
	if (parts.empty()) return false;
	if (parts.size() == 1) {
		extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
		          " WHERE json_each.value = ?)";
	} else {
		std::string ph;
		for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
		extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
		          " WHERE json_each.value IN (" + ph + "))";
	}
	for (auto& p : parts) vals.push_back(p);
	return true;
}

} // namespace route
