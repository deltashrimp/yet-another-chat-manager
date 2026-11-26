#pragma once

namespace yacm::tg {
struct Message;
}

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Registers /flood (admin) and /setflood /setfloodtimer /setfloodmode
// /clearflood (owner).
void register_antiflood(app::AppContext& ctx);

// Feeds a group message to the flood tracker and punishes the sender when
// the chat's antiflood limits are exceeded. Admins are exempt. Returns
// true when the message was part of a flood (it may have been deleted).
bool check_flood(app::AppContext& ctx, const tg::Message& m);

}  // namespace yacm::handlers
