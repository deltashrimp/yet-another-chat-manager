#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace yacm::services {

struct HistoryEntry {
    std::string author;  // "@username", or the display name when there is none
    std::string text;
};

// Last few text messages of every chat, used as AI context. In memory
// only (empty after a restart). Thread-safe: the polling thread records
// users' messages, the AI worker records the bot's own answers.
class ChatHistory {
public:
    explicit ChatHistory(std::size_t per_chat = 5);

    void record(int64_t chat_id, HistoryEntry entry);
    // Oldest first.
    std::vector<HistoryEntry> recent(int64_t chat_id) const;

    // Longest stored text per message (bytes, UTF-8 safe).
    static constexpr std::size_t kMaxTextBytes = 1000;

private:
    std::size_t per_chat_;
    mutable std::mutex mu_;
    std::unordered_map<int64_t, std::deque<HistoryEntry>> chats_;
};

}  // namespace yacm::services
