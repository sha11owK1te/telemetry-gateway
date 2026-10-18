#include "gateway.hpp"
#include <algorithm>
#include <stdexcept>

int64_t wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
uint32_t tick_ms() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

namespace {
class Statement {
public:
    sqlite3_stmt* value = nullptr;
    sqlite3* db;
    Statement(sqlite3* database, const char* sql) : db(database) {
        if (sqlite3_prepare_v2(db, sql, -1, &value, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
    }
    ~Statement() { sqlite3_finalize(value); }
    bool row() {
        int rc = sqlite3_step(value);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        throw std::runtime_error(sqlite3_errmsg(db));
    }
};
void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
}
Json reading_json(sqlite3_stmt* s) {
    return {{"id", sqlite3_column_int64(s, 0)}, {"device", sqlite3_column_int(s, 1)},
        {"sequence", sqlite3_column_int64(s, 2)}, {"temperature", sqlite3_column_double(s, 3)},
        {"humidity", sqlite3_column_double(s, 4)}, {"received_ms", sqlite3_column_int64(s, 5)}};
}
}

Store::Store(const std::string& path) {
    if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                         SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        std::string error = db_ ? sqlite3_errmsg(db_) : "database allocation failed";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(error);
    }
    try {
        sqlite3_busy_timeout(db_, 2000);
        exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
                  "CREATE TABLE IF NOT EXISTS readings("
                  "id INTEGER PRIMARY KEY, device INTEGER NOT NULL, sequence INTEGER NOT NULL,"
                  "temperature REAL NOT NULL, humidity REAL NOT NULL, received_ms INTEGER NOT NULL);"
                  "CREATE INDEX IF NOT EXISTS readings_device_id ON readings(device,id);");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Store::~Store() { if (db_) sqlite3_close(db_); }
void Store::append(const Reading& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement s(db_, "INSERT INTO readings(device,sequence,temperature,humidity,received_ms) VALUES(?,?,?,?,?)");
    sqlite3_bind_int(s.value, 1, r.device);
    sqlite3_bind_int64(s.value, 2, r.sequence);
    sqlite3_bind_double(s.value, 3, r.temperature);
    sqlite3_bind_double(s.value, 4, r.humidity);
    sqlite3_bind_int64(s.value, 5, r.received_ms);
    (void)s.row();
}
Json Store::devices() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement s(db_, "SELECT r.id,r.device,r.sequence,r.temperature,r.humidity,r.received_ms "
                     "FROM readings r JOIN (SELECT MAX(id) AS id FROM readings GROUP BY device) d "
                     "ON r.id=d.id ORDER BY r.device");
    Json result = Json::array();
    while (s.row()) {
        Json item = reading_json(s.value);
        item["online"] = wall_ms() - item["received_ms"].get<int64_t>() < 5000;
        result.push_back(std::move(item));
    }
    return result;
}
Json Store::history(int device, int limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement s(db_, "SELECT * FROM (SELECT id,device,sequence,temperature,humidity,received_ms "
                     "FROM readings WHERE device=? ORDER BY id DESC LIMIT ?) ORDER BY id");
    sqlite3_bind_int(s.value, 1, device);
    sqlite3_bind_int(s.value, 2, limit);
    Json result = Json::array();
    while (s.row()) result.push_back(reading_json(s.value));
    return result;
}
int64_t Store::count() {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement s(db_, "SELECT COUNT(*) FROM readings");
    s.row();
    return sqlite3_column_int64(s.value, 0);
}

Gateway::Gateway(const std::string& db, uint32_t timeout_ms, size_t capacity, unsigned delay)
    : store_(db), timeout_ms_(timeout_ms), capacity_(capacity), write_delay_ms_(delay) {
    if (capacity == 0 || timeout_ms == 0 || timeout_ms > INT32_MAX)
        throw std::invalid_argument("invalid queue capacity or timeout");
    event("info", "Gateway started; waiting for telemetry");
    worker_ = std::thread(&Gateway::run, this);
}
Gateway::~Gateway() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    changed_.notify_one();
    if (worker_.joinable()) worker_.join();
}
void Gateway::event(const std::string& level, const std::string& message) {
    if (events_.size() == 50) events_.pop_front();
    events_.push_back({{"level", level}, {"message", message}, {"time_ms", wall_ms()}});
}
void Gateway::expire() {
    const uint32_t now = tick_ms();
    for (auto& entry : sources_) {
        size_t dropped = sf_tick(&entry.second, now);
        if (dropped) event("warning", "Partial frame expired: " + entry.first + " (" + std::to_string(dropped) + " bytes)");
    }
}
void Gateway::decoded(const uint8_t* bytes, size_t length, void* self) {
    static_cast<Gateway*>(self)->accept(bytes, length);
}
void Gateway::accept(const uint8_t* p, size_t length) {
    // Payload v1: version:u8, device:u16, sequence:u32, temp:i16, humidity:u16, all LE.
    if (length != 11 || p[0] != 1) {
        ++invalid_; event("warning", "Unsupported telemetry payload"); return;
    }
    const int device = p[1] | (static_cast<int>(p[2]) << 8);
    const uint32_t seq = static_cast<uint32_t>(p[3]) | (static_cast<uint32_t>(p[4]) << 8) |
                         (static_cast<uint32_t>(p[5]) << 16) | (static_cast<uint32_t>(p[6]) << 24);
    int raw_temp = p[7] | (static_cast<int>(p[8]) << 8);
    if (raw_temp >= 32768) raw_temp -= 65536;
    const int humidity = p[9] | (static_cast<int>(p[10]) << 8);
    if (device < 1 || device > 64 || raw_temp < -5000 || raw_temp > 15000 || humidity > 10000) {
        ++invalid_; event("warning", "Telemetry value outside schema bounds"); return;
    }
    if (queue_.size() >= capacity_) {
        ++dropped_; event("warning", "Storage queue full; newest reading dropped"); return;
    }
    queue_.push_back({device, seq, raw_temp / 100.0, humidity / 100.0, wall_ms()});
    ++accepted_;
    high_water_ = std::max(high_water_, queue_.size());
    changed_.notify_one();
}
Json Gateway::ingest(const std::string& source, const std::string& bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(source);
    if (it == sources_.end()) {
        if (sources_.size() >= 32) throw std::length_error("Maximum 32 input sources per process");
        sf_parser parser;
        sf_init(&parser, timeout_ms_);
        it = sources_.emplace(source, parser).first;
    }
    expire();
    const auto before = accepted_, dropped = dropped_, invalid = invalid_;
    const auto errors = it->second.crc_errors;
    size_t frames = sf_feed(&it->second, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(),
                            tick_ms(), &Gateway::decoded, this);
    bytes_ += bytes.size();
    if (it->second.crc_errors != errors) event("warning", "CRC mismatch: " + source);
    return {{"frames", frames}, {"accepted", accepted_ - before}, {"dropped", dropped_ - dropped},
            {"invalid_payloads", invalid_ - invalid}, {"pending", it->second.pending}};
}
Json Gateway::stats() {
    Json result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t frames = 0, crc = 0, discarded = 0, timeouts = 0, pending = 0;
        for (const auto& entry : sources_) {
            frames += entry.second.frames; crc += entry.second.crc_errors;
            discarded += entry.second.discarded; timeouts += entry.second.timeouts;
            pending += entry.second.pending;
        }
        result = {{"received_bytes", bytes_}, {"frames", frames}, {"crc_errors", crc},
            {"discarded_bytes", discarded}, {"timeouts", timeouts}, {"pending_bytes", pending},
            {"accepted", accepted_}, {"stored", stored_}, {"queue_dropped", dropped_},
            {"invalid_payloads", invalid_}, {"storage_errors", storage_errors_},
            {"queue_depth", queue_.size()}, {"queue_capacity", capacity_},
            {"queue_high_water", high_water_}, {"sources", sources_.size()},
            {"uptime_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started_).count()}, {"events", events_}};
    }
    result["database_rows"] = store_.count();
    result["server_time_ms"] = wall_ms();
    return result;
}
void Gateway::run() {
    for (;;) {
        Reading next{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            changed_.wait_for(lock, std::chrono::milliseconds(25), [this] { return stopping_ || !queue_.empty(); });
            expire();
            if (queue_.empty()) { if (stopping_) return; else continue; }
            next = queue_.front(); queue_.pop_front();
        }
        // Optional delay is only a deterministic test aid for queue pressure.
        if (write_delay_ms_) std::this_thread::sleep_for(std::chrono::milliseconds(write_delay_ms_));
        try {
            store_.append(next);
            std::lock_guard<std::mutex> lock(mutex_); ++stored_;
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++storage_errors_; event("error", std::string("Storage failure: ") + error.what());
        }
    }
}
