#include "handlers/owner.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "services/roles.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <fmt/format.h>

#include <chrono>
#include <optional>
#include <string>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Target {
    int64_t id = 0;
    std::string handle;  // HTML-safe
};

// "<ref> [reason]" or a reply with "[reason]". A numeric ID works even for
// users the bot has never seen (so known spammers can be banned up front).
std::optional<Target> parse_target(AppContext& ctx, const tg::Message& m,
                                   const std::string& args, std::string& reason) {
    const std::string s = util::trim(args);
    const std::size_t sp = s.find_first_of(" \t");
    const std::string first = sp == std::string::npos ? s : s.substr(0, sp);
    const util::UserRef ref = util::parse_user_ref(first);

    if (ref.valid()) {
        reason = sp == std::string::npos ? std::string{} : util::trim(s.substr(sp + 1));
        if (const auto row = resolve_user_ref(ctx, m, first)) {
            return Target{row->id, user_handle(*row)};
        }
        if (ref.kind == util::UserRef::Kind::Id) {
            return Target{ref.id, "ID " + std::to_string(ref.id)};
        }
        return std::nullopt;  // unknown @username
    }
    reason = s;
    if (m.reply_to && m.reply_to->from) {
        return Target{m.reply_to->from->id, util::user_tag(*m.reply_to->from)};
    }
    return std::nullopt;
}

// /gbban <id|@username|reply> [reason] (bot owner): the bot ignores the user
// everywhere — commands, AI, reports.
void on_gbban(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!m.chat || !m.from) return;
    if (!require_role(ctx, m, services::Role::BotOwner)) return;

    std::string reason;
    const auto target = parse_target(ctx, m, args, reason);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    if (target->id == ctx.bot_id) {
        reply_error(ctx, m, "errors.cannot_restrict_bot");
        return;
    }
    if (target->id == m.from->id || target->id == ctx.config.bot_owner_id) {
        reply_error(ctx, m, "gbban.cannot_owner");
        return;
    }

    const std::string lang = chat_lang(ctx, m);
    if (!ctx.global_bans->add(target->id, reason, m.from->id, unix_now())) {
        reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(lang, "gbban.already")), target->handle),
              /*html=*/true);
        return;
    }
    ctx.audit->log(m.chat->id, m.from->id, "gbban", target->id, reason, unix_now());
    reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(lang, "gbban.done")), target->handle),
          /*html=*/true);
}

// /ungbban <id|@username|reply> (bot owner).
void on_ungbban(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!m.chat || !m.from) return;
    if (!require_role(ctx, m, services::Role::BotOwner)) return;

    std::string reason;
    const auto target = parse_target(ctx, m, args, reason);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const std::string lang = chat_lang(ctx, m);
    if (!ctx.global_bans->remove(target->id)) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "gbban.not_banned")), target->handle),
              /*html=*/true);
        return;
    }
    ctx.audit->log(m.chat->id, m.from->id, "ungbban", target->id, "", unix_now());
    reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(lang, "gbban.undone")), target->handle),
          /*html=*/true);
}

}  // namespace

void register_owner(AppContext& ctx) {
    using services::Role;
    ctx.router.add("gbban", [&ctx](const tg::Message& m, const std::string& a) {
        on_gbban(ctx, m, a);
    }, Role::BotOwner);
    ctx.router.add("ungbban", [&ctx](const tg::Message& m, const std::string& a) {
        on_ungbban(ctx, m, a);
    }, Role::BotOwner);
}

}  // namespace yacm::handlers
