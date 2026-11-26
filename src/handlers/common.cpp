#include "handlers/common.hpp"

#include "app/app_context.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <spdlog/spdlog.h>

#include <utility>

namespace yacm::handlers {

void reply(app::AppContext& ctx, const tg::Message& src, std::string text, bool html,
           bool reply_to) {
    if (!src.chat) return;
    ctx.sender->enqueue(tg::Outgoing{
        src.chat->id,
        std::move(text),
        html ? "HTML" : "",
        reply_to ? std::optional<int64_t>(src.message_id) : std::nullopt,
    });
}

void reply_error(app::AppContext& ctx, const tg::Message& src, const std::string& key) {
    reply(ctx, src, ctx.i18n.t(chat_lang(ctx, src), key));
}

std::string chat_lang(app::AppContext& ctx, const tg::Message& src) {
    if (src.chat && !src.chat->is_private()) {
        const std::string lang = ctx.chats->language(src.chat->id);
        if (!lang.empty()) return lang;
    }
    return ctx.config.default_lang;
}

bool success_answers_on(app::AppContext& ctx, const tg::Message& src,
                        const std::string& command) {
    if (!src.chat) return true;
    return ctx.answers->enabled(src.chat->id, command);
}

bool require_private(app::AppContext& ctx, const tg::Message& src) {
    if (src.chat && src.chat->is_private()) return true;
    reply_error(ctx, src, "errors.only_private");
    return false;
}

bool require_group(app::AppContext& ctx, const tg::Message& src) {
    if (src.chat && src.chat->is_group()) return true;
    reply_error(ctx, src, "errors.only_groups");
    return false;
}

bool require_role(app::AppContext& ctx, const tg::Message& src, services::Role required) {
    if (!src.chat || !src.from) return false;
    const services::Role actual = ctx.roles->resolve(src.chat->id, *src.from);
    if (services::meets(required, actual)) return true;
    reply_error(ctx, src, "errors.not_permitted");
    return false;
}

// Resolves a user reference to a DB row (cache).
std::optional<storage::UserRow> resolve_user_ref(app::AppContext& ctx, const tg::Message& m,
                                                 const std::string& args) {
    using util::UserRef;
    const UserRef ref = util::parse_user_ref(args);

    switch (ref.kind) {
        case UserRef::Kind::Id:
            return ctx.users->find_by_id(ref.id);
        case UserRef::Kind::Username:
            return ctx.users->find_by_username(ref.username);
        case UserRef::Kind::Reply:
        case UserRef::Kind::Invalid:
            // Only an *empty* reference means "the replied-to user". A typo'd
            // explicit target ("/ban @bad name" -> "bad") must not silently
            // hit whoever the command happens to reply to.
            if (ref.kind == UserRef::Kind::Invalid && !util::trim(args).empty()) {
                return std::nullopt;
            }
            if (m.reply_to && m.reply_to->from) {
                return ctx.users->find_by_id(m.reply_to->from->id);
            }
            return std::nullopt;
    }
    return std::nullopt;
}

void try_delete(app::AppContext& ctx, int64_t chat_id, int64_t message_id) {
    if (message_id <= 0) return;
    try {
        ctx.client->delete_message(chat_id, message_id);
    } catch (const std::exception& e) {
        ctx.log->debug("deleteMessage {} in {} failed: {}", message_id, chat_id, e.what());
    }
}

std::string user_handle(const storage::UserRow& row) {
    if (!row.username.empty()) return "@" + util::html_escape(row.username);

    std::string name = row.first_name;
    if (!row.last_name.empty()) {
        if (!name.empty()) name += ' ';
        name += row.last_name;
    }
    if (name.empty()) name = "ID " + std::to_string(row.id);
    return "<a href=\"tg://user?id=" + std::to_string(row.id) + "\">" +
           util::html_escape(name) + "</a>";
}

}  // namespace yacm::handlers