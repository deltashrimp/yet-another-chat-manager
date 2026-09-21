#pragma once

#include "telegram/types.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace yacm::tg {

// Thrown when the Telegram Bot API call fails.
struct TelegramError : std::runtime_error {
    int error_code = 0;
    long retry_after = 0;  // set for "flood wait" (429 with retry_after)

    TelegramError(std::string message) : std::runtime_error(std::move(message)) {}
    TelegramError(int code, std::string message)
        : std::runtime_error(std::move(message)), error_code(code) {}

    [[nodiscard]] bool is_flood() const { return retry_after > 0; }
};

// Abstract Telegram Bot API seam. Handlers and services talk to this
// interface only (never to the concrete libcurl client), so tests can
// inject a scripted fake (spec §10: "через интерфейс, чтобы мокать в
// тестах").
class TelegramClient {
public:
    virtual ~TelegramClient() = default;

    virtual User get_me() = 0;
    virtual std::vector<Update> get_updates(int64_t offset, int timeout_s) = 0;

    virtual Message send_message(int64_t chat_id, const std::string& text,
                                 const std::string& parse_mode = "",
                                 std::optional<int64_t> reply_to_message_id = std::nullopt,
                                 long timeout_s = 30) = 0;
    virtual void delete_message(int64_t chat_id, int64_t message_id) = 0;

    virtual Chat get_chat(int64_t chat_id) = 0;
    virtual ChatMember get_chat_member(int64_t chat_id, int64_t user_id) = 0;
    virtual std::vector<ChatMember> get_chat_administrators(int64_t chat_id) = 0;

    virtual void ban_chat_member(int64_t chat_id, int64_t user_id,
                                 const std::string& reason) = 0;
    virtual void unban_chat_member(int64_t chat_id, int64_t user_id) = 0;
    virtual void restrict_chat_member(int64_t chat_id, int64_t user_id, long until_date,
                                      const nlohmann::json& permissions) = 0;
    virtual void unpin_all_messages(int64_t chat_id) = 0;
};

// Thin wrapper over the Telegram Bot HTTP API (long polling).
// Thread-safe: every call uses its own libcurl handle.
class Client final : public TelegramClient {
public:
    explicit Client(std::string token);

    // Performs a POST request to the Bot API and returns the "result"
    // member of the response JSON. Throws TelegramError on failure.
    // (Lower-level helper; the TelegramClient interface does not expose it.)
    nlohmann::json call(const std::string& method, const nlohmann::json& params,
                        long timeout_s = 30);

    User get_me() override;
    std::vector<Update> get_updates(int64_t offset, int timeout_s) override;

    Message send_message(int64_t chat_id, const std::string& text,
                         const std::string& parse_mode = "",
                         std::optional<int64_t> reply_to_message_id = std::nullopt,
                         long timeout_s = 30) override;
    void delete_message(int64_t chat_id, int64_t message_id) override;

    Chat get_chat(int64_t chat_id) override;
    ChatMember get_chat_member(int64_t chat_id, int64_t user_id) override;
    std::vector<ChatMember> get_chat_administrators(int64_t chat_id) override;

    void ban_chat_member(int64_t chat_id, int64_t user_id, const std::string& reason) override;
    void unban_chat_member(int64_t chat_id, int64_t user_id) override;
    void restrict_chat_member(int64_t chat_id, int64_t user_id, long until_date,
                              const nlohmann::json& permissions) override;
    void unpin_all_messages(int64_t chat_id) override;

private:
    static std::pair<long, std::string> post(const std::string& url, const std::string& body,
                                             long timeout_s);

    std::string api_url_;
};

}  // namespace yacm::tg