#include "handlers/antiflood.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "services/antiflood.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "storage/repos.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <string>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

// Telegram's "GroupAnonymousBot": messages of anonymous admins.
constexpr int64_t kAnonymousAdminId = 1087968824;

constexpr int kMinFlood = 2;
constexpr int kMaxFlood = 100;
constexpr int64_t kMaxFloodWindow = 24 * 3600;

std::string on_off(AppContext& ctx, const std::string& lang, bool on) {
    return ctx.i18n.t(lang, on ? "reports.on" : "reports.off");
}

// /flood (admin): current antiflood configuration.
void on_flood(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const std::string lang = chat_lang(ctx, m);
    const storage::FloodSettings f = ctx.chats->flood_settings(m.chat->id);
    if (!f.enabled()) {
        reply(ctx, m, ctx.i18n.t(lang, "flood.disabled"));
        return;
    }
    const std::string limit = f.limit > 0 ? std::to_string(f.limit) : on_off(ctx, lang, false);
    const std::string timer =
        f.timer_count > 0 && f.timer_seconds > 0
            ? fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.timer_value")), f.timer_count,
                          util::format_duration(f.timer_seconds))
            : on_off(ctx, lang, false);
    const std::string action = util::action_from_db(f.action, f.action_duration).describe();

    std::string text = ctx.i18n.t(lang, "flood.title") + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.limit")), limit) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.timer")), timer) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.mode")), action) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.clear")), on_off(ctx, lang, f.clear));
    reply(ctx, m, text);
}

// /setflood <number|off> (owner): consecutive-message limit.
void on_setflood(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const std::string lang = chat_lang(ctx, m);
    const std::string arg = util::trim(args);
    const auto off = util::parse_yes_no(arg);
    if ((off && !*off) || arg == "0") {
        ctx.chats->set_flood_limit(m.chat->id, 0);
        if (success_answers_on(ctx, m, "setflood")) {
            reply(ctx, m, ctx.i18n.t(lang, "flood.limit_off"));
        }
        return;
    }
    const auto n = util::parse_int_in_range(arg, kMinFlood, kMaxFlood);
    if (!n) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "errors.invalid_flood_limit")),
                          kMinFlood, kMaxFlood));
        return;
    }
    ctx.chats->set_flood_limit(m.chat->id, *n);
    if (success_answers_on(ctx, m, "setflood")) {
        reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.limit_set")), *n));
    }
}

// /setfloodtimer <count> <duration> | off (owner): N messages within S.
void on_setfloodtimer(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const std::string lang = chat_lang(ctx, m);
    const std::string arg = util::trim(args);
    const auto off = util::parse_yes_no(arg);
    if ((off && !*off) || arg == "0") {
        ctx.chats->set_flood_timer(m.chat->id, 0, 0);
        if (success_answers_on(ctx, m, "setfloodtimer")) {
            reply(ctx, m, ctx.i18n.t(lang, "flood.timer_off"));
        }
        return;
    }

    const std::size_t sp = arg.find_first_of(" \t");
    const auto count = util::parse_int_in_range(arg.substr(0, sp), kMinFlood, kMaxFlood);
    const auto window = sp == std::string::npos
                            ? std::nullopt
                            : util::parse_duration_seconds(util::trim(arg.substr(sp + 1)));
    if (!count || !window || *window > kMaxFloodWindow) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "errors.invalid_flood_timer")),
                          kMinFlood, kMaxFlood));
        return;
    }
    ctx.chats->set_flood_timer(m.chat->id, *count, *window);
    if (success_answers_on(ctx, m, "setfloodtimer")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.timer_set")), *count,
                          util::format_duration(*window)));
    }
}

// /setfloodmode <ban|kick|mute|tban <t>|tmute <t>> (owner).
void on_setfloodmode(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const auto action = util::parse_action(args);
    if (!action || (action->timed() &&
                    !services::ModerationService::clamp_duration(action->duration_s))) {
        reply_error(ctx, m, "errors.invalid_flood_mode");
        return;
    }
    ctx.chats->set_flood_action(m.chat->id, action->name(), action->duration_s);
    if (success_answers_on(ctx, m, "setfloodmode")) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "flood.mode_set")),
                          action->describe()));
    }
}

// /clearflood <yes|no> (owner): delete the whole flood or only the excess.
void on_clearflood(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;

    const auto on = util::parse_yes_no(args);
    if (!on) {
        reply_error(ctx, m, "errors.invalid_yes_no");
        return;
    }
    ctx.chats->set_flood_clear(m.chat->id, *on);
    if (success_answers_on(ctx, m, "clearflood")) {
        const std::string lang = chat_lang(ctx, m);
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "flood.clear_set")),
                          on_off(ctx, lang, *on)));
    }
}

}  // namespace

bool check_flood(AppContext& ctx, const tg::Message& m) {
    if (!m.chat || !m.chat->is_group() || !m.from) return false;
    if (m.from->id == ctx.bot_id || m.from->id == kAnonymousAdminId) return false;
    // Service messages (joins/leaves) are not "messages" for flood purposes.
    if (!m.new_chat_members.empty() || m.left_chat_member) return false;

    const storage::FloodSettings settings = ctx.chats->flood_settings(m.chat->id);
    if (!settings.enabled()) return false;

    const auto verdict =
        ctx.flood.record(m.chat->id, m.from->id, m.message_id, m.date, settings);
    if (!verdict.triggered) return false;

    // Role checks are only paid for when a flood is detected: admins (and
    // the owner) may flood freely.
    const services::Role role = ctx.roles->resolve(m.chat->id, *m.from);
    if (role != services::Role::Everyone) return false;

    const util::Action action = util::action_from_db(settings.action, settings.action_duration);
    const auto r = ctx.moderation->apply(m.chat->id, ctx.bot_id, m.from->id, role, action,
                                         "flood");
    for (const int64_t id : settings.clear ? verdict.flood_ids : verdict.excess_ids) {
        try_delete(ctx, m.chat->id, id);
    }
    if (!r.ok) {
        spdlog::warn("antiflood action {} failed for {} in {}: {}", action.describe(),
                     m.from->id, m.chat->id, r.error_key);
        return true;
    }

    const std::string lang = chat_lang(ctx, m);
    const std::string key = "flood.punished_" + action.name();
    const std::string who = util::user_tag(*m.from);
    const std::string text =
        action.timed()
            ? fmt::format(fmt::runtime(ctx.i18n.t(lang, key)), who,
                          util::format_duration(action.duration_s))
            : fmt::format(fmt::runtime(ctx.i18n.t(lang, key)), who);
    // Not a reply: the triggering message may just have been deleted.
    reply(ctx, m, text, /*html=*/true, /*reply_to=*/false);
    return true;
}

void register_antiflood(AppContext& ctx) {
    using services::Role;
    ctx.router.add("flood", [&ctx](const tg::Message& m, const std::string& a) {
        on_flood(ctx, m, a);
    }, Role::Admin);
    ctx.router.add("setflood", [&ctx](const tg::Message& m, const std::string& a) {
        on_setflood(ctx, m, a);
    }, Role::Owner);
    ctx.router.add("setfloodtimer", [&ctx](const tg::Message& m, const std::string& a) {
        on_setfloodtimer(ctx, m, a);
    }, Role::Owner);
    ctx.router.add("setfloodmode", [&ctx](const tg::Message& m, const std::string& a) {
        on_setfloodmode(ctx, m, a);
    }, Role::Owner);
    ctx.router.add("clearflood", [&ctx](const tg::Message& m, const std::string& a) {
        on_clearflood(ctx, m, a);
    }, Role::Owner);
}

}  // namespace yacm::handlers
