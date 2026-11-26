#include "services/antiflood.hpp"

#include <iterator>

namespace yacm::services {

FloodTracker::Verdict FloodTracker::record(int64_t chat_id, int64_t user_id,
                                           int64_t message_id, int64_t now,
                                           const storage::FloodSettings& settings) {
    Verdict v;
    if (!settings.enabled()) {
        chats_.erase(chat_id);  // antiflood turned off: drop stale state
        return v;
    }
    ChatState& st = chats_[chat_id];

    // --- consecutive messages -------------------------------------------
    if (st.last_user != user_id) {
        st.last_user = user_id;
        st.run.clear();
    }
    st.run.push_back(message_id);
    if (settings.limit > 0 && st.run.size() > static_cast<std::size_t>(settings.limit)) {
        v.triggered = true;
        v.flood_ids = st.run;
        v.excess_ids.assign(st.run.begin() + settings.limit, st.run.end());
    } else if (settings.limit <= 0) {
        st.run.clear();  // only the timed check is active
        st.run.push_back(message_id);
    }

    // --- N messages within S seconds --------------------------------------
    const bool timer_on = settings.timer_count > 0 && settings.timer_seconds > 0;
    if (timer_on) {
        auto& window = st.windows[user_id];
        window.emplace_back(now, message_id);
        while (!window.empty() && window.front().first <= now - settings.timer_seconds) {
            window.pop_front();
        }
        if (!v.triggered && window.size() > static_cast<std::size_t>(settings.timer_count)) {
            v.triggered = true;
            for (const auto& [ts, id] : window) v.flood_ids.push_back(id);
            for (std::size_t i = static_cast<std::size_t>(settings.timer_count);
                 i < window.size(); ++i) {
                v.excess_ids.push_back(window[i].second);
            }
        }
        if (st.windows.size() > kMaxTrackedUsers) sweep(st, now, settings.timer_seconds);
    } else {
        st.windows.clear();
    }

    if (v.triggered) forget(chat_id, user_id);
    return v;
}

void FloodTracker::forget(int64_t chat_id, int64_t user_id) {
    auto it = chats_.find(chat_id);
    if (it == chats_.end()) return;
    ChatState& st = it->second;
    if (st.last_user == user_id) {
        st.last_user = 0;
        st.run.clear();
    }
    st.windows.erase(user_id);
}

void FloodTracker::sweep(ChatState& state, int64_t now, int64_t window) {
    for (auto it = state.windows.begin(); it != state.windows.end();) {
        auto& q = it->second;
        while (!q.empty() && q.front().first <= now - window) q.pop_front();
        it = q.empty() ? state.windows.erase(it) : std::next(it);
    }
}

}  // namespace yacm::services
