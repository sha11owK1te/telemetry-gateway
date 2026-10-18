#include "gateway.hpp"
#include "httplib.h"
#include <csignal>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void signal_handler(int) { stop_requested = 1; }
int number(const std::string& value, int low, int high) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Expected an unsigned decimal integer");
    unsigned long n = std::stoul(value);
    if (n < static_cast<unsigned>(low) || n > static_cast<unsigned>(high))
        throw std::invalid_argument("Integer outside allowed range");
    return static_cast<int>(n);
}
void json_response(httplib::Response& response, const Json& value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}
std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open web asset: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
}

int main(int argc, char** argv) {
    try {
        int port = 8080, timeout = 500, capacity = 1024, delay = 0;
        std::string database = "telemetry.db", web = "web";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "gateway [--port 8080] [--db telemetry.db] [--web web] "
                             "[--timeout-ms 500] [--capacity 1024] [--write-delay-ms 0]\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::invalid_argument("Missing argument value");
            std::string value = argv[++i];
            if (arg == "--port") port = number(value, 1, 65535);
            else if (arg == "--db") database = value;
            else if (arg == "--web") web = value;
            else if (arg == "--timeout-ms") timeout = number(value, 10, 60000);
            else if (arg == "--capacity") capacity = number(value, 1, 100000);
            else if (arg == "--write-delay-ms") delay = number(value, 0, 1000);
            else throw std::invalid_argument("Unknown option: " + arg);
        }
        const auto html = read_file(web + "/index.html");
        const auto css = read_file(web + "/style.css");
        const auto js = read_file(web + "/app.js");
        Gateway gateway(database, static_cast<uint32_t>(timeout), static_cast<size_t>(capacity), static_cast<unsigned>(delay));
        httplib::Server server;
        server.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 64); };
        server.set_payload_max_length(4096);
        server.set_read_timeout(3, 0);
        server.set_write_timeout(3, 0);
        server.set_keep_alive_max_count(20);
        server.set_default_headers({{"X-Content-Type-Options", "nosniff"},
            {"Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'"}});
        server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr error) {
            try { if (error) std::rethrow_exception(error); }
            catch (const std::length_error& e) { json_response(res, {{"error", e.what()}}, 429); return; }
            catch (const std::invalid_argument& e) { json_response(res, {{"error", e.what()}}, 400); return; }
            catch (const std::out_of_range&) { json_response(res, {{"error", "Integer outside allowed range"}}, 400); return; }
            catch (const std::exception& e) { std::cerr << "request error: " << e.what() << '\n'; }
            json_response(res, {{"error", "Internal server error"}}, 500);
        });
        server.Get("/", [&](const httplib::Request&, httplib::Response& res) { res.set_content(html, "text/html; charset=utf-8"); });
        server.Get("/style.css", [&](const httplib::Request&, httplib::Response& res) { res.set_content(css, "text/css; charset=utf-8"); });
        server.Get("/app.js", [&](const httplib::Request&, httplib::Response& res) { res.set_content(js, "application/javascript; charset=utf-8"); });
        server.Get("/api/health", [&](const httplib::Request&, httplib::Response& res) {
            json_response(res, {{"status", "ok"}, {"service", "telemetry-gateway"}, {"version", "0.1.0"}});
        });
        server.Get("/api/stats", [&](const httplib::Request&, httplib::Response& res) { json_response(res, gateway.stats()); });
        server.Get("/api/devices", [&](const httplib::Request&, httplib::Response& res) { json_response(res, gateway.devices()); });
        server.Get("/api/history", [&](const httplib::Request& req, httplib::Response& res) {
            int device = number(req.get_param_value("device"), 1, 64);
            int limit = req.has_param("limit") ? number(req.get_param_value("limit"), 1, 5000) : 120;
            json_response(res, gateway.history(device, limit));
        });
        server.Get("/api/export.csv", [&](const httplib::Request& req, httplib::Response& res) {
            const int device = number(req.get_param_value("device"), 1, 64);
            const auto records = gateway.history(device, 5000);
            std::ostringstream csv;
            csv << "id,device,sequence,temperature,humidity,received_ms\r\n";
            for (const auto& r : records) {
                csv << r["id"] << ',' << r["device"] << ',' << r["sequence"] << ','
                    << r["temperature"] << ',' << r["humidity"] << ',' << r["received_ms"] << "\r\n";
            }
            res.set_header("Content-Disposition", "attachment; filename=telemetry.csv");
            res.set_content(csv.str(), "text/csv; charset=utf-8");
        });
        server.Post(R"(/api/ingest/([A-Za-z0-9_-]{1,32}))", [&](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value("Content-Type") != "application/octet-stream") {
                json_response(res, {{"error", "Content-Type must be application/octet-stream"}}, 415); return;
            }
            auto result = gateway.ingest(req.matches[1].str(), req.body);
            const int status = result["dropped"].get<uint64_t>() ? 429 : 202;
            json_response(res, result, status);
        });
        if (!server.bind_to_port("127.0.0.1", port)) throw std::runtime_error("Cannot bind localhost port " + std::to_string(port));
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::cout << "Telemetry Gateway: http://127.0.0.1:" << port
                  << "\nDatabase: " << database << "\nCtrl+C to stop (pending writes drain on graceful shutdown)." << std::endl;
        std::atomic<bool> listener_done{false};
        std::thread listener([&] { server.listen_after_bind(); listener_done = true; });
        while (!stop_requested && !listener_done.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        server.stop();
        listener.join();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "gateway: " << e.what() << '\n';
        return 1;
    }
}
