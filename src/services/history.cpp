#include "services/history.hpp"

#include <utility>

namespace yacm::services {
namespace {

std::string utf8_truncate(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    s.resize(end);
    return s + "…";
}

}  // namespace

ChatHistory::ChatHistory(std::size_t per_chat) : per_chat_(per_chat) {}

void ChatHistory::record(int64_t chat_id, HistoryEntry entry) {
    if (entry.text.empty() || per_chat_ == 0) return;
    entry.text = utf8_truncate(std::move(entry.text), kMaxTextBytes);
    std::lock_guard<std::mutex> lk(mu_);
    auto& q = chats_[chat_id];
    q.push_back(std::move(entry));
    while (q.size() > per_chat_) q.pop_front();
}

std::vector<HistoryEntry> ChatHistory::recent(int64_t chat_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = chats_.find(chat_id);
    if (it == chats_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

}  // namespace yacm::services
