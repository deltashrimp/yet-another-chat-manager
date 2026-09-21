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

}  // namespace yacm::util