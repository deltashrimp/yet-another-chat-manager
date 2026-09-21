#include "telegram/types.hpp"

namespace yacm::tg {

void from_json(const nlohmann::json& j, User& u) {
    u.id = j.value("id", int64_t{0});
    u.is_bot = j.value("is_bot", false);
    u.first_name = j.value("first_name", std::string{});
    u.last_name = j.value("last_name", std::string{});
    u.username = j.value("username", std::string{});
    u.language_code = j.value("language_code", std::string{});
}

void from_json(const nlohmann::json& j, Chat& c) {
    c.id = j.value("id", int64_t{0});
    c.type = j.value("type", std::string{});
    c.title = j.value("title", std::string{});
    c.username = j.value("username", std::string{});
}

void from_json(const nlohmann::json& j, ReplyTo& r) {
    r.message_id = j.value("message_id", int64_t{0});
    r.date = j.value("date", int64_t{0});
    if (j.contains("from")) r.from = j.at("from").get<User>();
    r.text = j.value("text", std::string{});
}

void from_json(const nlohmann::json& j, Message& m) {
    m.message_id = j.value("message_id", int64_t{0});
    m.date = j.value("date", int64_t{0});
    if (j.contains("chat")) m.chat = j.at("chat").get<Chat>();
    if (j.contains("from")) m.from = j.at("from").get<User>();
    m.text = j.value("text", std::string{});
    if (j.contains("reply_to_message")) m.reply_to = j.at("reply_to_message").get<ReplyTo>();
    if (j.contains("new_chat_members")) {
        m.new_chat_members = j.at("new_chat_members").get<std::vector<User>>();
    }
    if (j.contains("left_chat_member")) {
        m.left_chat_member = j.at("left_chat_member").get<User>();
    }
}

void from_json(const nlohmann::json& j, ChatMemberUpdated& u) {
    u.date = j.value("date", int64_t{0});
    if (j.contains("chat")) u.chat = j.at("chat").get<Chat>();
    if (j.contains("from")) u.from = j.at("from").get<User>();

    auto read_member = [](const nlohmann::json& mj, int64_t& user_id, std::string& status) {
        if (mj.contains("user")) user_id = mj.at("user").value("id", int64_t{0});
        status = mj.value("status", std::string{});
    };
    if (j.contains("new_chat_member")) {
        read_member(j.at("new_chat_member"), u.user_id, u.new_status);
    }
    if (j.contains("old_chat_member")) {
        read_member(j.at("old_chat_member"), u.user_id, u.old_status);
    }
}

void from_json(const nlohmann::json& j, Update& u) {
    u.update_id = j.value("update_id", int64_t{0});
    if (j.contains("message")) u.message = j.at("message").get<Message>();
    if (j.contains("edited_message")) u.edited_message = j.at("edited_message").get<Message>();
    if (j.contains("chat_member")) u.chat_member = j.at("chat_member").get<ChatMemberUpdated>();
}

void from_json(const nlohmann::json& j, ChatMember& m) {
    if (j.contains("user")) m.user = j.at("user").get<User>();
    m.status = j.value("status", std::string{});
}

}  // namespace yacm::tg