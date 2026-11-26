#pragma once

#include "storage/repos.hpp"

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

namespace yacm::services {

// In-memory antiflood detector (Rose-style), fed with every group message.
// Two independent checks, either of which may be off:
//   * consecutive: more than `limit` messages in a row from one user, with
//     nobody else posting in between (/setflood);
//   * timed: more than `timer_count` messages from one user within
//     `timer_seconds` (/setfloodtimer).
// Not thread-safe: called from the single polling thread only. State is
// lost on restart, which is harmless for flood detection.
class FloodTracker {
public:
    struct Verdict {
        bool triggered = false;
        // Every message of the detected flood (deleted when /clearflood is on).
        std::vector<int64_t> flood_ids;
        // Only the messages past the limit (deleted otherwise).
        std::vector<int64_t> excess_ids;
    };

    Verdict record(int64_t chat_id, int64_t user_id, int64_t message_id, int64_t now,
                   const storage::FloodSettings& settings);

    // Forgets a user's flood state in a chat (after punishment).
    void forget(int64_t chat_id, int64_t user_id);

    // Upper bound for remembered users per chat before stale ones are
    // swept (keeps memory bounded in huge chats).
    static constexpr std::size_t kMaxTrackedUsers = 2000;

private:
    struct ChatState {
        int64_t last_user = 0;
        std::vector<int64_t> run;  // message ids of last_user's current streak
        // user -> (timestamp, message id) inside the timer window
        std::unordered_map<int64_t, std::deque<std::pair<int64_t, int64_t>>> windows;
    };

    static void sweep(ChatState& state, int64_t now, int64_t window);

    std::unordered_map<int64_t, ChatState> chats_;
};

}  // namespace yacm::services
