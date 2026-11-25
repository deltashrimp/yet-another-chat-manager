#pragma once

#include "telegram/client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace spdlog {
class logger;
}  // namespace spdlog

namespace yacm::tg {

struct Outgoing {
    int64_t chat_id = 0;
    std::string text;
    std::string parse_mode;  // "HTML" or empty
    std::optional<int64_t> reply_to_message_id;
};

// Single-threaded outbound queue. Enforces Telegram send limits
// (default: 30 msg/s globally, 20 msg/min per group chat) and retries
// flood waits ("retry_after").
class Sender {
public:
    Sender(TelegramClient& client, std::shared_ptr<spdlog::logger> log);
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    void start();
    // Drains remaining messages, then stops the worker thread.
    void shutdown();

    void enqueue(Outgoing message);

    void set_limits(int messages_per_sec, int messages_per_chat_min);

private:
    void loop();
    void record_send(int64_t chat_id);
    // Blocks until a send slot is free; false when shutdown interrupted it.
    bool wait_for_slot(int64_t chat_id);
    // Sleeps for `d` unless shutdown is requested; false when interrupted.
    bool sleep_unless_stopped(std::chrono::steady_clock::duration d);
    void send_with_retry(const Outgoing& message);

    TelegramClient& client_;
    std::shared_ptr<spdlog::logger> log_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Outgoing> queue_;
    bool stop_ = false;
    std::thread worker_;

    int global_per_sec_ = 30;
    int chat_per_min_ = 20;

    std::deque<std::chrono::steady_clock::time_point> global_sends_;
    std::unordered_map<int64_t, std::deque<std::chrono::steady_clock::time_point>> chat_sends_;
};

}  // namespace yacm::tg