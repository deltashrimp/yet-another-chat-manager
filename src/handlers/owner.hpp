#pragma once

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Bot-owner commands: /gbban /ungbban (global bot ban).
void register_owner(app::AppContext& ctx);

}  // namespace yacm::handlers
