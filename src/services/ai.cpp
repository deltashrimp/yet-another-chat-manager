#include "services/ai.hpp"

#include "util/parsers.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace yacm::ai {
namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

int progress_callback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    // Non-zero aborts the transfer (CURLE_ABORTED_BY_CALLBACK).
    return static_cast<std::atomic<bool>*>(clientp)->load() ? 1 : 0;
}

// Cuts `s` to at most `max` bytes without splitting a UTF-8 sequence.
std::string utf8_truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end) + "…";
}

}  // namespace

// --- OpenAiClient ---------------------------------------------------------

OpenAiClient::OpenAiClient(config::AiConfig config) : config_(std::move(config)) {}

nlohmann::json OpenAiClient::build_request(const config::AiConfig& config,
                                           const std::vector<ChatMessage>& messages) {
    nlohmann::json msgs = nlohmann::json::array();
    for (const auto& m : messages) msgs.push_back({{"role", m.role}, {"content", m.content}});
    nlohmann::json body = {
        {"model", config.model},
        {"messages", msgs},
        {"max_tokens", config.max_tokens},
    };
    if (config.temperature) body["temperature"] = *config.temperature;
    if (config.top_p) body["top_p"] = *config.top_p;
    if (config.frequency_penalty) body["frequency_penalty"] = *config.frequency_penalty;
    if (config.presence_penalty) body["presence_penalty"] = *config.presence_penalty;
    return body;
}

Completion OpenAiClient::parse_response(long http_status, const std::string& body) {
    const auto json = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object()) {
        throw AiError(http_status, "invalid JSON from AI provider (HTTP " +
                                       std::to_string(http_status) + ")");
    }
    if (json.contains("error")) {
        const auto& e = json.at("error");
        std::string message = "AI provider error";
        if (e.is_object() && e.contains("message") && e["message"].is_string()) {
            message = e["message"].get<std::string>();
        } else if (e.is_string()) {
            message = e.get<std::string>();
        }
        throw AiError(http_status, message);
    }
    if (http_status < 200 || http_status >= 300) {
        throw AiError(http_status, "HTTP " + std::to_string(http_status));
    }
    const auto& choices = json.value("choices", nlohmann::json::array());
    if (!choices.is_array() || choices.empty()) throw AiError(http_status, "no choices in reply");
    const auto& message = choices.at(0).value("message", nlohmann::json::object());
    const auto& content = message.value("content", nlohmann::json());

    Completion out;
    if (content.is_string()) out.text = content.get<std::string>();
    const auto& usage = json.value("usage", nlohmann::json::object());
    if (usage.is_object()) {
        const auto count = [&usage](const char* key) -> int64_t {
            const auto it = usage.find(key);
            return it != usage.end() && it->is_number_integer() ? it->get<int64_t>() : -1;
        };
        out.prompt_tokens = count("prompt_tokens");
        out.completion_tokens = count("completion_tokens");
    }
    return out;
}

Completion OpenAiClient::complete(const std::vector<ChatMessage>& messages) {
    const std::string payload = build_request(config_, messages).dump();

    CURL* handle = curl_easy_init();
    if (!handle) throw AiError(0, "curl_easy_init failed");

    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + config_.api_key;
    headers = curl_slist_append(headers, auth.c_str());

    curl_easy_setopt(handle, CURLOPT_URL, config_.api_url.c_str());
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(config_.timeout_s));
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "yacm-ai/0.1.0");
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &cancelled_);

    const CURLcode rc = curl_easy_perform(handle);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    if (rc != CURLE_OK) throw AiError(0, std::string("curl error: ") + curl_easy_strerror(rc));
    return parse_response(status, response);
}

// --- helpers ----------------------------------------------------------------

namespace {

// A value written as a path: "./x", "../x", "/x", "~/x" or "*.md" / "*.txt".
bool looks_like_path(const std::string& v) {
    if (v.empty() || v.find('\n') != std::string::npos) return false;
    if (v.rfind("./", 0) == 0 || v.rfind("../", 0) == 0 || v.front() == '/' ||
        v.rfind("~/", 0) == 0) {
        return true;
    }
    const auto ends_with = [&v](const char* ext) {
        const std::string e(ext);
        return v.size() > e.size() && v.compare(v.size() - e.size(), e.size(), e) == 0;
    };
    return ends_with(".md") || ends_with(".txt");
}

std::optional<std::string> read_text_file(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

std::string value_or_file(const std::string& raw) {
    const std::string value = util::trim(raw);
    // Explicit "file:/path" (the Java service's syntax) or a plain path.
    const bool explicit_file = value.rfind("file:", 0) == 0;
    const std::string path = explicit_file ? util::trim(value.substr(5)) : value;
    if (!explicit_file && !looks_like_path(path)) return raw;  // literal text

    if (auto content = read_text_file(path)) return *content;

    std::error_code ec;
    spdlog::error("AI: cannot read file '{}' (resolved to '{}'): {}", path,
                  std::filesystem::absolute(path, ec).string(),
                  std::filesystem::exists(path, ec) ? "not a readable file" : "no such file");
    // Never send the path itself to the model as if it were the rules.
    return {};
}

bool is_silent(const std::string& reply) {
    std::string t = util::trim(reply);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!t.empty() && (t.back() == '.' || t.back() == '!')) t.pop_back();
    return t.empty() || t == "none";
}

std::vector<std::string> split_message(const std::string& text, std::size_t max_bytes) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text.size() - pos <= max_bytes) {
            parts.push_back(text.substr(pos));
            break;
        }
        std::size_t cut = pos + max_bytes;
        // never cut inside a UTF-8 sequence
        while (cut > pos && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
        // prefer a newline, then a space, in the second half of the chunk
        const std::size_t floor = pos + max_bytes / 2;
        std::size_t nl = text.rfind('\n', cut - 1);
        if (nl != std::string::npos && nl >= floor) {
            cut = nl + 1;
        } else {
            std::size_t sp = text.rfind(' ', cut - 1);
            if (sp != std::string::npos && sp >= floor) cut = sp + 1;
        }
        if (cut <= pos) cut = pos + max_bytes;  // pathological input
        parts.push_back(text.substr(pos, cut - pos));
        pos = cut;
    }
    return parts;
}

// --- Assistant ----------------------------------------------------------------

Assistant::Assistant(config::AiConfig config, AiClient& client)
    : config_(std::move(config)), client_(client) {
    reload();
}

void Assistant::reload() {
    std::string rules = value_or_file(config_.rules);
    std::string bio = value_or_file(config_.bio);
    spdlog::info("AI config loaded: model={}, rules={} chars, bio={} chars", config_.model,
                 rules.size(), bio.size());
    std::lock_guard<std::mutex> lk(mu_);
    rules_ = std::move(rules);
    bio_ = std::move(bio);
}

std::string Assistant::system_prompt() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string p =
        "You are a helpful assistant built into a Telegram group-management bot.\n"
        "Guidelines:\n"
        "- Reply in the language of the user's message.\n"
        "- Keep answers short and conversational; use plain text, no Markdown tables.\n"
        "- Never claim to have performed moderation actions; you cannot ban, mute or warn.\n"
        "- If a message is not addressed to you or needs no reply, answer exactly: none\n";
    if (!bio_.empty()) p += "\nAbout you:\n" + bio_ + "\n";
    if (!rules_.empty()) p += "\nRules you must follow:\n" + rules_ + "\n";
    return p;
}

std::vector<ChatMessage> Assistant::build_messages(const Request& r) const {
    std::vector<ChatMessage> msgs;
    msgs.push_back({"system", system_prompt()});

    std::string user;
    if (!r.chat_title.empty()) user += "[Chat: " + utf8_truncate(r.chat_title, 256) + "]\n";
    if (!r.history.empty()) {
        user += "[Recent messages]\n";
        for (const auto& h : r.history) user += h.author + " said: " + h.text + "\n";
        user += "\n";
    }
    if (!r.replied_text.empty()) {
        if (r.replied_is_bot) {
            // Continue the conversation: the bot's previous message is its own turn.
            msgs.push_back({"assistant", utf8_truncate(r.replied_text, kMaxInputBytes)});
        } else {
            user += "[In reply to: \"" + utf8_truncate(r.replied_text, kMaxInputBytes) + "\"]\n";
        }
    }
    const std::string name = r.user_name.empty() ? "User" : utf8_truncate(r.user_name, 128);
    user += name + " said: " + utf8_truncate(r.text, kMaxInputBytes);
    msgs.push_back({"user", user});
    return msgs;
}

Completion Assistant::ask(const Request& request) {
    Completion c = client_.complete(build_messages(request));
    c.text = is_silent(c.text) ? std::string{} : util::trim(c.text);
    return c;
}

// --- RateLimiter --------------------------------------------------------------

RateLimiter::RateLimiter(int user_cooldown_s, int chat_per_min)
    : user_cooldown_s_(user_cooldown_s), chat_per_min_(chat_per_min) {}

bool RateLimiter::allow(int64_t chat_id, int64_t user_id, int64_t now) {
    auto last = last_user_request_.find(user_id);
    if (last != last_user_request_.end() && now - last->second < user_cooldown_s_) return false;

    auto& window = chat_requests_[chat_id];
    while (!window.empty() && window.front() <= now - 60) window.pop_front();
    if (static_cast<int>(window.size()) >= chat_per_min_) return false;

    window.push_back(now);
    last_user_request_[user_id] = now;

    // keep the maps bounded
    if (last_user_request_.size() > 10000) {
        for (auto it = last_user_request_.begin(); it != last_user_request_.end();) {
            it = now - it->second >= user_cooldown_s_ ? last_user_request_.erase(it)
                                                      : std::next(it);
        }
    }
    return true;
}

// --- Worker -------------------------------------------------------------------

Worker::Worker(Assistant& assistant, AiClient& client, Deliver deliver,
               std::size_t queue_limit, std::shared_ptr<spdlog::logger> log)
    : assistant_(assistant),
      client_(client),
      deliver_(std::move(deliver)),
      queue_limit_(queue_limit),
      log_(std::move(log)) {}

Worker::~Worker() { shutdown(); }

void Worker::start() { thread_ = std::thread(&Worker::loop, this); }

void Worker::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ && !thread_.joinable()) return;
        stop_ = true;
        queue_.clear();
    }
    client_.cancel();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool Worker::submit(Job job) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_ || queue_.size() >= queue_limit_) return false;
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void Worker::loop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Result result;
        const auto started = std::chrono::steady_clock::now();
        try {
            const Completion c = assistant_.ask(job.request);
            if (!c.text.empty()) result.answer = c.text;
            result.prompt_tokens = c.prompt_tokens;
            result.completion_tokens = c.completion_tokens;
        } catch (const std::exception& e) {
            result.failed = true;
            log_->warn("AI request for chat {} failed: {}", job.chat_id, e.what());
        }
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stop_) return;  // shutting down: don't deliver
        }
        try {
            deliver_(job, result);
        } catch (const std::exception& e) {
            log_->warn("AI delivery for chat {} failed: {}", job.chat_id, e.what());
        }
    }
}

}  // namespace yacm::ai
