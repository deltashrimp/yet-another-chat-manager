#pragma once

namespace yacm::tg {
struct Message;
}

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Creates the AI assistant and its worker thread when AI_API_KEY is set,
// and registers /ai (everyone), /aichat /aireply (owner) and /aireload
// (bot owner). Commands answer "AI is not configured" when it is off.
void register_ai(app::AppContext& ctx);

// Answers a plain (non-command) message with AI when appropriate: every
// message in a private chat, and in groups with /aireply on, messages that
// mention the bot or reply to it. Returns true when a request was queued.
bool maybe_ai_reply(app::AppContext& ctx, const tg::Message& m);

}  // namespace yacm::handlers
