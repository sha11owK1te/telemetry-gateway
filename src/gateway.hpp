#pragma once
#include "json.hpp"
#include "serial-frame/serial_frame.h"
#include "sqlite3.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

using Json = nlohmann::json;
int64_t wall_ms();
uint32_t tick_ms();

struct Reading {
    int device;
    uint32_t sequence;
    double temperature;
    double humidity;
    int64_t received_ms;
};

class Store {
public:
    explicit Store(const std::string& path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    void append(const Reading& reading);
    Json devices();
    Json history(int device, int limit);
    int64_t count();
private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
};

class Gateway {
public:
    Gateway(const std::string& db, uint32_t timeout_ms, size_t capacity,
            unsigned write_delay_ms = 0);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;
    Json ingest(const std::string& source, const std::string& bytes);
    Json stats();
    Json devices() { return store_.devices(); }
    Json history(int device, int limit) { return store_.history(device, limit); }
private:
    static void decoded(const uint8_t* bytes, size_t length, void* self);
    void accept(const uint8_t* bytes, size_t length);
    void expire(); // mutex_ must be held
    void event(const std::string& level, const std::string& message);
    void run();
    Store store_;
    const uint32_t timeout_ms_;
    const size_t capacity_;
    const unsigned write_delay_ms_;
    const std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    std::map<std::string, sf_parser> sources_;
    std::deque<Reading> queue_;
    std::deque<Json> events_;
    uint64_t bytes_ = 0, accepted_ = 0, stored_ = 0, dropped_ = 0;
    uint64_t invalid_ = 0, storage_errors_ = 0;
    size_t high_water_ = 0;
    std::thread worker_;
};
