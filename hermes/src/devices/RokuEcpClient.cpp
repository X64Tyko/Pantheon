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
} // namespace

bool RokuEcpClient::launch(const std::string& ip_address, const std::string& app_id,
                           const std::map<std::string, std::string>& params) {
    std::string qs;
    for (auto& [k, v] : params)
        qs += (qs.empty() ? "?" : "&") + urlEncode(k) + "=" + urlEncode(v);

    httplib::Client cli("http://" + ip_address + ":8060");
    cli.set_connection_timeout(3);
    cli.set_read_timeout(5);
    auto res = cli.Post("/launch/" + app_id + qs, "", "text/plain");
    if (!res) {
        std::cerr << "[roku-ecp] launch " << ip_address << " app=" << app_id << " unreachable\n";
        return false;
    }
    return true;
}
