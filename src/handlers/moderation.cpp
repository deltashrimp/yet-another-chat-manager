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

// --- restrictions: ban / tban / dban / sban, mute / tmute / ..., kick ---
//
// Every restriction comes in up to four flavours (Rose-compatible):
//   /ban    <ref|reply> [reason]           plain
//   /tban   <ref|reply> <time> [reason]    timed
//   /dban   [reason]          (reply only)  also deletes the replied message
//   /sban   <ref|reply> [reason]           silent: deletes the command (and
//                                          the replied message), no notice
// Errors are always reported, even for the silent flavour.

enum class Op { Ban, TBan, Kick, Mute, TMute };
enum class Flavor { Normal, Delete, Silent };

bool is_timed(Op op) { return op == Op::TBan || op == Op::TMute; }

services::Role required_role(Op op) {
    // Bans stay owner-only (project policy); mutes and kicks are for admins.
    return (op == Op::Ban || op == Op::TBan) ? services::Role::Owner : services::Role::Admin;
}

struct RestrictionArgs {
    std::optional<storage::UserRow> target;
    int64_t duration_s = 0;
    std::string reason;
    std::string error_key;  // set when parsing failed
};

// Target from the replied-to message. The author is upserted first: they
// may never have posted since the bot joined, but the reply proves they
// exist.
std::optional<storage::UserRow> reply_target(AppContext& ctx, const tg::Message& m) {
    if (!m.reply_to || !m.reply_to->from) return std::nullopt;
    try {
        ctx.users->upsert(*m.reply_to->from, m.date);
    } catch (const std::exception& e) {
        spdlog::warn("cannot cache replied-to user: {}", e.what());
    }
    return ctx.users->find_by_id(m.reply_to->from->id);
}

RestrictionArgs parse_restriction(AppContext& ctx, const tg::Message& m, const std::string& args,
                                  Op op, Flavor flavor) {
    RestrictionArgs out;
    if (flavor == Flavor::Delete) {
        out.target = reply_target(ctx, m);
        if (!out.target) {
            out.error_key = "errors.need_reply";
            return out;
        }
        std::string rest = util::trim(args);
        if (is_timed(op)) {
            const std::size_t sp = rest.find_first_of(" \t");
            const std::string tok = sp == std::string::npos ? rest : rest.substr(0, sp);
            const auto d = util::parse_duration_seconds(tok);
            if (!d) {
                out.error_key = tok.empty() ? "errors.duration_required" : "errors.invalid_duration";
                return out;
            }
            out.duration_s = *d;
            rest = util::trim(sp == std::string::npos ? std::string{} : rest.substr(sp + 1));
        }
        out.reason = rest;
        return out;
    }

    if (op == Op::Ban || op == Op::Kick) {
        const auto [ref, reason] = split_ref_reason(args);
        out.target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
        out.reason = reason;
    } else {
        const util::MuteTokens toks = util::split_mute_tokens(args);
        if (toks.bad_duration) {
            out.error_key = "errors.invalid_duration";
            return out;
        }
        if (is_timed(op) && !toks.duration) {
            out.error_key = "errors.duration_required";
            return out;
        }
        out.target =
            toks.target.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, toks.target);
        if (toks.duration) out.duration_s = *util::parse_duration_seconds(*toks.duration);
        out.reason = toks.reason;
    }
    if (!out.target) out.error_key = "errors.user_not_found";
    return out;
}

std::string restriction_text(AppContext& ctx, const tg::Message& m, Op op,
                             const storage::UserRow& target, int64_t duration_s) {
    const std::string lang = chat_lang(ctx, m);
    const std::string who = user_handle(target);
    const std::string dur = util::format_duration(duration_s);
    switch (op) {
        case Op::Ban:
            return fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.banned")), who);
        case Op::TBan:
            return fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.banned_for")), who, dur);
        case Op::Kick:
            return fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.kicked")), who);
        case Op::Mute:
        case Op::TMute:
            break;
    }
    if (duration_s > 0) {
        return fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.muted_for")), who, dur);
    }
    return fmt::format(fmt::runtime(ctx.i18n.t(lang, "moderation.muted")), who);
}

void run_restriction(AppContext& ctx, const tg::Message& m, const std::string& args, Op op,
                     Flavor flavor, const std::string& command) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, required_role(op))) return;

    const RestrictionArgs a = parse_restriction(ctx, m, args, op, flavor);
    if (!a.error_key.empty()) {
        reply_error(ctx, m, a.error_key);
        return;
    }
    const storage::UserRow& target = *a.target;
    const int64_t chat_id = m.chat->id;
    const services::Role role = ctx.roles->resolve(chat_id, to_user(target));

    services::ModerationResult r;
    switch (op) {
        case Op::Ban:
        case Op::TBan:
            r = ctx.moderation->ban(chat_id, m.from->id, target.id, role, a.reason,
                                    a.duration_s);
            break;
        case Op::Kick:
            r = ctx.moderation->kick(chat_id, m.from->id, target.id, role, a.reason);
            break;
        case Op::Mute:
        case Op::TMute:
            r = ctx.moderation->mute(chat_id, m.from->id, target.id, role, a.duration_s,
                                     a.reason);
            break;
    }
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }

    if (flavor == Flavor::Delete && m.reply_to) try_delete(ctx, chat_id, m.reply_to->message_id);
    if (flavor == Flavor::Silent) {
        try_delete(ctx, chat_id, m.message_id);
        if (m.reply_to) try_delete(ctx, chat_id, m.reply_to->message_id);
        return;
    }
    if (success_answers_on(ctx, m, command)) {
        reply(ctx, m, restriction_text(ctx, m, op, target, a.duration_s), /*html=*/true);
    }
}

// --- /unban (owner) /unmute (admin) ------------------------------------------

void on_unban(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const std::string ref = first_ref(args);
    const auto target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
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

void on_unmute(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
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

// --- warnings (admin) ---------------------------------------------------------
//   /warn  <ref|reply> [reason]
//   /dwarn [reason]                 (reply only, deletes the replied message)
//   /swarn <ref|reply> [reason]     silent
//   /unwarn (/rmwarn) <ref|reply>   removes the latest warn
//   /resetwarn <ref|reply>          removes all warns
//   /warnlist (/warns) <ref|reply>

std::string warn_limit_text(AppContext& ctx, const std::string& lang,
                            const services::WarnOutcome& r, const storage::UserRow& target) {
    const std::string key = "warns.limit_" + r.fired_action;
    if (r.fired_duration > 0) {
        return fmt::format(fmt::runtime(ctx.i18n.t(lang, key)), user_handle(target),
                           util::format_duration(r.fired_duration));
    }
    return fmt::format(fmt::runtime(ctx.i18n.t(lang, key)), user_handle(target));
}

void run_warn(AppContext& ctx, const tg::Message& m, const std::string& args, Flavor flavor,
              const std::string& command) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    std::optional<storage::UserRow> target;
    std::string reason;
    if (flavor == Flavor::Delete) {
        target = reply_target(ctx, m);
        if (!target) {
            reply_error(ctx, m, "errors.need_reply");
            return;
        }
        reason = util::trim(args);
    } else {
        const auto [ref, rsn] = split_ref_reason(args);
        target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
        reason = rsn;
    }
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

    if (flavor == Flavor::Delete && m.reply_to) {
        try_delete(ctx, m.chat->id, m.reply_to->message_id);
    }
    if (flavor == Flavor::Silent) {
        try_delete(ctx, m.chat->id, m.message_id);
        if (m.reply_to) try_delete(ctx, m.chat->id, m.reply_to->message_id);
        return;
    }
    if (!success_answers_on(ctx, m, command)) return;

    const std::string lang = chat_lang(ctx, m);
    if (!r.fired_action.empty()) {
        // Limit reached and the automatic action fired (spec §5.6).
        reply(ctx, m, warn_limit_text(ctx, lang, r, *target), /*html=*/true);
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
    const auto target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
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

void on_resetwarn(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    const services::Role role = ctx.roles->resolve(m.chat->id, to_user(*target));
    const auto r = ctx.warn_service->reset(m.chat->id, m.from->id, target->id, role);
    if (!r.ok) {
        reply_error(ctx, m, r.error_key);
        return;
    }
    if (success_answers_on(ctx, m, "resetwarn")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "warns.reset")),
                          user_handle(*target), r.limit),
              /*html=*/true);
    }
}

void on_warnlist(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string ref = first_ref(args);
    const auto target = ref.empty() ? reply_target(ctx, m) : resolve_target(ctx, m, ref);
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
    using services::Role;
    const auto restriction = [&ctx](const std::string& name, Op op, Flavor flavor) {
        ctx.router.add(name, [&ctx, name, op, flavor](const tg::Message& m,
                                                      const std::string& a) {
            run_restriction(ctx, m, a, op, flavor, name);
        }, required_role(op));
    };
    restriction("ban", Op::Ban, Flavor::Normal);
    restriction("tban", Op::TBan, Flavor::Normal);
    restriction("dban", Op::Ban, Flavor::Delete);
    restriction("sban", Op::Ban, Flavor::Silent);
    restriction("mute", Op::Mute, Flavor::Normal);
    restriction("tmute", Op::TMute, Flavor::Normal);
    restriction("dmute", Op::Mute, Flavor::Delete);
    restriction("smute", Op::Mute, Flavor::Silent);
    restriction("kick", Op::Kick, Flavor::Normal);
    restriction("dkick", Op::Kick, Flavor::Delete);
    restriction("skick", Op::Kick, Flavor::Silent);

    ctx.router.add("unban", [&ctx](const tg::Message& m, const std::string& a) {
        on_unban(ctx, m, a);
    }, Role::Owner);
    ctx.router.add("unmute", [&ctx](const tg::Message& m, const std::string& a) {
        on_unmute(ctx, m, a);
    }, Role::Admin);

    const auto warn = [&ctx](const std::string& name, Flavor flavor) {
        ctx.router.add(name, [&ctx, name, flavor](const tg::Message& m, const std::string& a) {
            run_warn(ctx, m, a, flavor, name);
        }, Role::Admin);
    };
    warn("warn", Flavor::Normal);
    warn("dwarn", Flavor::Delete);
    warn("swarn", Flavor::Silent);

    for (const char* name : {"unwarn", "rmwarn"}) {
        ctx.router.add(name, [&ctx](const tg::Message& m, const std::string& a) {
            on_unwarn(ctx, m, a);
        }, Role::Admin);
    }
    ctx.router.add("resetwarn", [&ctx](const tg::Message& m, const std::string& a) {
        on_resetwarn(ctx, m, a);
    }, Role::Admin);
    for (const char* name : {"warnlist", "warns"}) {
        ctx.router.add(name, [&ctx](const tg::Message& m, const std::string& a) {
            on_warnlist(ctx, m, a);
        }, Role::Admin);
    }
}

}  // namespace yacm::handlers