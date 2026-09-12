#pragma once
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace route
{
	inline void ok(httplib::Response& res, const std::string& body)
	{
		res.set_content(body, "application/json");
	}

	inline void err(httplib::Response& res, int status, const std::string& msg)
	{
		res.status = status;
		res.set_content(nlohmann::json{{"error", msg}}.dump(), "application/json");
	}

	inline void logErr(const std::string& ctx, const std::exception& e)
	{
		std::cerr << "[error] " << ctx << ": " << e.what() << "\n";
	}

	// Split on ';', trim whitespace, skip empty tokens.
	inline std::vector<std::string> splitSemicolon(const std::string& s)
	{
		std::vector<std::string> parts;
		std::stringstream ss(s);
		std::string token;
		while (std::getline(ss, token, ';'))
		{
			auto b = token.find_first_not_of(" \t");
			auto e = token.find_last_not_of(" \t");
			if (b != std::string::npos) parts.push_back(token.substr(b, e - b + 1));
		}
		return parts;
	}

	// Split on ',', trim whitespace, skip empty tokens — used for CSV-style
	// multi-value params like the Library page's multi-select library_ids.
	inline std::vector<std::string> splitComma(const std::string& s)
	{
		std::vector<std::string> parts;
		std::stringstream ss(s);
		std::string token;
		while (std::getline(ss, token, ','))
		{
			auto b = token.find_first_not_of(" \t");
			auto e = token.find_last_not_of(" \t");
			if (b != std::string::npos) parts.push_back(token.substr(b, e - b + 1));
		}
		return parts;
	}

	// Append "AND col = ?" or "AND col IN (?,?,...)" to extras; returns true if non-empty.
	inline bool appendInClause(const std::string& col, const std::string& raw,
							   std::string& extras, std::vector<std::string>& vals)
	{
		auto parts = splitSemicolon(raw);
		if (parts.empty()) return false;
		if (parts.size() == 1)
		{
			extras += " AND " + col + " = ?";
		}
		else
		{
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
								   std::string& extras, std::vector<std::string>& vals)
	{
		auto parts = splitSemicolon(raw);
		if (parts.empty()) return false;
		if (parts.size() == 1)
		{
			extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
				" WHERE json_each.value = ?)";
		}
		else
		{
			std::string ph;
			for (size_t i = 0; i < parts.size(); ++i) ph += (i ? ",?" : "?");
			extras += " AND EXISTS (SELECT 1 FROM json_each(" + tbl + "." + col + ")"
				" WHERE json_each.value IN (" + ph + "))";
		}
		for (auto& p : parts) vals.push_back(p);
		return true;
	}

	// True when the caller explicitly asked to keep accumulated cursor/RNG state
	// across a mutation that would otherwise hard-reset it (Hades' save-time
	// "keep positions" choice) -- see ScheduleCache::hardReset.
	inline bool wantsPreserveCursor(const httplib::Request& req)
	{
		return req.has_param("preserve_cursor") && req.get_param_value("preserve_cursor") == "true";
	}

	inline std::string urlEncode(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() * 3);
		static const char* hex = "0123456789ABCDEF";
		for (unsigned char c : s)
		{
			if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			{
				out += static_cast<char>(c);
			}
			else
			{
				out += '%';
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
		}
		return out;
	}
} // namespace route