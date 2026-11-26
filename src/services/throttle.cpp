#include "services/throttle.hpp"

#include <iterator>

namespace yacm::services {

CommandThrottle::CommandThrottle(int max_commands, int64_t window_ms, int64_t block_ms)
    : max_commands_(max_commands), window_ms_(window_ms), block_ms_(block_ms) {}

bool CommandThrottle::blocked(int64_t user_id, int64_t now_ms) const {
    const auto it = users_.find(user_id);
    return it != users_.end() && now_ms < it->second.blocked_until;
}

bool CommandThrottle::allow(int64_t user_id, int64_t now_ms) {
    UserState& st = users_[user_id];
    if (now_ms < st.blocked_until) return false;

    st.times.push_back(now_ms);
    while (!st.times.empty() && st.times.front() <= now_ms - window_ms_) st.times.pop_front();
    if (static_cast<int>(st.times.size()) > max_commands_) {
        st.blocked_until = now_ms + block_ms_;
        st.times.clear();
        return false;
    }
    if (users_.size() > 10000) sweep(now_ms);
    return true;
}

void CommandThrottle::sweep(int64_t now_ms) {
    for (auto it = users_.begin(); it != users_.end();) {
        const UserState& st = it->second;
        const bool idle = now_ms >= st.blocked_until &&
                          (st.times.empty() || st.times.back() <= now_ms - window_ms_);
        it = idle ? users_.erase(it) : std::next(it);
    }
}

}  // namespace yacm::services
