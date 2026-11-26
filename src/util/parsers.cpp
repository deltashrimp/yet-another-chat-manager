#include "util/parsers.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>

namespace yacm::util {

std::string trim(std::string s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = std::find_if_not(s.begin(), s.end(), is_space);
    auto end = std::find_if_not(s.rbegin(), s.rend(), is_space).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

UserRef UserRef::from_id(int64_t i) {
    UserRef ref;
    ref.kind = Kind::Id;
    ref.id = i;
    return ref;
}

UserRef UserRef::from_username(std::string u) {
    UserRef ref;
    ref.kind = Kind::Username;
    ref.username = std::move(u);
    return ref;
}

UserRef UserRef::reply() {
    UserRef ref;
    ref.kind = Kind::Reply;
    return ref;
}

UserRef UserRef::invalid() { return UserRef{}; }

namespace {

bool all_digits(std::string_view v) {
    return !v.empty() && std::all_of(v.begin(), v.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

// "30s", "10m", "2h", "3d", "1w" — a token that *looks* like a duration
// (digits + a unit suffix letter) even when the value is out of range.
bool looks_like_duration(const std::string& token) {
    if (token.size() < 2) return false;
    const char suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(token.back())));
    if (suffix != 's' && suffix != 'm' && suffix != 'h' && suffix != 'd' && suffix != 'w') {
        return false;
    }
    return all_digits(std::string_view(token.data(), token.size() - 1));
}

}  // namespace

UserRef parse_user_ref(const std::string& arg) {
    std::string s = trim(arg);
    if (s.empty()) return UserRef::invalid();

    if (s.front() == '@') {
        if (s.size() == 1) return UserRef::invalid();
        return UserRef::from_username(s.substr(1));
    }

    if (all_digits(s)) {
        int64_t id = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), id);
        if (ec == std::errc{} && ptr == s.data() + s.size()) {
            return UserRef::from_id(id);
        }
    }

    return UserRef::invalid();
}

std::optional<int64_t> parse_duration_seconds(const std::string& token) {
    std::string v = trim(token);
    if (v.size() < 2) return std::nullopt;

    const char suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(v.back())));
    int64_t multiplier = 0;
    switch (suffix) {
        case 's': multiplier = 1; break;
        case 'm': multiplier = 60; break;
        case 'h': multiplier = 3600; break;
        case 'd': multiplier = 86400; break;
        case 'w': multiplier = 604800; break;
        default: return std::nullopt;
    }

    std::string_view num(v.data(), v.size() - 1);
    if (!all_digits(num)) return std::nullopt;

    int64_t n = 0;
    auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), n);
    if (ec != std::errc{} || ptr != num.data() + num.size()) return std::nullopt;

    if (n <= 0 || n > 10000) return std::nullopt;
    return n * multiplier;
}

MuteTokens split_mute_tokens(const std::string& args) {
    MuteTokens out;
    std::string s = trim(args);

    // First token; the rest becomes reason / duration+reason.
    std::string first = s;
    std::string rest;
    const std::size_t sp = s.find_first_of(" \t");
    if (sp != std::string::npos) {
        first = s.substr(0, sp);
        rest = trim(s.substr(sp + 1));
    }
    if (first.empty()) return out;  // pure reply usage

    if (parse_user_ref(first).valid()) {
        out.target = first;
    } else if (parse_duration_seconds(first)) {
        // Reply usage with a leading duration: "/mute 10m reason".
        out.duration = first;
    } else if (looks_like_duration(first)) {
        out.bad_duration = true;  // e.g. "0s", "10001w"
    } else {
        out.target = first;  // won't resolve -> "User not found"
    }

    // Optional duration as the second token.
    if (!rest.empty() && !out.duration) {
        const std::size_t sp2 = rest.find_first_of(" \t");
        const std::string tok2 =
            sp2 == std::string::npos ? rest : rest.substr(0, sp2);
        if (parse_duration_seconds(tok2)) {
            out.duration = tok2;
            rest = trim(sp2 == std::string::npos ? std::string{} : rest.substr(sp2 + 1));
        } else if (looks_like_duration(tok2)) {
            out.bad_duration = true;
        }
    }
    out.reason = trim(rest);
    return out;
}

std::optional<int> parse_warn_limit(const std::string& token) {
    std::string v = trim(token);
    if (!all_digits(v)) return std::nullopt;

    int n = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || ptr != v.data() + v.size()) return std::nullopt;
    if (n < 3 || n > 10) return std::nullopt;
    return n;
}

std::optional<std::string> parse_lang_token(const std::string& token) {
    std::string v = trim(token);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "en" || v == "ru") return v;
    return std::nullopt;
}

std::optional<bool> parse_yes_no(const std::string& token) {
    std::string v = trim(token);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "yes" || v == "y" || v == "on" || v == "true") return true;
    if (v == "no" || v == "n" || v == "off" || v == "false") return false;
    return std::nullopt;
}

std::string Action::name() const {
    switch (kind) {
        case Kind::Ban: return "ban";
        case Kind::Kick: return "kick";
        case Kind::Mute: return "mute";
        case Kind::TBan: return "tban";
        case Kind::TMute: return "tmute";
    }
    return "mute";
}

std::string Action::describe() const {
    if (timed()) return name() + " " + format_duration(duration_s);
    return name();
}

std::optional<Action> parse_action(const std::string& args) {
    std::string s = trim(args);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string head = s;
    std::string tail;
    const std::size_t sp = s.find_first_of(" \t");
    if (sp != std::string::npos) {
        head = s.substr(0, sp);
        tail = trim(s.substr(sp + 1));
    }

    Action a;
    if (head == "ban") a.kind = Action::Kind::Ban;
    else if (head == "kick") a.kind = Action::Kind::Kick;
    else if (head == "mute") a.kind = Action::Kind::Mute;
    else if (head == "tban") a.kind = Action::Kind::TBan;
    else if (head == "tmute") a.kind = Action::Kind::TMute;
    else return std::nullopt;

    if (!a.timed()) {
        if (!tail.empty()) return std::nullopt;
        return a;
    }
    const auto d = parse_duration_seconds(tail);
    if (!d) return std::nullopt;
    a.duration_s = *d;
    return a;
}

Action action_from_db(const std::string& name, int64_t duration_s) {
    Action a;
    if (name == "ban") a.kind = Action::Kind::Ban;
    else if (name == "kick") a.kind = Action::Kind::Kick;
    else if (name == "tban" && duration_s > 0) a.kind = Action::Kind::TBan;
    else if (name == "tmute" && duration_s > 0) a.kind = Action::Kind::TMute;
    else a.kind = Action::Kind::Mute;
    a.duration_s = a.timed() ? duration_s : 0;
    return a;
}

std::string format_duration(int64_t seconds) {
    if (seconds <= 0) return "0s";
    struct Unit {
        int64_t size;
        char suffix;
    };
    static constexpr Unit kUnits[] = {
        {604800, 'w'}, {86400, 'd'}, {3600, 'h'}, {60, 'm'}, {1, 's'},
    };
    for (const auto& u : kUnits) {
        if (seconds % u.size == 0) return std::to_string(seconds / u.size) + u.suffix;
    }
    return std::to_string(seconds) + "s";
}

std::optional<int> parse_int_in_range(const std::string& token, int min, int max) {
    const std::string v = trim(token);
    if (!all_digits(v)) return std::nullopt;
    int n = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec != std::errc{} || ptr != v.data() + v.size()) return std::nullopt;
    if (n < min || n > max) return std::nullopt;
    return n;
}

}  // namespace yacm::util