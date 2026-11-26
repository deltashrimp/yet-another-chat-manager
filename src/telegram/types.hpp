#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace yacm::tg {

struct User {
    int64_t id = 0;
    bool is_bot = false;
    std::string first_name;
    std::string last_name;
    std::string username;
    std::string language_code;
};

struct Chat {
    int64_t id = 0;
    std::string type;  // "private" | "group" | "supergroup" | "channel"
    std::string title;
    std::string username;

    [[nodiscard]] bool is_private() const { return type == "private"; }
    [[nodiscard]] bool is_group() const { return type == "group" || type == "supergroup"; }
};

// Trimmed-down copy of a message that a command may reply to.
struct ReplyTo {
    int64_t message_id = 0;
    int64_t date = 0;
    std::optional<User> from;
    std::string text;
    std::string caption;  // text attached to a photo/video/document

    // Readable text: the message text, or the media caption.
    [[nodiscard]] const std::string& content() const { return text.empty() ? caption : text; }
};

struct Message {
    int64_t message_id = 0;
    int64_t date = 0;
    std::optional<Chat> chat;
    std::optional<User> from;
    std::string text;
    std::string caption;  // text attached to a photo/video/document
    std::optional<ReplyTo> reply_to;
    std::vector<User> new_chat_members;
    std::optional<User> left_chat_member;

    [[nodiscard]] bool is_command() const {
        return !text.empty() && text.front() == '/';
    }
    // Readable text: the message text, or the media caption (images
    // themselves are never read).
    [[nodiscard]] const std::string& content() const { return text.empty() ? caption : text; }
};

struct ChatMemberUpdated {
    int64_t date = 0;
    std::optional<Chat> chat;
    std::optional<User> from;
    int64_t user_id = 0;
    std::string old_status;
    std::string new_status;
};

struct Update {
    int64_t update_id = 0;
    std::optional<Message> message;
    std::optional<Message> edited_message;
    std::optional<ChatMemberUpdated> chat_member;
};

struct ChatMember {
    User user;
    std::string status;  // creator | administrator | member | restricted | left | kicked
};

// --- JSON deserialization ------------------------------------------------

void from_json(const nlohmann::json& j, User& u);
void from_json(const nlohmann::json& j, Chat& c);
void from_json(const nlohmann::json& j, ReplyTo& r);
void from_json(const nlohmann::json& j, Message& m);
void from_json(const nlohmann::json& j, ChatMemberUpdated& u);
void from_json(const nlohmann::json& j, Update& u);
void from_json(const nlohmann::json& j, ChatMember& m);

}  // namespace yacm::tg