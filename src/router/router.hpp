#pragma once

#include "services/roles.hpp"
#include "telegram/types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yacm::router {

// Registers slash-command handlers and dispatches parsed commands from
// messages.
class HandlerRegistry {
public:
    using Handler = std::function<void(const tg::Message&, const std::string& args)>;

    struct Command {
        Handler handler;
        services::Role min_role = services::Role::Everyone;
        bool dm_only = false;
    };

    void add(const std::string& name, Handler handler,
             services::Role min_role = services::Role::Everyone, bool dm_only = false);

    // Own bot username (without '@'). Once set, "/cmd@OtherBot" commands
    // addressed to a different bot are ignored. Empty = accept any suffix.
    void set_bot_username(const std::string& username);

    // Returns true when the message contained a registered command.
    bool dispatch(const tg::Message& message) const;

    // True when the message is a registered command addressed to this bot
    // (not "/cmd@OtherBot", not an unknown command).
    bool is_own_command(const tg::Message& message) const;

    // Commands visible to `role` in `in_private_chat` (used by /help).
    std::vector<std::string> commands_for(services::Role role, bool in_private_chat) const;

    const Command* find(const std::string& name) const;

private:
    std::map<std::string, Command> commands_;
    std::string bot_username_;  // lower-case

    static std::optional<std::pair<std::string, std::string>> parse_command(
        const std::string& text);
};

}  // namespace yacm::router