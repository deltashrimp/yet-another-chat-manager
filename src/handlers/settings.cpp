#include "handlers/settings.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "router/router.hpp"
#include "storage/repos.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// --- /warnsetnumber /warnsetaction (owner, spec §5.6) ---------------------

void on_warnsetnumber(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    const auto limit = util::parse_warn_limit(util::trim(args));
    if (!limit) {
        reply_error(ctx, m, "errors.invalid_warn_limit");
        return;
    }
    ctx.chats->set_warn_limit(m.chat->id, *limit);
    if (!success_answers_on(ctx, m, "warnsetnumber")) return;
    reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "settings.warn_limit_set")),
                              *limit));
}

void on_warnsetaction(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    const auto action = util::parse_action(args);
    if (!action || (action->timed() &&
                    !services::ModerationService::clamp_duration(action->duration_s))) {
        reply_error(ctx, m, "errors.invalid_warn_action");
        return;
    }
    ctx.chats->set_warn_action(m.chat->id, action->name(), action->duration_s);
    if (!success_answers_on(ctx, m, "warnsetaction")) return;
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(chat_lang(ctx, m), "settings.warn_action_set")),
                      action->describe()));
}

// /warnings (admin, group): the chat's warn configuration.
void on_warnings(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Admin)) return;
    if (!m.chat) return;

    const std::string lang = chat_lang(ctx, m);
    const storage::ChatSettings s = ctx.chats->settings(m.chat->id);
    const util::Action action = util::action_from_db(s.warn_action, s.warn_action_duration);
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, "warns.config")), s.warn_limit,
                      action.describe()));
}

// --- /lang (owner, spec §6.1) ----------------------------------------------

void on_lang(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    const auto lang = util::parse_lang_token(args);
    if (!lang || !ctx.i18n.is_supported(*lang)) {
        std::string available;
        for (const auto& l : ctx.i18n.supported_langs()) {
            if (!available.empty()) available += ", ";
            available += l;
        }
        const std::string key = ctx.i18n.t(chat_lang(ctx, m), "errors.invalid_lang");
        reply(ctx, m, fmt::format(fmt::runtime(key), available));
        return;
    }
    ctx.chats->set_language(m.chat->id, *lang);

    // Confirm in the NEW language so the switch is immediately visible.
    const std::string name = ctx.i18n.t(*lang, "lang.name." + *lang);
    if (!success_answers_on(ctx, m, "lang")) return;
    reply(ctx, m, fmt::format(fmt::runtime(ctx.i18n.t(*lang, "settings.lang_changed")), name));
}

// --- /ruleset /rulereset (owner, spec §5.1) --------------------------------

void on_ruleset(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    if (!m.reply_to || m.reply_to->text.empty()) {
        reply_error(ctx, m, "errors.rules_need_reply");
        return;
    }
    // Stored raw; escaped on display (the parser does not reconstruct
    // entities, so the plain text is the source of truth).
    ctx.chats->set_rules(m.chat->id, m.reply_to->text);

    const std::string lang = chat_lang(ctx, m);
    const std::string link = util::message_link(m.chat->id, m.reply_to->message_id);
    if (!success_answers_on(ctx, m, "ruleset")) return;
    std::string text;
    if (link.empty()) {
        text = ctx.i18n.t(lang, "rules.updated_plain");
    } else {
        const std::string anchor =
            "<a href=\"" + link + "\">" + ctx.i18n.t(lang, "rules.this") + "</a>";
        text = fmt::format(fmt::runtime(ctx.i18n.t(lang, "rules.updated")), anchor);
    }
    reply(ctx, m, text, /*html=*/true);
}

void on_rulereset(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    ctx.chats->clear_rules(m.chat->id);
    if (!success_answers_on(ctx, m, "rulereset")) return;
    reply(ctx, m, ctx.i18n.t(chat_lang(ctx, m), "rules.cleared"));
}

// --- /reports (owner, spec §5.7) ------------------------------------------

void on_reports(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    const auto enabled = util::parse_yes_no(args);
    if (!enabled) {
        reply_error(ctx, m, "errors.invalid_yes_no");
        return;
    }
    ctx.chats->set_reports_enabled(m.chat->id, *enabled);
    if (!success_answers_on(ctx, m, "reports")) return;
    const std::string lang = chat_lang(ctx, m);
    const std::string state = ctx.i18n.t(lang, *enabled ? "reports.on" : "reports.off");
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.reports_set")), state));
}

// --- /answer /settings (owner, spec §6.2 / §5.8) --------------------------

// Spec §6.2: success answers can't be toggled for /answer (and the
// /answerlist listing) or for any "General commands" command (§4): start,
// help, ping, version, id, idg, admins, rules, report.
bool is_answer_forbidden(const std::string& command) {
    static const std::vector<std::string> forbidden = {
        "answer", "answerlist", "start", "help", "ping", "version", "id",
        "idg", "admins", "rules", "report",
        // read-only views and silent actions never send a success answer
        "settings", "warnings", "warnlist", "warns", "flood",
        "sban", "smute", "skick", "swarn",
        // the AI reply is the answer itself
        "ai", "aireload", "aidebug", "gbban", "ungbban",
    };
    return std::find(forbidden.begin(), forbidden.end(), command) != forbidden.end();
}

// /answer <command> <yes/no y/n on/off true/false> (owner, group): toggles
// the per-chat success replies for a moderation command. Errors are always
// shown regardless of the toggle.
void on_answer(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;
    const std::string lang = chat_lang(ctx, m);

    // "<command> <value>"; the command may be given as "/warn" or "warn".
    std::string s = util::trim(args);
    const std::size_t sp = s.find_first_of(" \t");
    std::string command = sp == std::string::npos ? s : s.substr(0, sp);
    const std::string value =
        util::trim(sp == std::string::npos ? std::string{} : s.substr(sp + 1));
    if (command.empty() || value.empty()) {
        reply_error(ctx, m, "errors.answer_need_args");
        return;
    }
    if (command.front() == '/') command = command.substr(1);
    command = to_lower(command);

    if (!ctx.router.find(command)) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "errors.answer_unknown_command")),
                          command));
        return;
    }
    if (is_answer_forbidden(command)) {
        reply(ctx, m,
              fmt::format(fmt::runtime(ctx.i18n.t(lang, "errors.answer_forbidden_command")),
                          command));
        return;
    }

    const auto enabled = util::parse_yes_no(value);
    if (!enabled) {
        reply_error(ctx, m, "errors.invalid_yes_no");
        return;
    }
    ctx.answers->set(m.chat->id, command, *enabled);
    const std::string state = ctx.i18n.t(lang, *enabled ? "reports.on" : "reports.off");
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.answer_set")), state, command));
}

// /settings (owner, group, spec §5.8): snapshot of the chat's settings.
void on_settings(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;

    const std::string lang = chat_lang(ctx, m);
    const storage::ChatSettings s = ctx.chats->settings(m.chat->id);

    const std::string lang_name = ctx.i18n.t(lang, "lang.name." + s.language);
    const std::string reports =
        ctx.i18n.t(lang, s.reports_enabled ? "reports.on" : "reports.off");
    const std::string rules = ctx.i18n.t(
        lang, s.rules_html.empty() ? "settings.rules_not_set" : "settings.rules_set");

    const std::string title =
        util::chat_display(*m.chat, ctx.i18n.t(lang, "idg.untitled"));
    std::string text =
        fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.title")), title) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.language")), lang_name) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.reports")), reports) + "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.warn_limit")), s.warn_limit) +
            "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.warn_action")),
                        util::action_from_db(s.warn_action, s.warn_action_duration)
                            .describe()) +
            "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.flood")),
                        ctx.i18n.t(lang, s.flood.enabled() ? "reports.on" : "reports.off")) +
            "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.ai")),
                        ctx.i18n.t(lang, s.ai_enabled ? "reports.on" : "reports.off")) +
            "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.ai_autoreply")),
                        ctx.i18n.t(lang, s.ai_autoreply ? "reports.on" : "reports.off")) +
            "\n";
    text += fmt::format(fmt::runtime(ctx.i18n.t(lang, "settings.rules")), rules);
    reply(ctx, m, text, /*html=*/true);
}

// /answerlist (owner, group): lists every toggleable command with its
// success-answers state. Companion to /answer — the per-command detail that
// /settings' old "Answers > on/off" boolean couldn't show.
void on_answerlist(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    if (!m.chat) return;
    const std::string lang = chat_lang(ctx, m);

    const std::string title =
        util::chat_display(*m.chat, ctx.i18n.t(lang, "idg.untitled"));
    std::vector<std::string> lines;
    lines.push_back(fmt::format(fmt::runtime(ctx.i18n.t(lang, "answerlist.title")), title));
    // Enumerate all registered commands (BotOwner outranks everything; the
    // router returns them alphabetically) and drop the non-toggleable ones,
    // so the list stays in sync with registration automatically.
    for (const std::string& command :
         ctx.router.commands_for(services::Role::BotOwner, /*in_private_chat=*/true)) {
        if (is_answer_forbidden(command)) continue;
        const std::string state =
            ctx.i18n.t(lang, ctx.answers->enabled(m.chat->id, command) ? "reports.on"
                                                                       : "reports.off");
        lines.push_back(fmt::format(fmt::runtime(ctx.i18n.t(lang, "answerlist.line")),
                                    command, state));
    }
    reply(ctx, m, fmt::format("{}", fmt::join(lines, "\n")), /*html=*/true);
}

}  // namespace

void register_settings(AppContext& ctx) {
    // Rose names (/setwarnlimit, /setwarnmode) are aliases of the originals.
    for (const char* name : {"warnsetnumber", "setwarnlimit"}) {
        ctx.router.add(name, [&ctx](const tg::Message& m, const std::string& a) {
            on_warnsetnumber(ctx, m, a);
        }, services::Role::Owner);
    }
    for (const char* name : {"warnsetaction", "setwarnmode"}) {
        ctx.router.add(name, [&ctx](const tg::Message& m, const std::string& a) {
            on_warnsetaction(ctx, m, a);
        }, services::Role::Owner);
    }
    ctx.router.add("warnings", [&ctx](const tg::Message& m, const std::string& a) {
        on_warnings(ctx, m, a);
    }, services::Role::Admin);
    ctx.router.add("lang", [&ctx](const tg::Message& m, const std::string& a) {
        on_lang(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("ruleset", [&ctx](const tg::Message& m, const std::string& a) {
        on_ruleset(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("rulereset", [&ctx](const tg::Message& m, const std::string& a) {
        on_rulereset(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("reports", [&ctx](const tg::Message& m, const std::string& a) {
        on_reports(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("answer", [&ctx](const tg::Message& m, const std::string& a) {
        on_answer(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("answerlist", [&ctx](const tg::Message& m, const std::string& a) {
        on_answerlist(ctx, m, a);
    }, services::Role::Owner);
    ctx.router.add("settings", [&ctx](const tg::Message& m, const std::string& a) {
        on_settings(ctx, m, a);
    }, services::Role::Owner);
}

}  // namespace yacm::handlers