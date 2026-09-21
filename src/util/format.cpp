#include "util/format.hpp"

#include "telegram/types.hpp"

#include <ctime>
#include <string>

namespace yacm::util {

std::string html_escape(std::string s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string user_tag(const tg::User& user) {
    if (!user.username.empty()) return "@" + user.username;
    return "<a href=\"tg://user?id=" + std::to_string(user.id) + "\">" +
           html_escape(display_name(user)) + "</a>";
}

std::string display_name(const tg::User& user) {
    std::string name = user.first_name;
    if (!user.last_name.empty()) {
        if (!name.empty()) name += ' ';
        name += user.last_name;
    }
    return name;
}

std::string display_name_or_id(const tg::User& user) {
    std::string name = display_name(user);
    if (name.empty()) name = "ID " + std::to_string(user.id);
    return name;
}

std::string format_date(int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string message_link(int64_t chat_id, int64_t message_id) {
    if (message_id <= 0 || chat_id >= 0) return "";
    std::string peer = std::to_string(chat_id);
    if (peer.rfind("-100", 0) != 0) return "";  // basic groups: no public link
    peer = peer.substr(4);                       // strip the supergroup "-100"
    return "https://t.me/c/" + peer + "/" + std::to_string(message_id);
}

}  // namespace yacm::util