#pragma once

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Registers general commands: /start /help /ping /version /id /idg /admins
// /rules /report.
void register_general(app::AppContext& ctx);

}  // namespace yacm::handlers