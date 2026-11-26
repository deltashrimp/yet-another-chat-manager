#include "telegram/client.hpp"

#include <curl/curl.h>
#include <spdlog/spdlog.h>

#include <stdexcept>

namespace yacm::tg {
namespace {

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

Client::Client(std::string token)
    : api_url_("https://api.telegram.org/bot" + std::move(token) + "/") {}

std::pair<long, std::string> Client::post(const std::string& url, const std::string& body,
                                          long timeout_s) {
    CURL* handle = curl_easy_init();
    if (!handle) throw std::runtime_error("curl_easy_init failed");

    std::string response;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    long http_status = 0;

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "yacm/0.1.0");
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(handle);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_status);
    } else {
        const std::string error = curl_easy_strerror(rc);
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        throw std::runtime_error("curl error: " + error);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return {http_status, std::move(response)};
}

nlohmann::json Client::call(const std::string& method, const nlohmann::json& params,
                            long timeout_s) {
    auto [status, body] = post(api_url_ + method, params.dump(), timeout_s);

    // The Bot API reports errors (including 429 flood waits) with a non-2xx
    // HTTP status AND a JSON body carrying error_code / retry_after, so the
    // body must be parsed before the status is judged — otherwise flood
    // waits are never honoured and callers hammer the API.
    nlohmann::json response = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (response.is_discarded() || !response.is_object()) {
        if (status < 200 || status >= 300) {
            throw TelegramError(static_cast<int>(status), "HTTP " + std::to_string(status));
        }
        throw TelegramError("invalid JSON response from Telegram");
    }

    if (!response.value("ok", false)) {
        const int code = response.value("error_code", static_cast<int>(status));
        std::string description = "unknown error";
        if (response.contains("description") && response["description"].is_string()) {
            description = response["description"].get<std::string>();
        }
        TelegramError err(code, std::move(description));
        if (response.contains("parameters") && response["parameters"].is_object()) {
            const auto& p = response["parameters"];
            if (p.contains("retry_after") && p["retry_after"].is_number_integer()) {
                err.retry_after = p["retry_after"].get<long>();
            }
        }
        throw err;
    }
    if (!response.contains("result")) throw TelegramError("Telegram response has no result");
    return response.at("result");
}

User Client::get_me() { return call("getMe", nlohmann::json::object()).get<User>(); }

std::vector<Update> Client::get_updates(int64_t offset, int timeout_s) {
    nlohmann::json params = {
        {"offset", offset},
        {"timeout", timeout_s},
        {"allowed_updates", {"message", "edited_message", "chat_member"}},
    };
    return call("getUpdates", params, static_cast<long>(timeout_s) + 15L)
        .get<std::vector<Update>>();
}

Message Client::send_message(int64_t chat_id, const std::string& text,
                             const std::string& parse_mode,
                             std::optional<int64_t> reply_to_message_id, long timeout_s) {
    nlohmann::json params = {{"chat_id", chat_id}, {"text", text}};
    if (!parse_mode.empty()) params["parse_mode"] = parse_mode;
    if (reply_to_message_id) params["reply_to_message_id"] = *reply_to_message_id;
    return call("sendMessage", params, timeout_s).get<Message>();
}

void Client::delete_message(int64_t chat_id, int64_t message_id) {
    call("deleteMessage", {{"chat_id", chat_id}, {"message_id", message_id}});
}

void Client::send_chat_action(int64_t chat_id, const std::string& action) {
    call("sendChatAction", {{"chat_id", chat_id}, {"action", action}}, 10);
}

Chat Client::get_chat(int64_t chat_id) {
    return call("getChat", {{"chat_id", chat_id}}).get<Chat>();
}

ChatMember Client::get_chat_member(int64_t chat_id, int64_t user_id) {
    return call("getChatMember", {{"chat_id", chat_id}, {"user_id", user_id}})
        .get<ChatMember>();
}

std::vector<ChatMember> Client::get_chat_administrators(int64_t chat_id) {
    return call("getChatAdministrators", {{"chat_id", chat_id}})
        .get<std::vector<ChatMember>>();
}

void Client::ban_chat_member(int64_t chat_id, int64_t user_id, const std::string& reason,
                             long until_date) {
    (void)reason;  // the Bot API has no reason field; it lives in the bans table/audit
    nlohmann::json params = {{"chat_id", chat_id}, {"user_id", user_id}};
    if (until_date > 0) params["until_date"] = until_date;
    call("banChatMember", params);
}

void Client::unban_chat_member(int64_t chat_id, int64_t user_id) {
    call("unbanChatMember",
         {{"chat_id", chat_id}, {"user_id", user_id}, {"only_if_banned", true}});
}

void Client::restrict_chat_member(int64_t chat_id, int64_t user_id, long until_date,
                                  const nlohmann::json& permissions) {
    nlohmann::json params = {
        {"chat_id", chat_id},
        {"user_id", user_id},
        {"permissions", permissions},
    };
    if (until_date > 0) params["until_date"] = until_date;
    call("restrictChatMember", params);
}

void Client::unpin_all_messages(int64_t chat_id) {
    call("unpinAllChatMessages", {{"chat_id", chat_id}});
}

}  // namespace yacm::tg