#include "RokuEcpClient.h"
#include <httplib.h>
#include <iostream>

namespace {

std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

// ECP's /query/apps returns a tiny, fixed-shape XML document
// (<app id="...">Name</app> per installed channel) — not worth pulling in a
// full XML dependency for. This tolerantly scans for exactly that shape and
// gives up (returns what it has so far) on anything unexpected.
std::vector<RokuEcpClient::App> parseAppsXml(const std::string& xml) {
    std::vector<RokuEcpClient::App> apps;
    size_t pos = 0;
    while (true) {
        auto tagStart = xml.find("<app", pos);
        if (tagStart == std::string::npos) break;
        auto idAttr = xml.find("id=\"", tagStart);
        auto tagEnd = xml.find('>', tagStart);
        if (idAttr == std::string::npos || tagEnd == std::string::npos || idAttr > tagEnd) { pos = tagStart + 4; continue; }
        auto idStart = idAttr + 4;
        auto idEnd   = xml.find('"', idStart);
        if (idEnd == std::string::npos) break;

        auto nameEnd = xml.find("</app>", tagEnd);
        if (nameEnd == std::string::npos) break;

        RokuEcpClient::App app;
        app.id   = xml.substr(idStart, idEnd - idStart);
        app.name = xml.substr(tagEnd + 1, nameEnd - (tagEnd + 1));
        apps.push_back(std::move(app));
        pos = nameEnd + 6;
    }
    return apps;
}

} // namespace

std::vector<RokuEcpClient::App> RokuEcpClient::queryApps(const std::string& ip_address) {
    httplib::Client cli("http://" + ip_address + ":8060");
    cli.set_connection_timeout(3);
    cli.set_read_timeout(5);
    auto res = cli.Get("/query/apps");
    if (!res || res->status != 200) {
        std::cerr << "[roku-ecp] query/apps " << ip_address << " failed\n";
        return {};
    }
    return parseAppsXml(res->body);
}

bool RokuEcpClient::launch(const std::string& ip_address, const std::string& app_id,
                           const std::map<std::string, std::string>& params) {
    std::string path = "/launch/" + app_id;
    std::string qs;
    for (auto& [k, v] : params)
        qs += (qs.empty() ? "?" : "&") + urlEncode(k) + "=" + urlEncode(v);

    httplib::Client cli("http://" + ip_address + ":8060");
    cli.set_connection_timeout(3);
    cli.set_read_timeout(5);
    auto res = cli.Post(path + qs, "", "text/plain");
    if (!res) {
        std::cerr << "[roku-ecp] launch " << ip_address << path << " unreachable\n";
        return false;
    }
    return true;
}
