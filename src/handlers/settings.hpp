#pragma once

namespace yacm::app {
class AppContext;
}

namespace yacm::handlers {

// Registers owner settings commands: /warnsetnumber /warnsetaction (spec
// §5.6), /ruleset /rulereset (spec §5.1), /lang (spec §6.1) and /reports
// (spec §5.7).
void register_settings(app::AppContext& ctx);

}  // namespace yacm::handlers