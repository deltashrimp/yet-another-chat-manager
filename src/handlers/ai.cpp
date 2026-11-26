#include "handlers/ai.hpp"

#include "app/app_context.hpp"
#include "handlers/common.hpp"
#include "services/ai.hpp"
#include "services/roles.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace yacm::handlers {
namespace {

using yacm::app::AppContext;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Builds the request with the chat's last messages (before `m`) as context.
// Images are never read; a photo's caption counts as its text.
ai::Request make_request(AppContext& ctx, const tg::Message& m, const std::string& text) {
    ai::Request r;
    if (m.chat) {
        if (!m.chat->is_private()) r.chat_title = m.chat->title;
        r.history = ctx.history.recent(m.chat->id);
    }
    if (m.from) r.user_name = util::author_handle(*m.from);
    r.text = text;
    if (m.reply_to && !m.reply_to->content().empty()) {
        r.replied_text = m.reply_to->content();
        r.replied_is_bot = m.reply_to->from && m.reply_to->from->id == ctx.bot_id;
    }
    return r;
}

std::string debug_footer(AppContext& ctx, const std::string& lang,
                         const ai::Worker::Result& result) {
    const auto n = [](int64_t v) { return v < 0 ? std::string("?") : std::to_string(v); };
    return fmt::format(fmt::runtime(ctx.i18n.t(lang, "ai.debug_footer")),
                       n(result.prompt_tokens), n(result.completion_tokens), result.elapsed_ms);
}

// Rate-limits and queues a request. Errors are only shown for explicit
// /ai requests; automatic replies fail silently.
bool submit(AppContext& ctx, const tg::Message& m, ai::Request request, bool explicit_request) {
    if (!ctx.ai_worker || !m.chat || !m.from) {
        if (explicit_request) reply_error(ctx, m, "ai.disabled");
        return false;
    }
    const bool exempt = ctx.config.bot_owner_id != 0 && m.from->id == ctx.config.bot_owner_id;
    if (!exempt && !ctx.ai_limiter->allow(m.chat->id, m.from->id, m.date)) {
        if (explicit_request) reply_error(ctx, m, "ai.cooldown");
        return false;
    }

    ai::Worker::Job job;
    job.chat_id = m.chat->id;
    job.reply_to_message_id = m.message_id;
    job.lang = chat_lang(ctx, m);
    job.explicit_request = explicit_request;
    job.request = std::move(request);
    if (!ctx.ai_worker->submit(std::move(job))) {
        if (explicit_request) reply_error(ctx, m, "ai.busy");
        return false;
    }
    try {
        ctx.client->send_chat_action(m.chat->id, "typing");
    } catch (const std::exception& e) {
        ctx.log->debug("sendChatAction failed in {}: {}", m.chat->id, e.what());
    }
    return true;
}

// Runs on the AI worker thread: only thread-safe pieces (sender queue,
// read-only i18n) are used here.
void deliver(AppContext& ctx, const ai::Worker::Job& job, const ai::Worker::Result& result) {
    const auto send = [&ctx, &job](std::string text, bool reply_to) {
        ctx.sender->enqueue(tg::Outgoing{
            job.chat_id, std::move(text), "",
            reply_to ? std::optional<int64_t>(job.reply_to_message_id) : std::nullopt});
    };
    const bool debug = ctx.ai_debug.load();
    const auto with_debug = [&](std::string text) {
        if (debug) text += "\n\n" + debug_footer(ctx, job.lang, result);
        return text;
    };
    if (result.failed) {
        if (job.explicit_request) send(with_debug(ctx.i18n.t(job.lang, "ai.failed")), true);
        return;
    }
    if (!result.answer) {
        if (job.explicit_request) send(with_debug(ctx.i18n.t(job.lang, "ai.no_answer")), true);
        return;
    }
    // The bot's own answer becomes context for the next request.
    ctx.history.record(job.chat_id,
                       {ctx.bot_username.empty() ? "Bot" : "@" + ctx.bot_username,
                        *result.answer});
    const auto parts = ai::split_message(with_debug(*result.answer));
    for (std::size_t i = 0; i < parts.size(); ++i) send(parts[i], i == 0);
}

// /ai <question> (everyone). Without a question, asks about the replied
// message.
void on_ai(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!m.chat) return;
    if (!m.chat->is_private() && !ctx.chats->ai_enabled(m.chat->id)) {
        reply_error(ctx, m, "ai.chat_disabled");
        return;
    }
    std::string question = util::trim(args);
    ai::Request request = make_request(ctx, m, question);
    if (question.empty()) {
        if (request.replied_text.empty()) {
            reply_error(ctx, m, "ai.usage");
            return;
        }
        // "/ai" as a reply: the replied message itself is the question.
        request.text = request.replied_text;
        request.replied_text.clear();
        request.replied_is_bot = false;
    }
    submit(ctx, m, std::move(request), /*explicit_request=*/true);
}

void toggle(AppContext& ctx, const tg::Message& m, const std::string& args,
            const std::string& command, void (storage::ChatRepo::*setter)(int64_t, bool),
            const std::string& reply_key) {
    if (!require_group(ctx, m)) return;
    if (!require_role(ctx, m, services::Role::Owner)) return;
    const auto on = util::parse_yes_no(args);
    if (!on) {
        reply_error(ctx, m, "errors.invalid_yes_no");
        return;
    }
    (ctx.chats.get()->*setter)(m.chat->id, *on);
    if (!success_answers_on(ctx, m, command)) return;
    const std::string lang = chat_lang(ctx, m);
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, reply_key)),
                      ctx.i18n.t(lang, *on ? "reports.on" : "reports.off")));
}

// /aidebug [on|off] (bot owner): toggles the "tokens in/out + time" footer
// on every AI answer, in every chat. Persisted across restarts.
void on_aidebug(AppContext& ctx, const tg::Message& m, const std::string& args) {
    if (!m.chat || !m.from) return;
    if (!require_role(ctx, m, services::Role::BotOwner)) return;

    bool on = !ctx.ai_debug.load();  // no argument: toggle
    if (!util::trim(args).empty()) {
        const auto v = util::parse_yes_no(args);
        if (!v) {
            reply_error(ctx, m, "errors.invalid_yes_no");
            return;
        }
        on = *v;
    }
    ctx.bot_settings->set_flag("ai_debug", on);
    ctx.ai_debug.store(on);
    const std::string lang = chat_lang(ctx, m);
    reply(ctx, m,
          fmt::format(fmt::runtime(ctx.i18n.t(lang, "ai.debug_set")),
                      ctx.i18n.t(lang, on ? "reports.on" : "reports.off")));
}

// /aireload (bot owner): re-reads AI_RULES / AI_BIO files.
void on_aireload(AppContext& ctx, const tg::Message& m, const std::string& /*args*/) {
    if (!m.chat || !m.from) return;
    if (!require_role(ctx, m, services::Role::BotOwner)) return;
    if (!ctx.assistant) {
        reply_error(ctx, m, "ai.disabled");
        return;
    }
    ctx.assistant->reload();
    reply(ctx, m, ctx.i18n.t(chat_lang(ctx, m), "ai.reloaded"));
}

}  // namespace

bool maybe_ai_reply(AppContext& ctx, const tg::Message& m) {
    if (!ctx.ai_worker || !m.chat || !m.from || m.from->is_bot) return false;
    if (m.content().empty() || m.is_command()) return false;

    if (!m.chat->is_private()) {
        if (!m.chat->is_group()) return false;
        const bool replied_to_bot =
            m.reply_to && m.reply_to->from && m.reply_to->from->id == ctx.bot_id;
        const bool mentioned = !ctx.bot_username.empty() &&
                               lower(m.content()).find("@" + lower(ctx.bot_username)) !=
                                   std::string::npos;
        if (!replied_to_bot && !mentioned) return false;
        if (!ctx.chats->ai_enabled(m.chat->id) || !ctx.chats->ai_autoreply(m.chat->id)) {
            return false;
        }
    }
    return submit(ctx, m, make_request(ctx, m, m.content()), /*explicit_request=*/false);
}

void register_ai(AppContext& ctx) {
    ctx.ai_debug.store(ctx.bot_settings->flag("ai_debug"));
    if (ctx.config.ai.enabled()) {
        ctx.ai_client = std::make_unique<ai::OpenAiClient>(ctx.config.ai);
        ctx.assistant = std::make_unique<ai::Assistant>(ctx.config.ai, *ctx.ai_client);
        ctx.ai_limiter = std::make_unique<ai::RateLimiter>(ctx.config.ai.user_cooldown_s,
                                                           ctx.config.ai.chat_per_min);
        ctx.ai_worker = std::make_unique<ai::Worker>(
            *ctx.assistant, *ctx.ai_client,
            [&ctx](const ai::Worker::Job& job, const ai::Worker::Result& result) {
                deliver(ctx, job, result);
            },
            static_cast<std::size_t>(ctx.config.ai.queue_limit), ctx.log);
        ctx.ai_worker->start();
        ctx.log->info("AI enabled: model={} url={}", ctx.config.ai.model, ctx.config.ai.api_url);
    } else {
        ctx.log->info("AI disabled (AI_API_KEY is not set)");
    }

    using services::Role;
    ctx.router.add("ai", [&ctx](const tg::Message& m, const std::string& a) {
        on_ai(ctx, m, a);
    }, Role::Everyone);
    ctx.router.add("aichat", [&ctx](const tg::Message& m, const std::string& a) {
        toggle(ctx, m, a, "aichat", &storage::ChatRepo::set_ai_enabled, "ai.chat_set");
    }, Role::Owner);
    ctx.router.add("aireply", [&ctx](const tg::Message& m, const std::string& a) {
        toggle(ctx, m, a, "aireply", &storage::ChatRepo::set_ai_autoreply, "ai.autoreply_set");
    }, Role::Owner);
    ctx.router.add("aidebug", [&ctx](const tg::Message& m, const std::string& a) {
        on_aidebug(ctx, m, a);
    }, Role::BotOwner);
    ctx.router.add("aireload", [&ctx](const tg::Message& m, const std::string& a) {
        on_aireload(ctx, m, a);
    }, Role::BotOwner);
}

}  // namespace yacm::handlers
