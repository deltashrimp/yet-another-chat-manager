#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace yacm::util {

std::string trim(std::string s);

// A user reference in a command argument: @username, numeric ID or reply.
struct UserRef {
    enum class Kind {
        Id,       // "123456789"
        Username, // "@duck"
        Reply,    // no argument, command is a reply
        Invalid,
    };

    Kind kind = Kind::Invalid;
    int64_t id = 0;
    std::string username;  // without leading '@'

    static UserRef from_id(int64_t id);
    static UserRef from_username(std::string username);
    static UserRef reply();
    static UserRef invalid();

    [[nodiscard]] bool valid() const { return kind != Kind::Invalid; }
};

UserRef parse_user_ref(const std::string& arg);

// Parses durations like 30s, 10m, 2h, 3d, 1w into seconds.
// Returns std::nullopt for invalid input.
std::optional<int64_t> parse_duration_seconds(const std::string& token);

// Tokens of a "/mute <target> [duration] [reason]" argument string.
struct MuteTokens {
    std::string target;                  // "@user" | "123" | "" (reply usage)
    std::optional<std::string> duration; // "10m" etc., when present
    std::string reason;
    bool bad_duration = false;           // a duration-like token failed to parse
};

// Splits a /mute argument string into target / optional duration / reason.
// The duration may be the second token ("/mute @user 10m spam") or, for a
// reply-based call, the first ("/mute 10m spam" with a reply target).
MuteTokens split_mute_tokens(const std::string& args);

// Parses a warn limit, valid range [3, 10].
std::optional<int> parse_warn_limit(const std::string& token);

// Normalizes a language token ("EN", "en", "ru") — lowercases and returns
// it if supported.
std::optional<std::string> parse_lang_token(const std::string& token);

// Parses a yes/no value ("yes/no y/n on/off true/false", any case).
// Returns true for an affirmative value, false for a negative one,
// std::nullopt for anything else.
std::optional<bool> parse_yes_no(const std::string& token);

// A punishment applied automatically (warn limit, antiflood): ban, kick,
// mute, or the timed tban / tmute with a duration.
struct Action {
    enum class Kind { Ban, Kick, Mute, TBan, TMute };

    Kind kind = Kind::Mute;
    int64_t duration_s = 0;  // > 0 only for TBan / TMute

    [[nodiscard]] bool timed() const { return kind == Kind::TBan || kind == Kind::TMute; }
    // "ban" | "kick" | "mute" | "tban" | "tmute" (the DB representation).
    [[nodiscard]] std::string name() const;
    // "tmute 10m", "ban" — human-readable, round-trips through parse_action.
    [[nodiscard]] std::string describe() const;

    bool operator==(const Action& o) const {
        return kind == o.kind && duration_s == o.duration_s;
    }
};

// Parses "ban", "kick", "mute", "tban 3d", "tmute 10m" (case-insensitive).
// Timed kinds require a valid duration; the others reject one.
std::optional<Action> parse_action(const std::string& args);

// Builds an Action from its stored form (name + duration); unknown names
// fall back to a permanent mute (the schema default).
Action action_from_db(const std::string& name, int64_t duration_s);

// Inverse of parse_duration_seconds: largest unit that divides evenly
// ("600" -> "10m", "90" -> "90s", "1209600" -> "2w").
std::string format_duration(int64_t seconds);

// Parses a positive integer in [min, max]; nullopt otherwise.
std::optional<int> parse_int_in_range(const std::string& token, int min, int max);

}  // namespace yacm::util