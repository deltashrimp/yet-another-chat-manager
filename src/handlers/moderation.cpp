#include "handlers/moderation.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "router/router.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "services/warns.hpp"
#include "storage/repos.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

// Builds a tg::User from a cached row (for role resolution).
tg::User to_user(const storage::UserRow& row) {
    return tg::User{row.id, row.is_bot, row.first_name, row.last_name, row.username, ""};
}

// Splits "<ref> [reason]" into a ref token and the reason. When the first
// token is not a user reference, everything is treated as the reason
// (reply-based usage: "/ban spam reason" targets the replied-to message).
std::pair<std::string, std::string> split_ref_reason(const std::string& args) {
    std::string s = util::trim(args);
    if (s.empty()) return {"", ""};

    const std::size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos) {
        if (util::parse_user_ref(s).valid()) return {s, ""};
        return {"", s};  // reply usage with a one-word reason ("/ban spam")
    }

    const std::string tok = s.substr(0, sp);
    const std::string rest = util::trim(s.substr(sp + 1));
    if (util::parse_user_ref(tok).valid()) return {tok, rest};
    return {"", s};  // reply usage, all text is the reason
}

// The target ref only (commands that take no reason).
std::string first_ref(const std::string& args) {
    return split_ref_reason(args).first;
}

std::optional<storage::UserRow> resolve_target(AppContext& ctx, const tg::Message& m,
                                               const std::string& ref_token) {
    return resolve_user_ref(ctx, m, ref_token);
}

// --- /ban /unban (owner) ------------------------------------------------

void on_ban(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const auto [ref, reason] = split_ref_reason(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.moderation->ban(m.chat->id, m.from->id, target->id, role, reason);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "ban")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "moderation.banned")),
                          user_handle(*target)),
              /*html=*/true);
    }
}

void on_unban(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const std::string ref = first_ref(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.moderation->unban(m.chat->id, m.from->id, target->id, role);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "unban")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "moderation.unbanned")),
                          user_handle(*target)),
              /*html=*/true);
    }
}

// --- /kick (admin) -------------------------------------------------------

void on_kick(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const auto [ref, reason] = split_ref_reason(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.moderation->kick(m.chat->id, m.from->id, target->id, role, reason);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "kick")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "moderation.kicked")),
                          user_handle(*target)),
              /*html=*/true);
    }
}

// --- /mute /unmute (admin) ----------------------------------------------

void on_mute(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const util::MuteTokens toks = util::split_mute_tokens(args);
    if (toks.bad_duration) {
        reply_error(ctx, m, "errors.invalid_duration");
        return;
    }
    const auto target = resolve_target(ctx, m, toks.target);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const int64_t until_s =
        toks.duration ? *util::parse_duration_seconds(*toks.duration) : 0;
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.moderation->mute(m.chat->id, m.from->id, target->id, role, until_s,
                                        toks.reason);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    const std::string lang = chat_lang(ctx, m);
    if (!success_answers_on(ctx, m, "mute")) return;
    const std::string text =
        toks.duration
            ? fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.muted_for")),
                          user_handle(*target), *toks.duration)
            : fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.muted")),
                          user_handle(*target));
    reply(ctx, m, text, /*html=*/true);
}

void on_unmute(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.moderation->unmute(m.chat->id, m.from->id, target->id, role);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "unmute")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "moderation.unmuted")),
                          user_handle(*target)),
              /*html=*/true);
    }
}

// --- /warn /unwarn /warnlist (admin) -------------------------------------

void on_warn(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const auto [ref, reason] = split_ref_reason(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.warn_service->warn(m.chat->id, m.from->id, target->id, role, reason);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    const std::string lang = chat_lang(ctx, m);
    if (!success_answers_on(ctx, m, "warn")) return;
    if (!r.fired_action.empty()) {
        // Limit reached and the automatic action fired (spec §5.6).
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "warns.limit_" + r.fired_action)),
                          user_handle(*target)),
              /*html=*/true);
        return;
    }
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, "warns.given")), user_handle(*target),
                      r.count, r.limit),
          /*html=*/true);
}

void on_unwarn(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.warn_service->unwarn(m.chat->id, m.from->id, target->id, role);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "unwarn")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "warns.removed")),
                          user_handle(*target), r.count, r.limit),
              /*html=*/true);
    }
}

void on_warnlist(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }

    const std::string lang = chat_lang(ctx, m);
    const auto rows = ctx.warn_service->list(m.chat->id, target->id);
    std::string text =
        fmt::format(fmt::runtime(ctx.i18n.t(lang, "warns.list_title")), user_handle(*target),
                    rows.size()) +
        "\n";
    if (rows.empty()) {
        text += ctx.i18n.t(lang, "warns.empty");
        reply(ctx, m, text, /*html=*/true);
        return;
    }
    int n = 0;
    for (const auto& row : rows) {
        ++n;
        const std::string reason =
            row.reason.empty() ? ctx.i18n.t(lang, "warns.no_reason")
                               : util::html_escape(row.reason);
        text += fmt::format("{}. {} — {}", n, util::format_date(row.created_at), reason) + "\n";
    }
    reply(ctx, m, text, /*html=*/true);
}

}  // namespace

void register_moderation(AppContext& ctx) {
    ctx.router.add("ban", [&ctx](const tg::Message& m, const std::string& a) {
        on_ban(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("unban", [&ctx](const tg::Message& m, const std::string& a) {
        on_unban(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("kick", [&ctx](const tg::Message& m, const std::string& a) {
        on_kick(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("mute", [&ctx](const tg::Message& m, const std::string& a) {
        on_mute(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("unmute", [&ctx](const tg::Message& m, const std::string& a) {
        on_unmute(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("warn", [&ctx](const tg::Message& m, const std::string& a) {
        on_warn(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("unwarn", [&ctx](const tg::Message& m, const std::string& a) {
        on_unwarn(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("warnlist", [&ctx](const tg::Message& m, const std::string& a) {
        on_warnlist(ctx, m, a);
    }, services::Role::Admin);
}

}  // namespace yacm::handlers