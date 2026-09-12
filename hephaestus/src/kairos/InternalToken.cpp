#include "InternalToken.h"
#include <fstream>

namespace
{
	std::string trim(std::string s)
	{
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
		while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s = s.substr(1);
		return s;
	}
} // namespace

std::string readKairosInternalToken(const std::string& conf_path)
{
	std::ifstream f(conf_path);
	if (!f) return "";

	std::string line, section;
	while (std::getline(f, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line.front() == '[')
		{
			auto close = line.rfind(']');
			section    = (close != std::string::npos) ? line.substr(1, close - 1) : "";
			continue;
		}
		if (section != "_global") continue;
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		if (trim(line.substr(0, eq)) == "internal_token") return trim(line.substr(eq + 1));
	}
	return "";
}