#pragma once

#include "services/roles.hpp"
#include "storage/repos.hpp"
#include "telegram/types.hpp"

#include <optional>
#include <string>

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Sends a message to the same chat as `src`, optionally replying to it.
void reply(app::AppContext& ctx, const tg::Message& src, std::string text,
           bool html = false, bool reply_to = true);

// Sends a localized error message to the same chat as `src`.
void reply_error(app::AppContext& ctx, const tg::Message& src, const std::string& key);

// Language of the chat (per-chat language, defaulting to the global one).
std::string chat_lang(app::AppContext& ctx, const tg::Message& src);

// Spec §6.2: whether success replies for `command` are enabled in this
// chat. Errors are always shown regardless. Defaults to enabled.
bool success_answers_on(app::AppContext& ctx, const tg::Message& src,
                        const std::string& command);

bool require_private(app::AppContext& ctx, const tg::Message& src);
bool require_group(app::AppContext& ctx, const tg::Message& src);
bool require_role(app::AppContext& ctx, const tg::Message& src, services::Role required);

// Resolves a user reference (@username / numeric ID / reply) against the
// `users` DB cache. Returns nullopt when the user is unknown to the bot.
std::optional<storage::UserRow> resolve_user_ref(app::AppContext& ctx, const tg::Message& m,
                                                 const std::string& args);

// "@username", or an HTML profile link built from the display name.
std::string user_handle(const storage::UserRow& row);

}  // namespace yacm::handlers