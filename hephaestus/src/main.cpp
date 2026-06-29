#include <httplib.h>
#include <nlohmann/json.hpp>
#include <iostream>

using json = nlohmann::json;

int main(int argc, char* argv[]) {
    int port = 8082;
    for (int i = 1; i < argc - 1; ++i) {
        if (std::string(argv[i]) == "--port")
            port = std::stoi(argv[i + 1]);
    }

    httplib::Server svr;

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });

    std::cout << "[hephaestus] listening on port " << port << std::endl;
    svr.listen("0.0.0.0", port);
    return 0;
}
