#include "telegram/sender.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iterator>

namespace yacm::tg {

Sender::Sender(TelegramClient& client, std::shared_ptr<spdlog::logger> log)
    : client_(client), log_(std::move(log)) {}

Sender::~Sender() { shutdown(); }

void Sender::start() { worker_ = std::thread(&Sender::loop, this); }

void Sender::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void Sender::enqueue(Outgoing message) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(message));
    }
    cv_.notify_one();
}

void Sender::set_limits(int messages_per_sec, int messages_per_chat_min) {
    std::lock_guard<std::mutex> lk(mu_);
    global_per_sec_ = std::max(1, messages_per_sec);
    chat_per_min_ = std::max(1, messages_per_chat_min);
}

void Sender::loop() {
    for (;;) {
        std::optional<Outgoing> message;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (!queue_.empty()) {
                message = std::move(queue_.front());
                queue_.pop_front();
            } else if (stop_) {
                return;
            }
        }

        // On shutdown wait_for_slot() stops pacing and the retry sleeps
        // return at once, so the remaining queue is drained with a single
        // best-effort attempt per message (as documented in sender.hpp).
        wait_for_slot(message->chat_id);
        send_with_retry(*message);
    }
}

namespace {

void prune(std::deque<std::chrono::steady_clock::time_point>& times,
           std::chrono::steady_clock::time_point now,
           std::chrono::steady_clock::duration window) {
    while (!times.empty() && now - times.front() >= window) times.pop_front();
}

}  // namespace

bool Sender::wait_for_slot(int64_t chat_id) {
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (stop_) return false;
        const auto now = std::chrono::steady_clock::now();

        prune(global_sends_, now, std::chrono::seconds(1));
        if (static_cast<int>(global_sends_.size()) >= global_per_sec_) {
            const auto wait = global_sends_.front() + std::chrono::seconds(1) - now;
            cv_.wait_for(lk, wait, [this] { return stop_; });
            continue;
        }

        auto it = chat_sends_.find(chat_id);
        if (it != chat_sends_.end()) {
            prune(it->second, now, std::chrono::minutes(1));
            if (static_cast<int>(it->second.size()) >= chat_per_min_) {
                const auto wait = it->second.front() + std::chrono::minutes(1) - now;
                cv_.wait_for(lk, wait, [this] { return stop_; });
                continue;
            }
        }

        return true;
    }
}

void Sender::record_send(int64_t chat_id) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto now = std::chrono::steady_clock::now();
    prune(global_sends_, now, std::chrono::seconds(1));
    global_sends_.push_back(now);

    auto& chat_times = chat_sends_[chat_id];
    prune(chat_times, now, std::chrono::minutes(1));
    chat_times.push_back(now);

    // Drop idle chats so the map does not grow with every chat ever seen.
    for (auto it = chat_sends_.begin(); it != chat_sends_.end();) {
        prune(it->second, now, std::chrono::minutes(1));
        it = it->second.empty() ? chat_sends_.erase(it) : std::next(it);
    }
}

bool Sender::sleep_unless_stopped(std::chrono::steady_clock::duration d) {
    std::unique_lock<std::mutex> lk(mu_);
    return !cv_.wait_for(lk, d, [this] { return stop_; });
}

void Sender::send_with_retry(const Outgoing& message) {
    constexpr int kMaxAttempts = 10;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        try {
            client_.send_message(message.chat_id, message.text, message.parse_mode,
                                 message.reply_to_message_id);
            record_send(message.chat_id);
            return;
        } catch (const TelegramError& e) {
            if (e.is_flood()) {
                log_->warn("flood wait {}s, retrying message to chat {}", e.retry_after,
                           message.chat_id);
                if (!sleep_unless_stopped(std::chrono::seconds(e.retry_after + 1))) return;
                continue;
            }
            // Permanent-ish failures: log and drop so the queue doesn't stall.
            log_->error("failed to send message to chat {} (code {}): {}", message.chat_id,
                        e.error_code, e.what());
            return;
        } catch (const std::exception& e) {
            log_->error("send error for chat {}: {}", message.chat_id, e.what());
            if (!sleep_unless_stopped(std::chrono::milliseconds(500))) return;
        }
    }
    log_->error("giving up on message to chat {} after {} attempts", message.chat_id,
                kMaxAttempts);
}

}  // namespace yacm::tg