#pragma once

#include "config/config.hpp"
#include "services/history.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace spdlog {
class logger;
}

// AI assistant: a C++ port of the Java ai-service, built into the bot
// instead of running as a separate HTTP service. Config comes from .env
// (see config::AiConfig); rules/bio accept literal text or "file:/path".
namespace yacm::ai {

struct ChatMessage {
    std::string role;  // "system" | "user" | "assistant"
    std::string content;
};

// A model reply plus the token usage the provider reported (-1 = unknown).
struct Completion {
    std::string text;
    int64_t prompt_tokens = -1;
    int64_t completion_tokens = -1;
};

struct AiError : std::runtime_error {
    long status = 0;  // HTTP status, 0 for transport errors
    AiError(long s, const std::string& message) : std::runtime_error(message), status(s) {}
};

// Seam for the LLM provider (tests inject a fake).
class AiClient {
public:
    virtual ~AiClient() = default;
    // Returns the assistant's reply. Throws AiError.
    virtual Completion complete(const std::vector<ChatMessage>& messages) = 0;
    // Aborts an in-flight request (shutdown). Default: no-op.
    virtual void cancel() {}
};

// OpenAI-compatible /chat/completions client over libcurl (Groq,
// OpenRouter, OpenAI, llama.cpp, Ollama...).
class OpenAiClient final : public AiClient {
public:
    explicit OpenAiClient(config::AiConfig config);

    Completion complete(const std::vector<ChatMessage>& messages) override;
    void cancel() override { cancelled_.store(true); }

    static nlohmann::json build_request(const config::AiConfig& config,
                                        const std::vector<ChatMessage>& messages);
    // Extracts choices[0].message.content and usage; throws AiError on API
    // errors.
    static Completion parse_response(long http_status, const std::string& body);

private:
    config::AiConfig config_;
    std::atomic<bool> cancelled_{false};
};

// AI_RULES / AI_BIO value: the contents of a file for "file:/path" or a
// plain path ("./resources/x.md", "/abs/x.txt"; relative to the working
// directory), otherwise the literal text. An unreadable file logs an error
// and yields "" (the path is never sent to the model as text).
std::string value_or_file(const std::string& value);

// True for an empty reply or the model's "none" (it chose not to answer).
bool is_silent(const std::string& reply);

// Splits text into Telegram-sized chunks (<= max_bytes, UTF-8 safe,
// preferring to cut at newlines, then spaces).
std::vector<std::string> split_message(const std::string& text, std::size_t max_bytes = 4000);

// What the assistant is asked, with its Telegram context.
struct Request {
    std::string chat_title;     // empty in private chats
    std::string user_name;      // "@username" of the asker (display name without one)
    std::string text;           // the question / message
    // Recent chat messages before the request, oldest first.
    std::vector<services::HistoryEntry> history;
    std::string replied_text;   // text of the message being replied to, if any
    bool replied_is_bot = false;  // the reply is to one of the bot's own messages
};

// Builds prompts and talks to the client. Thread-safe.
class Assistant {
public:
    Assistant(config::AiConfig config, AiClient& client);

    // Re-reads rules/bio (from files when "file:" is used).
    void reload();

    std::string system_prompt() const;
    std::vector<ChatMessage> build_messages(const Request& request) const;

    // Completion.text is empty when the model chose not to answer
    // ("none" / blank). Throws AiError on provider failures.
    Completion ask(const Request& request);

    // Longest accepted text per field (protects the token budget).
    static constexpr std::size_t kMaxInputBytes = 4000;

private:
    config::AiConfig config_;
    AiClient& client_;
    mutable std::mutex mu_;
    std::string rules_;
    std::string bio_;
};

// Per-user cooldown + per-chat requests-per-minute limit. Used from the
// polling thread only.
class RateLimiter {
public:
    RateLimiter(int user_cooldown_s, int chat_per_min);

    // Records and allows the request, or returns false when over a limit.
    bool allow(int64_t chat_id, int64_t user_id, int64_t now);

private:
    int user_cooldown_s_;
    int chat_per_min_;
    std::unordered_map<int64_t, int64_t> last_user_request_;
    std::unordered_map<int64_t, std::deque<int64_t>> chat_requests_;
};

// Runs AI requests on a background thread so slow LLM calls never block
// the polling loop (moderation stays responsive).
class Worker {
public:
    struct Job {
        int64_t chat_id = 0;
        int64_t reply_to_message_id = 0;
        std::string lang;
        bool explicit_request = false;  // /ai command (vs. mention / DM)
        Request request;
    };
    struct Result {
        std::optional<std::string> answer;  // nullopt: model stayed silent
        bool failed = false;
        int64_t prompt_tokens = -1;      // -1 = not reported by the provider
        int64_t completion_tokens = -1;
        int64_t elapsed_ms = 0;          // wall time of the provider call
    };
    using Deliver = std::function<void(const Job&, const Result&)>;

    Worker(Assistant& assistant, AiClient& client, Deliver deliver, std::size_t queue_limit,
           std::shared_ptr<spdlog::logger> log);
    ~Worker();

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void start();
    // Drops pending jobs, aborts the in-flight request and joins.
    void shutdown();

    // false when the queue is full (caller answers "busy").
    bool submit(Job job);

private:
    void loop();

    Assistant& assistant_;
    AiClient& client_;
    Deliver deliver_;
    std::size_t queue_limit_;
    std::shared_ptr<spdlog::logger> log_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace yacm::ai
