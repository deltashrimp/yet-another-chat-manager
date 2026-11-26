#include "router/router.hpp"

#include "util/parsers.hpp"

#include <cctype>

namespace yacm::router {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "BotName" from "/cmd@BotName args", or "" when there is no @suffix.
std::string parse_addressee(const std::string& text) {
    std::size_t i = 1;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) &&
           text[i] != '@') {
        ++i;
    }
    if (i >= text.size() || text[i] != '@') return "";
    const std::size_t begin = i + 1;
    std::size_t end = begin;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
    return text.substr(begin, end - begin);
}

}  // namespace

void HandlerRegistry::set_bot_username(const std::string& username) {
    bot_username_ = lower(username);
}

void HandlerRegistry::add(const std::string& name, Handler handler,
                          services::Role min_role, bool dm_only) {
    commands_[name] = Command{std::move(handler), min_role, dm_only};
}

bool HandlerRegistry::is_own_command(const tg::Message& message) const {
    if (!message.is_command()) return false;
    const auto parsed = parse_command(message.text);
    if (!parsed) return false;
    if (!bot_username_.empty()) {
        const std::string addressee = parse_addressee(message.text);
        if (!addressee.empty() && lower(addressee) != bot_username_) return false;
    }
    return commands_.count(parsed->first) > 0;
}

bool HandlerRegistry::dispatch(const tg::Message& message) const {
    if (!message.is_command()) return false;
    auto parsed = parse_command(message.text);
    if (!parsed) return false;

    // "/cmd@OtherBot" is addressed to another bot in the same group: ignore
    // it, otherwise e.g. "/ban@OtherBot" would trigger our /ban as well.
    if (!bot_username_.empty()) {
        const std::string addressee = parse_addressee(message.text);
        if (!addressee.empty() && lower(addressee) != bot_username_) return false;
    }

    auto it = commands_.find(parsed->first);
    if (it == commands_.end()) return false;

    it->second.handler(message, parsed->second);
    return true;
}

std::vector<std::string> HandlerRegistry::commands_for(services::Role role,
                                                       bool in_private_chat) const {
    std::vector<std::string> result;
    for (const auto& [name, command] : commands_) {
        if (command.dm_only && !in_private_chat) continue;  // hidden in groups
        if (!services::meets(command.min_role, role)) continue;
        result.push_back(name);
    }
    return result;
}

const HandlerRegistry::Command* HandlerRegistry::find(const std::string& name) const {
    auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

std::optional<std::pair<std::string, std::string>> HandlerRegistry::parse_command(
    const std::string& text) {
    if (text.empty() || text.front() != '/') return std::nullopt;

    const std::size_t begin = 1;
    std::size_t end = begin;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
           text[end] != '@') {
        ++end;
    }

    std::string name = text.substr(begin, end - begin);
    if (name.empty()) return std::nullopt;

    std::size_t rest = end;
    if (rest < text.size() && text[rest] == '@') {
        // "/cmd@BotName ..." — skip the bot username suffix.
        rest = text.find_first_of(" \t", rest);
        if (rest == std::string::npos) rest = text.size();
    }

    const std::string args = util::trim(text.substr(rest));
    return std::make_pair(name, args);
}

}  // namespace yacm::router