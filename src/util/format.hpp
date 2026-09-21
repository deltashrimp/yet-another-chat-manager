#pragma once

#include <string>

namespace yacm::tg {
struct User;
}

namespace yacm::util {

// Escapes text for Telegram HTML parse mode.
std::string html_escape(std::string s);

// "@username" or an HTML profile link using the display name.
std::string user_tag(const tg::User& user);

// Raw display name: "First Last".
std::string display_name(const tg::User& user);

// "First La..." with last name; falls back to ID when both names are empty.
std::string display_name_or_id(const tg::User& user);

// Formats a unix timestamp as local "YYYY-MM-DD HH:MM".
std::string format_date(int64_t unix_seconds);

// https://t.me/c/<peer>/<message_id> deep link for a chat, or "" when the
// chat has no stable public message link (basic groups, non-group chats).
// Supergroup ids (-100xxxxxxxxxx) are reduced to the <peer> part.
std::string message_link(int64_t chat_id, int64_t message_id);

}  // namespace yacm::util