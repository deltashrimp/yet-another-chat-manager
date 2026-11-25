#include "handlers/general.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "router/router.hpp"
#include "services/roles.hpp"
#include "storage/repos.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void on_start(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_private(ctx, m)) return;
    reply(ctx, m, ctx.welcome_message);
}

void on_help(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    const std::string lang = chat_lang(ctx, m);
    const bool in_private = m.chat && m.chat->is_private();

    services::Role role = services::Role::Everyone;
    if (m.chat && m.from) role = ctx.roles->resolve(m.chat->id, *m.from);

    const std::string title =
        in_private ? ctx.i18n.t(lang, "help.dm_title") : ctx.i18n.t(lang, "help.title");

    std::string body;
    for (const auto& name : ctx.router.commands_for(role, in_private)) {
        body += "/" + name + " — " + ctx.i18n.t(lang, "commands." + name) + "\n";
    }
    if (body.empty()) body = ctx.i18n.t(lang, "help.none");

    reply(ctx, m, title + "\n" + body);
}

void on_ping(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    reply(ctx, m, ctx.i18n.t(chat_lang(ctx, m), "general.pong"));
}

void on_version(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    const std::string template_ = ctx.i18n.t(chat_lang(ctx, m), "general.version");
    reply(ctx, m, fmt::format(fmt::runtime(template_), ctx.version, ctx.build_date));
}

void on_id(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;

    const auto target = resolve_user_ref(ctx, m, args);
    if (!target) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }

    const std::string lang = chat_lang(ctx, m);
    const std::string text = ctx.i18n.t(lang, "id.user") + user_handle(*target) + "\n" +
                             ctx.i18n.t(lang, "id.id") + std::to_string(target->id);
    reply(ctx, m, text, /*html=*/true);
}

void on_idg(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;
    if (!m.chat) return;

    const std::string lang = chat_lang(ctx, m);
    const std::string title = m.chat->title.empty() ? ctx.i18n.t(lang, "idg.untitled")
                                                    : m.chat->title;
    const std::string text = ctx.i18n.t(lang, "idg.group") + util::html_escape(title) +
                             "\n" + ctx.i18n.t(lang, "idg.id") + std::to_string(m.chat->id);
    reply(ctx, m, text, /*html=*/true);
}

int admin_rank(const std::string& status) {
    if (status == "creator") return 2;
    if (status == "administrator") return 1;
    return 0;
}

// Spec §4.4: list of current chat admins (everyone). Fetches the live list
// and write-through caches it (keeps the role-check cache warm); falls back
// to the cached `members` rows only when the API call fails.
void on_admins(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!m.chat) return;
    const std::string lang = chat_lang(ctx, m);

    std::vector<tg::ChatMember> admins;
    bool live_ok = false;
    try {
        admins = ctx.client->get_chat_administrators(m.chat->id);
        live_ok = true;
        const int64_t now = unix_now();
        for (const auto& a : admins) {
            ctx.users->upsert(a.user, now);
            ctx.users->upsert_member(m.chat->id, a.user.id, a.status, now, std::nullopt);
        }
    } catch (const std::exception& e) {
        spdlog::warn("getChatAdministrators failed for {}: {}", m.chat->id, e.what());
    }

    std::string lines;
    if (live_ok) {
        std::stable_sort(admins.begin(), admins.end(),
                         [](const tg::ChatMember& a, const tg::ChatMember& b) {
                             return admin_rank(a.status) > admin_rank(b.status);
                         });
        for (const auto& a : admins) lines += "> " + util::user_tag(a.user) + "\n";
    } else {
        const auto cached = ctx.users->admins(m.chat->id);
        if (cached.empty()) {
            reply_error(ctx, m, "admins.failed");
            return;
        }
        for (const auto& row : cached) lines += "> " + user_handle(row) + "\n";
    }

    reply(ctx, m, ctx.i18n.t(lang, "admins.title") + "\n" + lines, /*html=*/true);
}

// Spec §4.5: current chat rules (everyone). Rules are stored raw; escaped
// on display (HTML parse mode).
void on_rules(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!m.chat) return;
    const std::string lang = chat_lang(ctx, m);

    const std::string rules = ctx.chats->rules_text(m.chat->id);
    if (rules.empty()) {
        reply(ctx, m, ctx.i18n.t(lang, "rules.not_set"));
        return;
    }
    reply(ctx, m, ctx.i18n.t(lang, "rules.title") + "\n" + util::html_escape(rules),
          /*html=*/true);
}

// Spec §4.6: /report (everyone, group) — forwards a complaint about the
// replied-to message to the chat admins via DM. Guards: no self-reports
// (errors.report_self), §2 hierarchy (the bot/owner/admins cannot be
// reported), spec §3 dedup per (reporter, message) AND per (chat, reporter,
// target) so the same message/user can't be reported again. Delivery
// failures (an admin never opened a dialog with the bot) are logged by the
// sender; the loop continues to the remaining admins (spec §4.6 note).
void on_report(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!m.from || !m.chat) return;
    const std::string lang = chat_lang(ctx, m);

    if (!ctx.chats->reports_enabled(m.chat->id)) {
        reply_error(ctx, m, "errors.reports_disabled");
        return;
    }
    if (!m.reply_to) {
        reply_error(ctx, m, "errors.report_need_reply");
        return;
    }
    const tg::ReplyTo& reported = *m.reply_to;
    if (!reported.from) {
        reply_error(ctx, m, "errors.user_not_found");
        return;
    }
    if (m.from->id == reported.from->id) {
        reply_error(ctx, m, "errors.report_self");
        return;
    }
    // Spec §2 hierarchy applies to reports too: the bot itself, the chat
    // owner and other admins cannot be reported (they get the complaint DMs).
    if (reported.from->id == ctx.bot_id) {
        reply_error(ctx, m, "errors.cannot_report_bot");
        return;
    }
    const services::Role target_role = ctx.roles->resolve(m.chat->id, *reported.from);
    if (target_role == services::Role::Owner) {
        reply_error(ctx, m, "errors.cannot_report_owner");
        return;
    }
    if (target_role == services::Role::Admin) {
        reply_error(ctx, m, "errors.cannot_report_admin");
        return;
    }
    // Spec §3 dedup: once per (reporter, message) — a repeat of the same
    // message is not a new report (the ack says so instead of lying "Sent").
    if (ctx.reports->already_reported(m.chat->id, m.from->id, reported.message_id)) {
        reply_error(ctx, m, "errors.report_already");
        return;
    }
    // Same reporter cannot spam reports about the same user in this chat.
    if (ctx.reports->already_reported_target(m.chat->id, m.from->id,
                                             reported.from->id)) {
        reply_error(ctx, m, "errors.report_already_target");
        return;
    }

    // Recipients: live admin list (write-through cached), falling back to
    // the cached admins when the API call fails — same as /admins.
    std::vector<int64_t> admin_ids;
    bool live_ok = false;
    try {
        const auto admins = ctx.client->get_chat_administrators(m.chat->id);
        live_ok = true;
        const int64_t now = unix_now();
        for (const auto& a : admins) {
            ctx.users->upsert(a.user, now);
            ctx.users->upsert_member(m.chat->id, a.user.id, a.status, now, std::nullopt);
            // The bot itself is an admin when it manages the chat, but it
            // cannot DM itself — skip it (spec §2/§9).
            // Other bots can't receive DMs from bots either.
            if (a.user.id != ctx.bot_id && !a.user.is_bot) admin_ids.push_back(a.user.id);
        }
    } catch (const std::exception& e) {
        spdlog::warn("getChatAdministrators failed for {}: {}", m.chat->id, e.what());
    }
    if (!live_ok) {
        for (const auto& row : ctx.users->admins(m.chat->id)) {
            if (row.id != ctx.bot_id && !row.is_bot) admin_ids.push_back(row.id);
        }
    }
    if (admin_ids.empty()) {
        reply_error(ctx, m, "admins.failed");  // nowhere to deliver the report
        return;
    }

    const std::string reporter = util::user_tag(*m.from);
    const std::string target = util::user_tag(*reported.from);
    const std::string title =
        m.chat->title.empty() ? ctx.i18n.t(lang, "idg.untitled") : m.chat->title;
    const std::string link = util::message_link(m.chat->id, reported.message_id);
    const std::string text =
        link.empty()
            ? fmt::format(fmt::runtime(ctx.i18n.t(lang, "reports.admin_notice_plain")),
                          reporter, target, util::html_escape(title))
            : fmt::format(fmt::runtime(ctx.i18n.t(lang, "reports.admin_notice")), reporter,
                          target, util::html_escape(title), link);

    for (const int64_t id : admin_ids) {
        ctx.sender->enqueue(tg::Outgoing{id, text, "HTML", std::nullopt});
    }
    ctx.reports->add(m.chat->id, reported.message_id, m.from->id, reported.from->id,
                     unix_now());
    reply(ctx, m, ctx.i18n.t(lang, "reports.sent"));
}

}  // namespace

void register_general(AppContext& ctx) {
    ctx.router.add("start", [&ctx](const tg::Message& m, const std::string& a) {
        on_start(ctx, m, a);
    }, services::Role::Everyone, /*dm_only=*/true);
    ctx.router.add("help", [&ctx](const tg::Message& m, const std::string& a) {
        on_help(ctx, m, a);
    }, services::Role::Everyone);
    ctx.router.add("ping", [&ctx](const tg::Message& m, const std::string& a) {
        on_ping(ctx, m, a);
    }, services::Role::Everyone);
    ctx.router.add("version", [&ctx](const tg::Message& m, const std::string& a) {
        on_version(ctx, m, a);
    }, services::Role::Everyone);
    ctx.router.add("id", [&ctx](const tg::Message& m, const std::string& a) {
        on_id(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("idg", [&ctx](const tg::Message& m, const std::string& a) {
        on_idg(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("admins", [&ctx](const tg::Message& m, const std::string& a) {
        on_admins(ctx, m, a);
    }, services::Role::Everyone);
    ctx.router.add("rules", [&ctx](const tg::Message& m, const std::string& a) {
        on_rules(ctx, m, a);
    }, services::Role::Everyone);
    ctx.router.add("report", [&ctx](const tg::Message& m, const std::string& a) {
        on_report(ctx, m, a);
    }, services::Role::Everyone);
}

}  // namespace yacm::handlers