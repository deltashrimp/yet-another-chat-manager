#include "router/router.hpp"

#include "util/parsers.hpp"

#include <cctype>

namespace yacm::router {

void HandlerRegistry::add(const std::string& name, Handler handler,
                          services::Role min_role, bool dm_only) {
    commands_[name] = Command{std::move(handler), min_role, dm_only};
}

bool HandlerRegistry::dispatch(const tg::Message& message) const {
    if (!message.is_command()) return false;
    auto parsed = parse_command(message.text);
    if (!parsed) return false;

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