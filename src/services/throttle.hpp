#pragma once

#include <cstdint>
#include <deque>
#include <unordered_map>

namespace yacm::services {

// Command spam protection: a user who sends more than `max_commands`
// commands within `window_ms` is ignored for `block_ms` (every chat).
// Times are milliseconds from any monotonic clock. Polling thread only.
class CommandThrottle {
public:
    CommandThrottle(int max_commands = 5, int64_t window_ms = 2000, int64_t block_ms = 10000);

    // Records a command; false when it must be ignored (the user is, or
    // just became, blocked). Attempts while blocked don't extend the block.
    bool allow(int64_t user_id, int64_t now_ms);

    [[nodiscard]] bool blocked(int64_t user_id, int64_t now_ms) const;

private:
    struct UserState {
        std::deque<int64_t> times;
        int64_t blocked_until = 0;
    };

    void sweep(int64_t now_ms);

    int max_commands_;
    int64_t window_ms_;
    int64_t block_ms_;
    std::unordered_map<int64_t, UserState> users_;
};

}  // namespace yacm::services
