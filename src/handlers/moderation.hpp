#pragma once

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Registers moderation commands: /ban /unban /kick /mute /unmute
// /warn /unwarn /warnlist (spec §5).
void register_moderation(app::AppContext& ctx);

}  // namespace yacm::handlers