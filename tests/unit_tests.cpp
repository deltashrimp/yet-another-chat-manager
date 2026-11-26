#include "i18n/strings.hpp"
#include "router/router.hpp"
#include "services/ai.hpp"
#include "services/antiflood.hpp"
#include "services/history.hpp"
#include "services/throttle.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "services/warns.hpp"
#include "storage/db.hpp"
#include "storage/repos.hpp"
#include "telegram/client.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

template <typename T>
std::string show(const std::optional<T>& v) {
    if (!v) return "(nullopt)";
    return "(" + show(*v) + ")";
}

template <typename A, typename B>
void check_eq(const A& a, const B& b, const char* sa, const char* sb, const char* file,
              int line) {
    ++g_checks;
    if (!(a == b)) {
        std::printf("FAIL %s:%d: %s == %s (got %s vs %s)\n", file, line, sa, sb,
                    show(a).c_str(), show(b).c_str());
        ++g_failures;
    }
}
#define CHECK_EQ(a, b) check_eq((a), (b), #a, #b, __FILE__, __LINE__)

// --- util::parsers --------------------------------------------------------

void test_duration() {
    CHECK_EQ(yacm::util::parse_duration_seconds("30s"), 30);
    CHECK_EQ(yacm::util::parse_duration_seconds("10m"), 600);
    CHECK_EQ(yacm::util::parse_duration_seconds("2h"), 7200);
    CHECK_EQ(yacm::util::parse_duration_seconds("3d"), 259200);
    CHECK_EQ(yacm::util::parse_duration_seconds("1w"), 604800);
    CHECK(!yacm::util::parse_duration_seconds(""));
    CHECK(!yacm::util::parse_duration_seconds("5"));
    CHECK(!yacm::util::parse_duration_seconds("abc"));
    CHECK(!yacm::util::parse_duration_seconds("0s"));
    CHECK(!yacm::util::parse_duration_seconds("-30s"));
    CHECK(!yacm::util::parse_duration_seconds("3.5h"));
    CHECK(!yacm::util::parse_duration_seconds("10001w"));
}

void test_user_ref() {
    using yacm::util::UserRef;
    const auto username = yacm::util::parse_user_ref("@duck");
    CHECK(username.kind == UserRef::Kind::Username);
    CHECK_EQ(username.username, "duck");

    const auto by_id = yacm::util::parse_user_ref("123456789");
    CHECK(by_id.kind == UserRef::Kind::Id);
    CHECK_EQ(by_id.id, 123456789);

    CHECK(yacm::util::parse_user_ref("").kind == UserRef::Kind::Invalid);
    CHECK(yacm::util::parse_user_ref("12x").kind == UserRef::Kind::Invalid);
    CHECK(yacm::util::parse_user_ref("@").kind == UserRef::Kind::Invalid);
}

void test_warn_limit_lang() {
    CHECK_EQ(yacm::util::parse_warn_limit("3"), 3);
    CHECK_EQ(yacm::util::parse_warn_limit("10"), 10);
    CHECK(!yacm::util::parse_warn_limit("2"));
    CHECK(!yacm::util::parse_warn_limit("11"));
    CHECK(!yacm::util::parse_warn_limit("x"));
    CHECK_EQ(yacm::util::parse_lang_token("EN"), "en");
    CHECK_EQ(yacm::util::parse_lang_token("ru"), "ru");
    CHECK(!yacm::util::parse_lang_token("fr"));
}

void test_yes_no() {
    // affirmative forms
    CHECK_EQ(yacm::util::parse_yes_no("yes"), std::optional<bool>(true));
    CHECK_EQ(yacm::util::parse_yes_no("Y"), std::optional<bool>(true));
    CHECK_EQ(yacm::util::parse_yes_no("on"), std::optional<bool>(true));
    CHECK_EQ(yacm::util::parse_yes_no("TRUE"), std::optional<bool>(true));
    // negative forms
    CHECK_EQ(yacm::util::parse_yes_no("no"), std::optional<bool>(false));
    CHECK_EQ(yacm::util::parse_yes_no("n"), std::optional<bool>(false));
    CHECK_EQ(yacm::util::parse_yes_no("OFF"), std::optional<bool>(false));
    CHECK_EQ(yacm::util::parse_yes_no("false"), std::optional<bool>(false));
    // anything else is invalid
    CHECK(!yacm::util::parse_yes_no(""));
    CHECK(!yacm::util::parse_yes_no("maybe"));
    CHECK(!yacm::util::parse_yes_no("1"));
    CHECK(!yacm::util::parse_yes_no(" enable"));
}

void test_mute_tokens() {
    {
        const auto t = yacm::util::split_mute_tokens("@duck 10m spam reason");
        CHECK_EQ(t.target, "@duck");
        CHECK(t.duration.has_value());
        CHECK_EQ(*t.duration, "10m");
        CHECK_EQ(t.reason, "spam reason");
        CHECK(!t.bad_duration);
    }
    {
        const auto t = yacm::util::split_mute_tokens("@duck forever spam");
        CHECK_EQ(t.target, "@duck");
        CHECK(!t.duration);
        CHECK_EQ(t.reason, "forever spam");
    }
    {
        // reply usage: leading duration
        const auto t = yacm::util::split_mute_tokens("10m spam");
        CHECK_EQ(t.target, "");
        CHECK(t.duration.has_value());
        CHECK_EQ(*t.duration, "10m");
        CHECK_EQ(t.reason, "spam");
    }
    {
        const auto t = yacm::util::split_mute_tokens("123456789 reason");
        CHECK_EQ(t.target, "123456789");
        CHECK(!t.duration);
        CHECK_EQ(t.reason, "reason");
    }
    {
        const auto t = yacm::util::split_mute_tokens("@duck 0s bad");
        CHECK(t.bad_duration);
    }
    {
        const auto t = yacm::util::split_mute_tokens("");
        CHECK_EQ(t.target, "");
        CHECK(!t.duration);
        CHECK(!t.bad_duration);
    }
    {
        const auto t = yacm::util::split_mute_tokens("@duck 10001w out");
        CHECK(t.bad_duration);
    }
}

// --- util::format ---------------------------------------------------------

void test_format() {
    CHECK_EQ(yacm::util::html_escape("<b>&\"'"), "&lt;b&gt;&amp;&quot;&#39;");

    using yacm::tg::User;
    CHECK_EQ(yacm::util::display_name(User{1, false, "Ada", "Lovelace", "", ""}),
             "Ada Lovelace");
    CHECK_EQ(yacm::util::user_tag(User{1, false, "Ada", "", "ada", ""}), "@ada");
    CHECK_EQ(yacm::util::user_tag(User{1, false, "Ada", "", "", ""}),
             "<a href=\"tg://user?id=1\">Ada</a>");

    using yacm::tg::Chat;
    // public chat: the name becomes a t.me link
    CHECK_EQ(yacm::util::chat_display(Chat{-100, "supergroup", "My Group", "my_group"}, ""),
             "<a href=\"https://t.me/my_group\">My Group</a>");
    // no username: plain (escaped) title
    CHECK_EQ(yacm::util::chat_display(Chat{-100, "group", "My <Group>", ""}, ""),
             "My &lt;Group&gt;");
    // empty title: localized fallback
    CHECK_EQ(yacm::util::chat_display(Chat{-100, "supergroup", "", ""}, "Untitled"),
             "Untitled");

    const std::string d = yacm::util::format_date(1700000000);
    CHECK_EQ(d.size(), 16u);          // "YYYY-MM-DD HH:MM"
    CHECK_EQ(d.substr(4, 1), "-");
    CHECK_EQ(d.substr(7, 1), "-");
    CHECK_EQ(d.substr(13, 1), ":");

    CHECK_EQ(yacm::util::message_link(-1001234567890, 42), "https://t.me/c/1234567890/42");
    CHECK_EQ(yacm::util::message_link(-1001234567890, 0), "");   // no valid message id
    CHECK_EQ(yacm::util::message_link(-12345, 42), "");          // basic group: no public link
    CHECK_EQ(yacm::util::message_link(12345, 42), "");           // non-group chat
}

// --- i18n -----------------------------------------------------------------

void test_i18n() {
    yacm::i18n::Strings strings("en");
    CHECK_EQ(strings.t("en", "errors.only_private"),
             "This command is only available in private chats.");
    CHECK(strings.t("ru", "errors.only_private").find("Она") != std::string::npos ||
          strings.t("ru", "errors.only_private").find("личных") != std::string::npos);
    // unknown language falls back to English
    CHECK_EQ(strings.t("fr", "general.pong"), "Pong!");
    // missing key returns the key itself
    CHECK_EQ(strings.t("en", "no.such.key"), "no.such.key");
    CHECK(strings.is_supported("ru"));
    CHECK(!strings.is_supported("fr"));
    // localized language names (used by /lang replies)
    CHECK_EQ(strings.t("en", "lang.name.en"), "English");
    CHECK_EQ(strings.t("en", "lang.name.ru"), "Russian");
    CHECK_EQ(strings.t("ru", "lang.name.en"), "Английский");
    CHECK_EQ(strings.t("ru", "lang.name.ru"), "Русский");
    CHECK_EQ(strings.t("fr", "lang.name.en"), "English");  // fallback
    CHECK_EQ(strings.t("en", "settings.lang_changed"), "✓ Changed language to {0}");
    // step 6 report keys
    CHECK_EQ(strings.t("en", "reports.sent"), "✓ Sent report to admins");
    CHECK_EQ(strings.t("en", "reports.admin_notice"),
             "{0} reported {1} in {2}. Link: {3}");
    CHECK_EQ(strings.t("en", "settings.reports_set"), "✓ Reports are {0}");
    CHECK_EQ(strings.t("en", "errors.report_self"), "You cannot report your own message.");
    // report hardening: bot/owner/admin targets + already-reported acks
    CHECK_EQ(strings.t("en", "errors.cannot_report_bot"), "You cannot report the bot.");
    CHECK_EQ(strings.t("en", "errors.report_already"),
             "You have already reported this message.");
    CHECK_EQ(strings.t("en", "errors.report_already_target"),
             "You have already reported this user.");
    // step 8 keys
    CHECK_EQ(strings.t("en", "settings.title"), "Settings for {0}:");
    CHECK_EQ(strings.t("en", "settings.answer_set"), "✓ Answers are {0} for /{1}");
    CHECK_EQ(strings.t("en", "errors.answer_forbidden_command"),
             "Cannot toggle answers for /{0}");
    CHECK_EQ(strings.t("ru", "settings.language"), "Язык        > {0}");
    CHECK_EQ(strings.t("en", "answerlist.title"), "Answers for {0}:");
    CHECK_EQ(strings.t("ru", "answerlist.line"), "{0} > {1}");
    CHECK_EQ(strings.t("ru", "commands.answerlist"), "Ответы по командам (владелец)");
    CHECK_EQ(strings.t("en", "errors.invalid_yes_no"),
             "Invalid value. Use yes/no, y/n, on/off or true/false.");
    // supported_langs() lists every catalog
    const auto langs = strings.supported_langs();
    CHECK_EQ(langs.size(), 2u);
    bool has_en = false, has_ru = false;
    for (const auto& l : langs) {
        if (l == "en") has_en = true;
        if (l == "ru") has_ru = true;
    }
    CHECK(has_en);
    CHECK(has_ru);
}

// --- storage --------------------------------------------------------------

class TempDatabase {
public:
    TempDatabase() {
        path_ = std::filesystem::temp_directory_path() /
                ("yacm-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(::rand()) + ".db");
        db_.open();
        db_.migrate();
    }
    ~TempDatabase() {
        db_.close();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_.string() + "-wal", ec);
        std::filesystem::remove(path_.string() + "-shm", ec);
    }
    yacm::storage::Database& db() { return db_; }

private:
    std::filesystem::path path_;
    yacm::storage::Database db_;
};

void test_db_migrations() {
    TempDatabase tmp;
    CHECK_EQ(tmp.db().schema_version(), 6);
    for (const char* table : {"users", "chats", "warns", "mutes", "bans", "reports",
                              "audit", "members", "answers"}) {
        CHECK(tmp.db().table_exists(table));
    }
}

void test_db_creates_parent_dir() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("yacm-dir-test-" + std::to_string(::getpid()) + "-" +
                       std::to_string(::rand()));
    const auto nested = base / "sub/dir/bot.db";
    {
        yacm::storage::Database db(nested.string());
        db.open();  // must create base/sub/dir on its own
        db.migrate();
        CHECK(std::filesystem::exists(nested));
        CHECK_EQ(db.schema_version(), 6);
    }
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

void test_user_repo() {
    TempDatabase tmp;
    yacm::storage::UserRepo repo(tmp.db());

    yacm::tg::User user{42, false, "Ada", "Lovelace", "ada", ""};
    repo.upsert(user, 111);

    // @-resolution works only for users the bot has seen.
    const auto found = repo.find_by_username("ADA");
    CHECK(found.has_value());
    CHECK_EQ(found->id, 42);
    CHECK_EQ(repo.find_by_id(42)->first_name, "Ada");
    CHECK(!repo.find_by_username("nobody"));

    // Upsert refreshes metadata.
    yacm::tg::User renamed{42, false, "Ada", "Byron", "ada", ""};
    repo.upsert(renamed, 222);
    CHECK_EQ(repo.find_by_id(42)->last_name, "Byron");

    // Several users without a username must coexist (stored as NULL, not
    // '', which used to collide on the unique username index).
    repo.upsert(yacm::tg::User{50, false, "NoName1", "", "", ""}, 300);
    repo.upsert(yacm::tg::User{51, false, "NoName2", "", "", ""}, 300);
    CHECK(repo.find_by_id(50).has_value());
    CHECK(repo.find_by_id(51).has_value());
    CHECK(!repo.find_by_username(""));

    // A username that moved to another account is released by the stale row.
    repo.upsert(yacm::tg::User{60, false, "Grace", "", "ADA", ""}, 400);
    CHECK_EQ(repo.find_by_username("ada")->id, 60);
    CHECK(repo.find_by_id(42)->username.empty());
}

void test_chat_repo() {
    TempDatabase tmp;
    yacm::storage::ChatRepo repo(tmp.db());

    yacm::tg::Chat chat{1001, "supergroup", "Test Group", ""};
    repo.ensure(chat, 1, "en");
    CHECK_EQ(repo.language(1001), "en");
    repo.set_language(1001, "ru");
    CHECK_EQ(repo.language(1001), "ru");

    CHECK_EQ(repo.warn_limit(1001), 3);
    repo.set_warn_limit(1001, 5);
    CHECK_EQ(repo.warn_limit(1001), 5);
    CHECK_EQ(repo.warn_action(1001), "mute");
    repo.set_warn_action(1001, "ban");
    CHECK_EQ(repo.warn_action(1001), "ban");

    CHECK(!repo.has_rules(1001));
    repo.set_rules(1001, "<b>rules</b>");
    CHECK(repo.has_rules(1001));
    CHECK_EQ(repo.rules_text(1001), "<b>rules</b>");
    repo.clear_rules(1001);
    CHECK(!repo.has_rules(1001));
    CHECK_EQ(repo.rules_text(1001), "");

    CHECK(repo.reports_enabled(1001));
    repo.set_reports_enabled(1001, false);
    CHECK(!repo.reports_enabled(1001));

    // settings() snapshots everything, with schema defaults.
    {
        yacm::storage::ChatSettings s = repo.settings(9999);  // unknown chat
        CHECK_EQ(s.language, "en");
        CHECK_EQ(s.warn_limit, 3);
        CHECK_EQ(s.warn_action, "mute");
        CHECK(s.reports_enabled);
        CHECK_EQ(s.rules_html, "");
    }
    repo.set_language(1001, "ru");
    repo.set_warn_limit(1001, 5);
    repo.set_warn_action(1001, "ban");
    repo.set_rules(1001, "be nice");
    {
        yacm::storage::ChatSettings s = repo.settings(1001);
        CHECK_EQ(s.language, "ru");
        CHECK_EQ(s.warn_limit, 5);
        CHECK_EQ(s.warn_action, "ban");
        CHECK(!s.reports_enabled);
        CHECK_EQ(s.rules_html, "be nice");
    }
}

void test_answer_repo() {
    TempDatabase tmp;
    yacm::storage::AnswerRepo repo(tmp.db());

    // default: enabled
    CHECK(repo.enabled(-100, "warn"));
    CHECK(repo.enabled(-100, "ban"));

    repo.set(-100, "warn", false);
    CHECK(!repo.enabled(-100, "warn"));
    CHECK(repo.enabled(-100, "ban"));   // other command unaffected
    CHECK(repo.enabled(-200, "warn"));  // other chat unaffected

    repo.set(-100, "warn", true);
    CHECK(repo.enabled(-100, "warn"));
}

void test_member_cache() {
    TempDatabase tmp;
    yacm::storage::UserRepo repo(tmp.db());

    yacm::tg::User owner{1, false, "Owner", "", "owner", ""};
    yacm::tg::User admin{2, false, "Admin", "", "admin", ""};
    yacm::tg::User member{3, false, "Member", "", "", ""};
    yacm::tg::User other{4, false, "Other", "", "", ""};

    repo.upsert(owner, 10);
    repo.upsert(admin, 10);
    repo.upsert(member, 10);
    repo.upsert_member(-100, 1, "creator", 10, 10);
    repo.upsert_member(-100, 2, "administrator", 10, 10);
    repo.upsert_member(-100, 3, "member", 10, 10);
    repo.upsert_member(-100, 4, "left", 10, std::nullopt);

    CHECK_EQ(repo.member_status(-100, 1), std::optional<std::string>("creator"));
    CHECK_EQ(repo.member_status(-100, 2), std::optional<std::string>("administrator"));
    CHECK_EQ(repo.member_status(-100, 3), std::optional<std::string>("member"));
    CHECK_EQ(repo.member_status(-100, 4), std::optional<std::string>("left"));
    CHECK(!repo.member_status(-100, 999));
    CHECK(!repo.member_status(-200, 1));  // other chat

    // admins() returns creator first, then admins, never plain members.
    const auto admins = repo.admins(-100);
    CHECK_EQ(admins.size(), 2u);
    CHECK_EQ(admins[0].id, 1);
    CHECK_EQ(admins[0].username, "owner");
    CHECK_EQ(admins[1].id, 2);
    CHECK_EQ(repo.admins(-200).size(), 0u);

    // promotion write-through: member becomes admin
    repo.upsert_member(-100, 3, "administrator", 20, std::nullopt);
    CHECK_EQ(repo.member_status(-100, 3), std::optional<std::string>("administrator"));

    // Posting a message never downgrades a cached admin/creator, only
    // revives users cached as gone, and never invents a row.
    repo.mark_member_active(-100, 1, 30);
    repo.mark_member_active(-100, 3, 30);
    repo.mark_member_active(-100, 4, 30);
    repo.mark_member_active(-100, 999, 30);
    CHECK_EQ(repo.member_status(-100, 1), std::optional<std::string>("creator"));
    CHECK_EQ(repo.member_status(-100, 3), std::optional<std::string>("administrator"));
    CHECK_EQ(repo.member_status(-100, 4), std::optional<std::string>("member"));
    CHECK(!repo.member_status(-100, 999));
    CHECK_EQ(repo.member_entry(-100, 4)->updated_at, 30);
    CHECK_EQ(repo.admins(-100).size(), 3u);
}

void test_warn_repo() {
    TempDatabase tmp;
    yacm::storage::WarnRepo repo(tmp.db());
    CHECK_EQ(repo.count(-100, 7), 0);
    repo.add(-100, 7, "first", 1, 1000);
    repo.add(-100, 7, "second", 1, 2000);
    repo.add(-100, 8, "other", 1, 3000);
    CHECK_EQ(repo.count(-100, 7), 2);
    CHECK_EQ(repo.count(-100, 8), 1);
    CHECK_EQ(repo.count(-100, 9), 0);

    const auto rows = repo.list(-100, 7);
    CHECK_EQ(rows.size(), 2u);
    CHECK_EQ(rows[0].reason, "second");  // newest first
    CHECK_EQ(rows[0].created_at, 2000);
    CHECK_EQ(rows[1].reason, "first");

    CHECK(repo.remove_last(-100, 7));
    CHECK_EQ(repo.count(-100, 7), 1);
    CHECK_EQ(repo.list(-100, 7)[0].reason, "first");
    CHECK(repo.remove_last(-100, 7));
    CHECK(!repo.remove_last(-100, 7));  // nothing left
    CHECK_EQ(repo.count(-100, 7), 0);

    repo.clear(-100, 8);
    CHECK_EQ(repo.count(-100, 8), 0);
}

void test_mute_ban_repo() {
    TempDatabase tmp;
    yacm::storage::MuteRepo mutes(tmp.db());
    yacm::storage::BanRepo bans(tmp.db());

    CHECK(!mutes.until(-100, 7));
    mutes.set(-100, 7, 3600, "spam", 1, 1000);
    CHECK_EQ(*mutes.until(-100, 7), 3600);
    mutes.set(-100, 7, 7200, "again", 1, 2000);  // replaces previous mute
    CHECK_EQ(*mutes.until(-100, 7), 7200);
    mutes.clear(-100, 7);
    CHECK(!mutes.until(-100, 7));

    CHECK(!bans.reason(-100, 7));
    bans.set(-100, 7, "spam", 1, 1000);
    CHECK_EQ(*bans.reason(-100, 7), "spam");
    bans.set(-100, 7, "worse", 1, 2000);
    CHECK_EQ(*bans.reason(-100, 7), "worse");
    bans.clear(-100, 7);
    CHECK(!bans.reason(-100, 7));
}

void test_audit_repo() {
    TempDatabase tmp;
    yacm::storage::AuditRepo repo(tmp.db());
    repo.log(-100, 1, "ban", 7, "spam", 1000);
    repo.log(-100, 2, "warn", 8, "", 2000);
    repo.log(-100, 3, "kick", std::nullopt, "", 3000);

    yacm::storage::Statement st(tmp.db().raw(),
                                "SELECT COUNT(*), SUM(action='ban'), SUM(target_id IS NULL) "
                                "FROM audit");
    CHECK(st.step());
    CHECK_EQ(st.col_int(0), 3);
    CHECK_EQ(st.col_int(1), 1);
    CHECK_EQ(st.col_int(2), 1);
}

void test_report_repo() {
    TempDatabase tmp;
    yacm::storage::ReportRepo repo(tmp.db());

    CHECK(!repo.already_reported(-100, 7, 42));
    repo.add(-100, 42, 7, 8, 1000);
    CHECK(repo.already_reported(-100, 7, 42));

    // another user may report the same message
    repo.add(-100, 42, 9, 8, 2000);
    CHECK(repo.already_reported(-100, 9, 42));

    // same user, a different message — a new report
    repo.add(-100, 43, 7, 8, 3000);
    CHECK(repo.already_reported(-100, 7, 43));

    // a repeated report is a no-op, not an error (INSERT OR IGNORE)
    repo.add(-100, 42, 7, 8, 4000);

    yacm::storage::Statement st(
        tmp.db().raw(),
        "SELECT COUNT(*) FROM reports WHERE message_id = 42 AND reporter_id = 7");
    CHECK(st.step());
    CHECK_EQ(st.col_int(0), 1);

    // per-chat target dedup: the same reporter can't spam the same user
    CHECK(repo.already_reported_target(-100, 7, 8));   // reported already
    CHECK(!repo.already_reported_target(-100, 7, 9));  // different target
    CHECK(!repo.already_reported_target(-200, 7, 8));  // different chat
    repo.add(-200, 55, 7, 8, 5000);
    CHECK(repo.already_reported_target(-200, 7, 8));
    CHECK(repo.already_reported(-200, 7, 55));

    // message ids are per chat: message 42 of another chat is a different
    // message and its report must be stored, not swallowed by the dedup
    CHECK(!repo.already_reported(-200, 7, 42));
    repo.add(-200, 42, 7, 10, 6000);
    CHECK(repo.already_reported(-200, 7, 42));
    yacm::storage::Statement st2(tmp.db().raw(),
                                 "SELECT COUNT(*) FROM reports WHERE message_id = 42 "
                                 "AND reporter_id = 7");
    CHECK(st2.step());
    CHECK_EQ(st2.col_int(0), 2);
}

void test_transaction() {
    TempDatabase tmp;
    // rollback: nothing persisted
    {
        yacm::storage::Transaction tx(tmp.db());
        tmp.db().exec("INSERT INTO users (id, first_name, first_seen_at, updated_at) "
                      "VALUES (1, 'A', 1, 1)");
        tx.rollback();
    }
    {
        yacm::storage::Statement st(tmp.db().raw(), "SELECT COUNT(*) FROM users");
        CHECK(st.step());
        CHECK_EQ(st.col_int(0), 0);
    }

    // commit persists
    {
        yacm::storage::Transaction tx(tmp.db());
        tmp.db().exec("INSERT INTO users (id, first_name, first_seen_at, updated_at) "
                      "VALUES (2, 'B', 1, 1)");
        tx.commit();
    }
    {
        yacm::storage::Statement st(tmp.db().raw(), "SELECT COUNT(*) FROM users");
        CHECK(st.step());
        CHECK_EQ(st.col_int(0), 1);
    }

    // destructor rolls back uncommitted work
    {
        yacm::storage::Transaction tx(tmp.db());
        tmp.db().exec("INSERT INTO users (id, first_name, first_seen_at, updated_at) "
                      "VALUES (3, 'C', 1, 1)");
    }
    {
        yacm::storage::Statement st(tmp.db().raw(), "SELECT COUNT(*) FROM users");
        CHECK(st.step());
        CHECK_EQ(st.col_int(0), 1);
    }
}

// --- services -------------------------------------------------------------

void test_roles() {
    using yacm::services::Role;
    CHECK(yacm::services::role_rank(Role::BotOwner) > yacm::services::role_rank(Role::Owner));
    CHECK(yacm::services::meets(Role::Admin, Role::BotOwner));
    CHECK(yacm::services::meets(Role::Owner, Role::Owner));
    CHECK(!yacm::services::meets(Role::Owner, Role::Admin));
    CHECK(yacm::services::meets(Role::Everyone, Role::Everyone));
    CHECK(yacm::services::meets(Role::Owner, Role::BotOwner));

    // spec §2 hierarchy restrictions
    CHECK_EQ(yacm::services::moderation_block(7, 9, Role::Everyone), "");
    CHECK_EQ(yacm::services::moderation_block(7, 9, Role::Owner),
             "errors.cannot_restrict_owner");
    CHECK_EQ(yacm::services::moderation_block(7, 9, Role::Admin),
             "errors.cannot_restrict_admin");
    CHECK_EQ(yacm::services::moderation_block(9, 9, Role::Everyone),
             "errors.cannot_restrict_bot");
}

// --- router ---------------------------------------------------------------

void test_router() {
    using yacm::router::HandlerRegistry;

    HandlerRegistry reg;
    int start_calls = 0;
    reg.add("start",
            [&](const yacm::tg::Message&, const std::string&) { ++start_calls; });

    yacm::tg::Message m;
    m.text = "/start hello world";
    CHECK(reg.dispatch(m));
    CHECK_EQ(start_calls, 1);

    m.text = "/start@SomeBot args";
    CHECK(reg.dispatch(m));
    CHECK_EQ(start_calls, 2);

    // Once the bot knows its own username, commands addressed to another
    // bot are ignored; its own @suffix (any case) still works.
    reg.set_bot_username("Test_Bot");
    m.text = "/start@SomeBot args";
    CHECK(!reg.dispatch(m));
    CHECK_EQ(start_calls, 2);
    m.text = "/start@test_bot args";
    CHECK(reg.dispatch(m));
    m.text = "/start args";
    CHECK(reg.dispatch(m));
    CHECK_EQ(start_calls, 4);

    m.text = "not a command";
    CHECK(!reg.dispatch(m));

    m.text = "/unknown";
    CHECK(!reg.dispatch(m));

    bool args_ok = false;
    HandlerRegistry warn_reg;
    warn_reg.add("warn",
                 [&](const yacm::tg::Message&, const std::string& args) {
                     args_ok = (args == "@user reason");
                 });
    yacm::tg::Message m2;
    m2.text = "/warn @user reason";
    warn_reg.dispatch(m2);
    CHECK(args_ok);

    // command visibility by role
    HandlerRegistry roles_reg;
    roles_reg.add("a", [](const yacm::tg::Message&, const std::string&) {},
                  yacm::services::Role::Everyone);
    roles_reg.add("b", [](const yacm::tg::Message&, const std::string&) {},
                  yacm::services::Role::Admin);
    roles_reg.add("c", [](const yacm::tg::Message&, const std::string&) {},
                  yacm::services::Role::Owner);
    roles_reg.add("s", [](const yacm::tg::Message&, const std::string&) {},
                  yacm::services::Role::Everyone, /*dm_only=*/true);

    CHECK_EQ(roles_reg.commands_for(yacm::services::Role::Everyone, false).size(), 1u);
    CHECK_EQ(roles_reg.commands_for(yacm::services::Role::Admin, false).size(), 2u);
    CHECK_EQ(roles_reg.commands_for(yacm::services::Role::BotOwner, false).size(), 3u);
    // in a private chat the dm-only command is visible too
    CHECK_EQ(roles_reg.commands_for(yacm::services::Role::Everyone, true).size(), 2u);
}

// --- fake Telegram API client --------------------------------------------

// Scripted TelegramClient: records every outbound call and answers from
// scripted state. Unscripted getChatMember responses throw, so tests only
// observe what they explicitly set up. This is the spec §10 mock seam.
class FakeClient : public yacm::tg::TelegramClient {
public:
    yacm::tg::User me{777, true, "TestBot", "", "test_bot", ""};

    // Scripted getChatMember responses per (chat_id, user_id).
    std::map<std::pair<int64_t, int64_t>, yacm::tg::ChatMember> members;
    std::vector<yacm::tg::ChatMember> admins;

    struct BanCall {
        int64_t chat_id;
        int64_t user_id;
        std::string reason;
        long until_date = 0;
    };
    struct RestrictCall {
        int64_t chat_id;
        int64_t user_id;
        long until_date;
        nlohmann::json permissions;
    };

    // Recorded outbound calls.
    std::vector<BanCall> bans;
    std::vector<std::pair<int64_t, int64_t>> unbans;
    std::vector<RestrictCall> restricts;
    std::vector<std::pair<int64_t, int64_t>> chat_member_queries;
    std::vector<int64_t> admin_queries;
    std::vector<std::pair<int64_t, std::string>> messages;  // (chat_id, text)

    // When set, ban/unban/restrict throw (simulates a failed API call).
    bool fail_actions = false;

    yacm::tg::User get_me() override { return me; }
    std::vector<yacm::tg::Update> get_updates(int64_t, int) override { return {}; }

    yacm::tg::Message send_message(int64_t chat_id, const std::string& text,
                                   const std::string& parse_mode,
                                   std::optional<int64_t> reply_to_message_id,
                                   long timeout_s) override {
        (void)parse_mode;
        (void)reply_to_message_id;
        (void)timeout_s;
        messages.emplace_back(chat_id, text);
        yacm::tg::Message m;
        m.message_id = static_cast<int64_t>(messages.size());
        m.chat = yacm::tg::Chat{chat_id, "supergroup", "", ""};
        m.text = text;
        return m;
    }
    void delete_message(int64_t, int64_t) override {}
    yacm::tg::Chat get_chat(int64_t chat_id) override {
        return yacm::tg::Chat{chat_id, "supergroup", "Test Group", ""};
    }
    yacm::tg::ChatMember get_chat_member(int64_t chat_id, int64_t user_id) override {
        chat_member_queries.emplace_back(chat_id, user_id);
        const auto it = members.find({chat_id, user_id});
        if (it == members.end()) throw yacm::tg::TelegramError("no scripted member");
        return it->second;
    }
    std::vector<yacm::tg::ChatMember> get_chat_administrators(int64_t chat_id) override {
        admin_queries.push_back(chat_id);
        return admins;
    }
    void ban_chat_member(int64_t chat_id, int64_t user_id, const std::string& reason,
                         long until_date) override {
        if (fail_actions) throw yacm::tg::TelegramError(403, "forbidden");
        bans.push_back({chat_id, user_id, reason, until_date});
    }
    void unban_chat_member(int64_t chat_id, int64_t user_id) override {
        if (fail_actions) throw yacm::tg::TelegramError(403, "forbidden");
        unbans.emplace_back(chat_id, user_id);
    }
    void restrict_chat_member(int64_t chat_id, int64_t user_id, long until_date,
                              const nlohmann::json& permissions) override {
        if (fail_actions) throw yacm::tg::TelegramError(403, "forbidden");
        restricts.push_back({chat_id, user_id, until_date, permissions});
    }
    void unpin_all_messages(int64_t) override {}
};

// --- services against the fake Telegram API -------------------------------

void test_moderation_service() {
    using yacm::services::Role;
    using yacm::services::ModerationService;

    constexpr int64_t kBot = 777;
    constexpr int64_t kChat = -100;

    FakeClient fake;
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""},
                                   "administrator"};

    TempDatabase tmp;
    yacm::storage::MuteRepo mutes(tmp.db());
    yacm::storage::BanRepo bans(tmp.db());
    yacm::storage::AuditRepo audit(tmp.db());
    ModerationService svc(fake, kBot, tmp.db(), mutes, bans, audit);

    // ban: live call recorded, DB row + audit written
    const auto ban_r = svc.ban(kChat, 1, 2, Role::Everyone, "spam");
    CHECK(ban_r.ok);
    CHECK_EQ(fake.bans.size(), 1u);
    CHECK_EQ(fake.bans[0].chat_id, kChat);
    CHECK_EQ(fake.bans[0].user_id, 2);
    CHECK_EQ(fake.bans[0].reason, "spam");
    CHECK_EQ(*bans.reason(kChat, 2), "spam");

    // unban: clears the ban row
    const auto unban_r = svc.unban(kChat, 1, 2, Role::Everyone);
    CHECK(unban_r.ok);
    CHECK_EQ(fake.unbans.size(), 1u);
    CHECK_EQ(fake.unbans[0].second, 2);
    CHECK(!bans.reason(kChat, 2));

    // kick: ban + unban (a kick is a ban immediately lifted)
    const auto kick_r = svc.kick(kChat, 1, 3, Role::Everyone, "bye");
    CHECK(kick_r.ok);
    CHECK_EQ(fake.bans.size(), 2u);
    CHECK_EQ(fake.bans.back().user_id, 3);
    CHECK_EQ(fake.unbans.size(), 2u);

    // mute forever (until_s = 0) and with a duration
    const auto mute_r = svc.mute(kChat, 1, 4, Role::Everyone, 0, "noise");
    CHECK(mute_r.ok);
    CHECK_EQ(fake.restricts.size(), 1u);
    CHECK_EQ(fake.restricts[0].user_id, 4);
    CHECK_EQ(fake.restricts[0].until_date, 0L);
    CHECK_EQ(fake.restricts[0].permissions["can_send_messages"], false);
    CHECK(mutes.until(kChat, 4).has_value());
    CHECK_EQ(*mutes.until(kChat, 4), 0);

    const auto mute_r2 = svc.mute(kChat, 1, 5, Role::Everyone, 3600, "flood");
    CHECK(mute_r2.ok);
    CHECK_EQ(fake.restricts.size(), 2u);
    CHECK(fake.restricts[1].until_date > 0);  // now + duration
    // the DB stores the absolute expiry (same value sent to Telegram), not
    // the duration
    CHECK_EQ(*mutes.until(kChat, 5), static_cast<int64_t>(fake.restricts[1].until_date));
    CHECK(*mutes.until(kChat, 5) > 3600);

    // > 366 days would be permanent on Telegram's side: rejected, no API call
    const auto mute_long = svc.mute(kChat, 1, 5, Role::Everyone, 400LL * 86400, "x");
    CHECK(!mute_long.ok);
    CHECK_EQ(mute_long.error_key, "errors.invalid_duration");
    CHECK_EQ(fake.restricts.size(), 2u);

    // unmute: fresh permissions that restore sending + mutes row dropped
    const auto unmute_r = svc.unmute(kChat, 1, 4, Role::Everyone);
    CHECK(unmute_r.ok);
    CHECK_EQ(fake.restricts.size(), 3u);
    CHECK_EQ(fake.restricts[2].until_date, 0L);
    CHECK_EQ(fake.restricts[2].permissions["can_send_messages"], true);
    CHECK(!mutes.until(kChat, 4));

    // spec §2 hierarchy: cannot moderate the bot, the owner or another admin
    const auto bot_r = svc.ban(kChat, 1, kBot, Role::Everyone, "x");
    CHECK(!bot_r.ok);
    CHECK_EQ(bot_r.error_key, "errors.cannot_restrict_bot");
    const auto owner_r = svc.ban(kChat, 1, 9, Role::Owner, "x");
    CHECK(!owner_r.ok);
    CHECK_EQ(owner_r.error_key, "errors.cannot_restrict_owner");
    const auto admin_r = svc.ban(kChat, 1, 9, Role::Admin, "x");
    CHECK(!admin_r.ok);
    CHECK_EQ(admin_r.error_key, "errors.cannot_restrict_admin");
    CHECK_EQ(fake.bans.size(), 2u);  // no API calls for blocked targets

    // bot without rights: refused before any API call
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""}, "member"};
    const auto rights_r = svc.mute(kChat, 1, 4, Role::Everyone, 0, "x");
    CHECK(!rights_r.ok);
    CHECK_EQ(rights_r.error_key, "errors.not_enough_rights");
    CHECK_EQ(fake.restricts.size(), 3u);  // unchanged
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""},
                                   "administrator"};

    // failed API call -> moderation_failed, no DB row, no API record
    fake.fail_actions = true;
    const auto fail_r = svc.ban(kChat, 1, 6, Role::Everyone, "x");
    CHECK(!fail_r.ok);
    CHECK_EQ(fail_r.error_key, "errors.moderation_failed");
    CHECK(!bans.reason(kChat, 6));
    CHECK_EQ(fake.bans.size(), 2u);  // the recorded ban never happened
    fake.fail_actions = false;

    // audit log: ban, unban, kick, mute, mute, unmute
    yacm::storage::Statement st(tmp.db().raw(),
                                "SELECT COUNT(*), SUM(action='kick'), SUM(action='unmute') "
                                "FROM audit");
    CHECK(st.step());
    CHECK_EQ(st.col_int(0), 6);
    CHECK_EQ(st.col_int(1), 1);
    CHECK_EQ(st.col_int(2), 1);
}

void test_roles_live() {
    using yacm::services::Role;
    using yacm::services::Roles;

    constexpr int64_t kChat = -100;
    constexpr int64_t kUserId = 42;

    FakeClient fake;
    TempDatabase tmp;
    yacm::storage::UserRepo users(tmp.db());
    yacm::config::Config cfg;  // default: bot_owner_id = 0
    Roles roles(cfg, fake, users);

    const yacm::tg::User user{kUserId, false, "Someone", "", "someone", ""};

    // No cached member -> live getChatMember (scripted "creator"), the
    // result is written through to the members cache.
    fake.members[{kChat, kUserId}] = {yacm::tg::User{kUserId, false, "Someone", "", "", ""},
                                      "creator"};
    CHECK(roles.resolve(kChat, user) == Role::Owner);
    CHECK_EQ(fake.chat_member_queries.size(), 1u);
    CHECK_EQ(users.member_status(kChat, kUserId),
             std::optional<std::string>("creator"));

    // Cache hit now: no second live call.
    CHECK(roles.resolve(kChat, user) == Role::Owner);
    CHECK_EQ(fake.chat_member_queries.size(), 1u);

    // bot_owner is global (before any cache).
    yacm::config::Config cfg_owner;
    cfg_owner.bot_owner_id = kUserId;
    Roles roles_owner(cfg_owner, fake, users);
    CHECK(roles_owner.resolve(kChat, user) == Role::BotOwner);

    // Live call failure falls back to Everyone (no crash, no cache write).
    const yacm::tg::User stranger{99, false, "Stranger", "", "", ""};
    CHECK(roles.resolve(kChat, stranger) == Role::Everyone);
    CHECK_EQ(fake.chat_member_queries.size(), 2u);
    CHECK(!users.member_status(kChat, 99));

    // A stale cached "administrator" (demoted while the bot was not
    // watching) is re-validated live instead of being trusted forever.
    const yacm::tg::User demoted{55, false, "Demoted", "", "", ""};
    users.upsert_member(kChat, 55, "administrator", /*now=*/1, std::nullopt);
    fake.members[{kChat, 55}] = {demoted, "member"};
    CHECK(roles.resolve(kChat, demoted) == Role::Everyone);
    CHECK_EQ(fake.chat_member_queries.size(), 3u);
    CHECK_EQ(users.member_status(kChat, 55), std::optional<std::string>("member"));

    // ...and when it cannot be re-validated it fails closed.
    users.upsert_member(kChat, 56, "administrator", /*now=*/1, std::nullopt);
    CHECK(roles.resolve(kChat, yacm::tg::User{56, false, "X", "", "", ""}) ==
          Role::Everyone);
}

void test_warn_limit_action() {
    using yacm::services::Role;
    using yacm::services::ModerationService;
    using yacm::services::WarnService;

    constexpr int64_t kBot = 777;
    constexpr int64_t kChat = -100;

    FakeClient fake;
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""},
                                   "administrator"};

    TempDatabase tmp;
    yacm::storage::ChatRepo chats(tmp.db());
    yacm::storage::WarnRepo warns(tmp.db());
    yacm::storage::MuteRepo mutes(tmp.db());
    yacm::storage::BanRepo bans(tmp.db());
    yacm::storage::AuditRepo audit(tmp.db());
    ModerationService moderation(fake, kBot, tmp.db(), mutes, bans, audit);
    WarnService svc(tmp.db(), chats, warns, audit, kBot, moderation);

    // Defaults: limit 3, action "mute" (spec §5.6).
    CHECK_EQ(svc.warn_limit(kChat), 3);

    // Warns 1 and 2: recorded, no Telegram call yet.
    auto w1 = svc.warn(kChat, 1, 2, Role::Everyone, "spam");
    CHECK(w1.ok);
    CHECK_EQ(w1.count, 1);
    CHECK(w1.fired_action.empty());
    auto w2 = svc.warn(kChat, 1, 2, Role::Everyone, "again");
    CHECK(w2.ok);
    CHECK_EQ(w2.count, 2);
    CHECK_EQ(w2.limit, 3);
    CHECK_EQ(fake.restricts.size(), 0u);

    // Warn 3 reaches the limit: the configured action ("mute") fires via
    // the real ModerationService (recorded on the fake), warns reset to 0.
    auto w3 = svc.warn(kChat, 1, 2, Role::Everyone, "third");
    CHECK(w3.ok);
    CHECK_EQ(w3.fired_action, "mute");
    CHECK_EQ(w3.count, 0);
    CHECK_EQ(fake.restricts.size(), 1u);
    CHECK_EQ(fake.restricts[0].user_id, 2);
    CHECK_EQ(fake.restricts[0].permissions["can_send_messages"], false);
    CHECK_EQ(svc.count(kChat, 2), 0);  // warns cleared

    // audit: 3 warns + the fired "mute" action + the reset.
    yacm::storage::Statement st(tmp.db().raw(),
                                "SELECT COUNT(*), SUM(action='mute'), SUM(action='warn_reset') "
                                "FROM audit");
    CHECK(st.step());
    CHECK_EQ(st.col_int(0), 5);
    CHECK_EQ(st.col_int(1), 1);
    CHECK_EQ(st.col_int(2), 1);

    // Bot without rights: the warn-limit action cannot fire. The warn stays
    // on record (out.ok=false with a clear error), the next /warn retries.
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""}, "member"};
    const auto f1 = svc.warn(kChat, 1, 3, Role::Everyone, "one");
    CHECK(f1.ok);
    const auto f2 = svc.warn(kChat, 1, 3, Role::Everyone, "two");
    CHECK(f2.ok);
    const auto f3 = svc.warn(kChat, 1, 3, Role::Everyone, "three");
    CHECK(!f3.ok);
    CHECK_EQ(f3.error_key, "errors.not_enough_rights");
    CHECK_EQ(svc.count(kChat, 3), 3);        // warn kept, not reset
    CHECK_EQ(fake.restricts.size(), 1u);     // no new API call
}


// --- Rose-compatible additions: actions, timed bans, antiflood --------------

void test_parse_action() {
    using yacm::util::Action;
    using yacm::util::parse_action;

    CHECK(parse_action("ban") == (Action{Action::Kind::Ban, 0}));
    CHECK(parse_action("KICK") == (Action{Action::Kind::Kick, 0}));
    CHECK(parse_action(" mute ") == (Action{Action::Kind::Mute, 0}));
    CHECK(parse_action("tban 3d") == (Action{Action::Kind::TBan, 3 * 86400}));
    CHECK(parse_action("tmute 10m") == (Action{Action::Kind::TMute, 600}));
    CHECK(!parse_action(""));
    CHECK(!parse_action("tban"));         // timed kinds need a duration
    CHECK(!parse_action("tmute forever"));
    CHECK(!parse_action("ban 3d"));       // plain kinds reject one
    CHECK(!parse_action("nuke"));

    CHECK_EQ(parse_action("tmute 90m")->describe(), "tmute 90m");
    CHECK_EQ(parse_action("tban 2w")->describe(), "tban 2w");
    CHECK_EQ(parse_action("kick")->describe(), "kick");

    // DB round trip; unknown or duration-less timed names fall back to mute
    CHECK(yacm::util::action_from_db("tban", 60) == (Action{Action::Kind::TBan, 60}));
    CHECK(yacm::util::action_from_db("ban", 60) == (Action{Action::Kind::Ban, 0}));
    CHECK(yacm::util::action_from_db("tmute", 0) == (Action{Action::Kind::Mute, 0}));
    CHECK(yacm::util::action_from_db("???", 0) == (Action{Action::Kind::Mute, 0}));

    CHECK_EQ(yacm::util::format_duration(600), "10m");
    CHECK_EQ(yacm::util::format_duration(90), "90s");
    CHECK_EQ(yacm::util::format_duration(5400), "90m");
    CHECK_EQ(yacm::util::format_duration(86400), "1d");
    CHECK_EQ(yacm::util::format_duration(1209600), "2w");

    CHECK_EQ(yacm::util::parse_int_in_range("5", 2, 100), std::optional<int>(5));
    CHECK(!yacm::util::parse_int_in_range("1", 2, 100));
    CHECK(!yacm::util::parse_int_in_range("101", 2, 100));
    CHECK(!yacm::util::parse_int_in_range("5x", 2, 100));
}

void test_chat_repo_flood() {
    TempDatabase tmp;
    yacm::storage::ChatRepo repo(tmp.db());

    // unknown chat: antiflood off, schema defaults
    auto f = repo.flood_settings(42);
    CHECK(!f.enabled());
    CHECK_EQ(f.action, "mute");

    repo.set_flood_limit(42, 5);
    repo.set_flood_timer(42, 10, 30);
    repo.set_flood_action(42, "tmute", 600);
    repo.set_flood_clear(42, true);
    f = repo.flood_settings(42);
    CHECK(f.enabled());
    CHECK_EQ(f.limit, 5);
    CHECK_EQ(f.timer_count, 10);
    CHECK_EQ(f.timer_seconds, 30);
    CHECK_EQ(f.action, "tmute");
    CHECK_EQ(f.action_duration, 600);
    CHECK(f.clear);
    CHECK_EQ(repo.settings(42).flood.limit, 5);

    repo.set_flood_limit(42, 0);
    repo.set_flood_timer(42, 0, 0);
    CHECK(!repo.flood_settings(42).enabled());

    // timed warn mode is persisted alongside the action name
    repo.set_warn_action(42, "tban", 86400);
    CHECK_EQ(repo.warn_action(42), "tban");
    CHECK_EQ(repo.warn_action_duration(42), 86400);
    CHECK_EQ(repo.settings(42).warn_action_duration, 86400);
    repo.set_warn_action(42, "kick");
    CHECK_EQ(repo.warn_action_duration(42), 0);
}

void test_flood_tracker() {
    using yacm::services::FloodTracker;
    yacm::storage::FloodSettings s;
    s.limit = 3;

    FloodTracker t;
    // three in a row is fine, the fourth triggers
    CHECK(!t.record(1, 10, 100, 1000, s).triggered);
    CHECK(!t.record(1, 10, 101, 1000, s).triggered);
    CHECK(!t.record(1, 10, 102, 1000, s).triggered);
    auto v = t.record(1, 10, 103, 1000, s);
    CHECK(v.triggered);
    CHECK_EQ(v.flood_ids.size(), 4u);
    CHECK_EQ(v.excess_ids.size(), 1u);
    CHECK_EQ(v.excess_ids[0], 103);

    // state is reset after a trigger
    CHECK(!t.record(1, 10, 104, 1000, s).triggered);

    // someone else posting breaks the streak
    CHECK(!t.record(2, 10, 1, 1000, s).triggered);
    CHECK(!t.record(2, 10, 2, 1000, s).triggered);
    CHECK(!t.record(2, 10, 3, 1000, s).triggered);
    CHECK(!t.record(2, 11, 4, 1000, s).triggered);
    CHECK(!t.record(2, 10, 5, 1000, s).triggered);
    // chats are independent
    CHECK(!t.record(3, 10, 1, 1000, s).triggered);

    // timed limit: more than 2 messages within 10 seconds
    yacm::storage::FloodSettings ts;
    ts.timer_count = 2;
    ts.timer_seconds = 10;
    FloodTracker tt;
    CHECK(!tt.record(1, 10, 1, 100, ts).triggered);
    CHECK(!tt.record(1, 11, 2, 101, ts).triggered);  // other user interleaves
    CHECK(!tt.record(1, 10, 3, 105, ts).triggered);
    auto tv = tt.record(1, 10, 4, 109, ts);
    CHECK(tv.triggered);
    CHECK_EQ(tv.flood_ids.size(), 3u);
    CHECK_EQ(tv.excess_ids.size(), 1u);
    // old messages fall out of the window
    CHECK(!tt.record(1, 12, 5, 200, ts).triggered);
    CHECK(!tt.record(1, 12, 6, 211, ts).triggered);
    CHECK(!tt.record(1, 12, 7, 222, ts).triggered);

    // disabled settings never trigger
    yacm::storage::FloodSettings off;
    FloodTracker to;
    for (int i = 0; i < 50; ++i) CHECK(!to.record(1, 10, i, 100, off).triggered);
}

void test_timed_actions() {
    using yacm::services::Role;
    using yacm::services::ModerationService;
    using yacm::services::WarnService;

    constexpr int64_t kBot = 777;
    constexpr int64_t kChat = -100;

    FakeClient fake;
    fake.members[{kChat, kBot}] = {yacm::tg::User{kBot, true, "Bot", "", "", ""},
                                   "administrator"};
    TempDatabase tmp;
    yacm::storage::ChatRepo chats(tmp.db());
    yacm::storage::WarnRepo warns(tmp.db());
    yacm::storage::MuteRepo mutes(tmp.db());
    yacm::storage::BanRepo bans(tmp.db());
    yacm::storage::AuditRepo audit(tmp.db());
    ModerationService moderation(fake, kBot, tmp.db(), mutes, bans, audit);
    WarnService svc(tmp.db(), chats, warns, audit, kBot, moderation);

    // tban sends an absolute until_date; a plain ban sends none
    CHECK(moderation.ban(kChat, 1, 2, Role::Everyone, "x", 3600).ok);
    CHECK_EQ(fake.bans.size(), 1u);
    CHECK(fake.bans[0].until_date > 3600);
    CHECK(moderation.ban(kChat, 1, 3, Role::Everyone, "x").ok);
    CHECK_EQ(fake.bans[1].until_date, 0L);
    // over a year would be permanent on Telegram's side: refused
    const auto too_long = moderation.ban(kChat, 1, 4, Role::Everyone, "x", 400LL * 86400);
    CHECK(!too_long.ok);
    CHECK_EQ(too_long.error_key, "errors.invalid_duration");
    CHECK_EQ(fake.bans.size(), 2u);

    // apply() dispatches every kind
    using yacm::util::Action;
    CHECK(moderation.apply(kChat, 1, 5, Role::Everyone, Action{Action::Kind::TMute, 600}, "f")
              .ok);
    CHECK(fake.restricts.back().until_date > 600);
    CHECK(moderation.apply(kChat, 1, 5, Role::Everyone, Action{Action::Kind::Kick, 0}, "f").ok);
    CHECK_EQ(fake.unbans.size(), 1u);

    // warn limit with a timed mode: tmute fires with its duration
    chats.set_warn_action(kChat, "tmute", 1800);
    const std::size_t restricts_before = fake.restricts.size();
    svc.warn(kChat, 1, 9, Role::Everyone, "a");
    svc.warn(kChat, 1, 9, Role::Everyone, "b");
    const auto w3 = svc.warn(kChat, 1, 9, Role::Everyone, "c");
    CHECK(w3.ok);
    CHECK_EQ(w3.fired_action, "tmute");
    CHECK_EQ(w3.fired_duration, 1800);
    CHECK_EQ(fake.restricts.size(), restricts_before + 1);
    CHECK(fake.restricts.back().until_date > 1800);

    // resetwarn clears everything and reports how many
    svc.warn(kChat, 1, 10, Role::Everyone, "a");
    svc.warn(kChat, 1, 10, Role::Everyone, "b");
    const auto r = svc.reset(kChat, 1, 10, Role::Everyone);
    CHECK(r.ok);
    CHECK_EQ(r.removed, 2);
    CHECK_EQ(svc.count(kChat, 10), 0);
    // hierarchy still applies
    CHECK(!svc.reset(kChat, 1, 10, Role::Admin).ok);
}

// --- AI assistant -------------------------------------------------------------

class FakeAi : public yacm::ai::AiClient {
public:
    std::string reply = "Hello!";
    bool fail = false;
    std::vector<std::vector<yacm::ai::ChatMessage>> calls;
    yacm::ai::Completion complete(const std::vector<yacm::ai::ChatMessage>& messages) override {
        calls.push_back(messages);
        if (fail) throw yacm::ai::AiError(500, "boom");
        return {reply, 11, 7};
    }
};

void test_ai_protocol() {
    using yacm::ai::OpenAiClient;
    yacm::config::AiConfig cfg;
    cfg.model = "m";
    cfg.max_tokens = 123;
    auto body = OpenAiClient::build_request(cfg, {{"system", "s"}, {"user", "u"}});
    CHECK_EQ(body["model"], "m");
    CHECK_EQ(body["max_tokens"], 123);
    CHECK_EQ(body["messages"].size(), 2u);
    CHECK_EQ(body["messages"][1]["role"], "user");
    CHECK(!body.contains("temperature"));  // unset sampling params are omitted
    cfg.temperature = 0.5;
    CHECK_EQ(OpenAiClient::build_request(cfg, {})["temperature"], 0.5);

    const auto c = OpenAiClient::parse_response(
        200, R"({"choices":[{"message":{"role":"assistant","content":"hi"}}],)"
             R"("usage":{"prompt_tokens":42,"completion_tokens":5,"total_tokens":47}})");
    CHECK_EQ(c.text, "hi");
    CHECK_EQ(c.prompt_tokens, 42);
    CHECK_EQ(c.completion_tokens, 5);
    // providers without usage: unknown (-1), not 0
    const auto no_usage = OpenAiClient::parse_response(
        200, R"({"choices":[{"message":{"content":"x"}}]})");
    CHECK_EQ(no_usage.prompt_tokens, -1);
    bool threw = false;
    try {
        OpenAiClient::parse_response(401, R"({"error":{"message":"bad key"}})");
    } catch (const yacm::ai::AiError& e) {
        threw = true;
        CHECK_EQ(e.status, 401L);
        CHECK_EQ(std::string(e.what()), "bad key");
    }
    CHECK(threw);
    threw = false;
    try {
        OpenAiClient::parse_response(502, "<html>gateway</html>");
    } catch (const yacm::ai::AiError&) {
        threw = true;
    }
    CHECK(threw);
}

void test_ai_helpers() {
    using yacm::ai::is_silent;
    using yacm::ai::split_message;
    CHECK(is_silent(""));
    CHECK(is_silent("  none "));
    CHECK(is_silent("None."));
    CHECK(!is_silent("none of that"));

    CHECK_EQ(split_message("short").size(), 1u);
    const std::string long_text = std::string(30, 'a') + "\n" + std::string(30, 'b');
    const auto parts = split_message(long_text, 40);
    CHECK_EQ(parts.size(), 2u);
    CHECK_EQ(parts[0], std::string(30, 'a') + "\n");  // cut at the newline
    // never splits a multi-byte character ("я" is 2 bytes)
    std::string cyr;
    for (int i = 0; i < 50; ++i) cyr += "я";
    std::string joined;
    for (const auto& p : split_message(cyr, 7)) {
        CHECK(p.size() <= 7);
        CHECK(p.size() % 2 == 0);
        joined += p;
    }
    CHECK_EQ(joined, cyr);

    // file: values are read from disk; unreadable files keep the literal
    const auto path = std::filesystem::temp_directory_path() /
                      ("yacm-ai-" + std::to_string(::getpid()) + ".md");
    { std::ofstream(path) << "be kind"; }
    CHECK_EQ(yacm::ai::value_or_file("file:" + path.string()), "be kind");
    // a plain path works too (the bug: it used to be sent as literal text)
    CHECK_EQ(yacm::ai::value_or_file(path.string()), "be kind");
    CHECK_EQ(yacm::ai::value_or_file("  " + path.string() + "  "), "be kind");
    std::filesystem::remove(path);

    // relative "./dir/file.md" resolves against the working directory
    const auto dir = std::filesystem::current_path() /
                     ("yacm-ai-rel-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    { std::ofstream(dir / "ruleset.md") << "rule one"; }
    const std::string rel = "./" + dir.filename().string() + "/ruleset.md";
    CHECK_EQ(yacm::ai::value_or_file(rel), "rule one");
    std::filesystem::remove_all(dir);

    // literal text stays literal
    CHECK_EQ(yacm::ai::value_or_file("literal"), "literal");
    CHECK_EQ(yacm::ai::value_or_file("Be nice. Never swear."), "Be nice. Never swear.");
    // a missing file is never sent to the model as text
    CHECK_EQ(yacm::ai::value_or_file("file:/no/such/file"), "");
    CHECK_EQ(yacm::ai::value_or_file("./no/such/ruleset.md"), "");
    CHECK_EQ(yacm::ai::value_or_file("missing.md"), "");
}

void test_ai_assistant() {
    FakeAi fake;
    yacm::config::AiConfig cfg;
    cfg.rules = "No politics.";
    cfg.bio = "You are Duck.";
    yacm::ai::Assistant assistant(cfg, fake);

    const std::string sys = assistant.system_prompt();
    CHECK(sys.find("No politics.") != std::string::npos);
    CHECK(sys.find("You are Duck.") != std::string::npos);
    CHECK(sys.find("none") != std::string::npos);

    yacm::ai::Request r;
    r.chat_title = "Test Group";
    r.user_name = "Ada";
    r.text = "what is 2+2?";
    r.replied_text = "math time";
    auto msgs = assistant.build_messages(r);
    CHECK_EQ(msgs.size(), 2u);
    CHECK_EQ(msgs[1].role, "user");
    CHECK(msgs[1].content.find("Test Group") != std::string::npos);
    CHECK(msgs[1].content.find("math time") != std::string::npos);
    CHECK(msgs[1].content.find("Ada said: what is 2+2?") != std::string::npos);

    // recent chat messages come first, as "<author> said: <text>"
    r.history = {{"@bob", "hi all"}, {"Carol", "photo caption"}};
    msgs = assistant.build_messages(r);
    const std::string& u = msgs.back().content;
    CHECK(u.find("@bob said: hi all\nCarol said: photo caption") != std::string::npos);
    CHECK(u.find("@bob said") < u.find("Ada said: what is 2+2?"));
    r.history.clear();

    // replying to the bot continues the conversation as an assistant turn
    r.replied_is_bot = true;
    msgs = assistant.build_messages(r);
    CHECK_EQ(msgs.size(), 3u);
    CHECK_EQ(msgs[1].role, "assistant");
    CHECK_EQ(msgs[1].content, "math time");

    const auto answer = assistant.ask(r);
    CHECK_EQ(answer.text, "Hello!");
    CHECK_EQ(answer.prompt_tokens, 11);
    CHECK_EQ(answer.completion_tokens, 7);
    fake.reply = "none";
    CHECK(assistant.ask(r).text.empty());
    fake.fail = true;
    bool threw = false;
    try {
        assistant.ask(r);
    } catch (const yacm::ai::AiError&) {
        threw = true;
    }
    CHECK(threw);
}

void test_ai_rate_limiter() {
    yacm::ai::RateLimiter rl(/*user_cooldown_s=*/10, /*chat_per_min=*/3);
    CHECK(rl.allow(1, 100, 1000));
    CHECK(!rl.allow(1, 100, 1005));  // user cooldown
    CHECK(rl.allow(1, 100, 1010));
    CHECK(rl.allow(1, 101, 1011));
    CHECK(!rl.allow(1, 102, 1012));  // chat: 3 per minute
    CHECK(rl.allow(2, 102, 1012));   // other chats are independent
    CHECK(rl.allow(1, 102, 1061));   // window slid
}

void test_ai_worker() {
    FakeAi fake;
    yacm::ai::Assistant assistant(yacm::config::AiConfig{}, fake);
    std::mutex mu;
    std::vector<std::pair<int64_t, std::optional<std::string>>> delivered;
    std::vector<yacm::ai::Worker::Result> results;
    yacm::ai::Worker worker(
        assistant, fake,
        [&](const yacm::ai::Worker::Job& job, const yacm::ai::Worker::Result& r) {
            std::lock_guard<std::mutex> lk(mu);
            delivered.emplace_back(job.chat_id, r.answer);
            results.push_back(r);
        },
        /*queue_limit=*/10, spdlog::default_logger());
    worker.start();
    yacm::ai::Worker::Job job;
    job.chat_id = 5;
    job.request.text = "hi";
    CHECK(worker.submit(job));
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!delivered.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        CHECK_EQ(delivered.size(), 1u);
        if (!delivered.empty()) {
            CHECK_EQ(delivered[0].first, 5);
            CHECK_EQ(delivered[0].second, std::optional<std::string>("Hello!"));
            CHECK_EQ(results[0].prompt_tokens, 11);
            CHECK_EQ(results[0].completion_tokens, 7);
            CHECK(results[0].elapsed_ms >= 0);
        }
    }
    worker.shutdown();
    CHECK(!worker.submit(job));  // refused after shutdown
}

void test_chat_repo_ai() {
    TempDatabase tmp;
    yacm::storage::ChatRepo repo(tmp.db());
    CHECK(repo.ai_enabled(7));      // default on
    CHECK(!repo.ai_autoreply(7));   // default off
    repo.set_ai_enabled(7, false);
    repo.set_ai_autoreply(7, true);
    CHECK(!repo.ai_enabled(7));
    CHECK(repo.ai_autoreply(7));
    const auto s = repo.settings(7);
    CHECK(!s.ai_enabled);
    CHECK(s.ai_autoreply);
}

void test_command_throttle() {
    yacm::services::CommandThrottle t(/*max=*/5, /*window_ms=*/2000, /*block_ms=*/10000);
    // 5 commands within 2 s are fine
    for (int i = 0; i < 5; ++i) CHECK(t.allow(1, 1000 + i * 100));
    // the 6th within the window blocks the user (and is itself ignored)
    CHECK(!t.allow(1, 1500));
    CHECK(t.blocked(1, 1500));
    CHECK(!t.allow(1, 5000));       // still blocked
    CHECK(!t.allow(1, 11499));      // attempts don't extend the block
    CHECK(t.allow(1, 11500));       // 10 s after the block started
    // other users are unaffected
    CHECK(t.allow(2, 1500));
    // slow senders never trip it: 6 commands spread over > 2 s
    yacm::services::CommandThrottle slow;
    for (int i = 0; i < 20; ++i) CHECK(slow.allow(3, i * 500));
}

void test_chat_history() {
    yacm::services::ChatHistory h(5);
    CHECK(h.recent(1).empty());
    for (int i = 1; i <= 7; ++i) h.record(1, {"@u", "m" + std::to_string(i)});
    const auto r = h.recent(1);
    CHECK_EQ(r.size(), 5u);  // only the last 5
    CHECK_EQ(r.front().text, "m3");
    CHECK_EQ(r.back().text, "m7");
    h.record(1, {"@u", ""});  // image without caption: nothing to store
    CHECK_EQ(h.recent(1).back().text, "m7");
    CHECK(h.recent(2).empty());  // per chat
    // long messages are cut (UTF-8 safe)
    std::string big;
    for (int i = 0; i < 2000; ++i) big += "я";
    h.record(3, {"@u", big});
    CHECK(h.recent(3)[0].text.size() <= yacm::services::ChatHistory::kMaxTextBytes + 3);
}

void test_caption_parsing() {
    const auto j = nlohmann::json::parse(R"({
        "message_id": 1, "date": 5,
        "chat": {"id": -100, "type": "supergroup"},
        "from": {"id": 7, "first_name": "Ann", "username": "ann"},
        "photo": [{"file_id": "x"}],
        "caption": "look at this",
        "reply_to_message": {"message_id": 0, "date": 1, "photo": [], "caption": "old pic"}
    })");
    const auto m = j.get<yacm::tg::Message>();
    CHECK(m.text.empty());
    CHECK_EQ(m.content(), "look at this");
    CHECK(!m.is_command());
    CHECK_EQ(m.reply_to->content(), "old pic");
    CHECK_EQ(yacm::util::author_handle(*m.from), "@ann");
    CHECK_EQ(yacm::util::author_handle(yacm::tg::User{9, false, "Bo", "Li", "", ""}), "Bo Li");
}

void test_global_bans_and_settings() {
    TempDatabase tmp;
    yacm::storage::GlobalBanRepo bans(tmp.db());
    CHECK(!bans.is_banned(5));
    CHECK(bans.add(5, "spam", 1, 100));
    CHECK(!bans.add(5, "again", 1, 101));  // already banned
    CHECK(bans.is_banned(5));
    CHECK(bans.remove(5));
    CHECK(!bans.remove(5));
    CHECK(!bans.is_banned(5));

    yacm::storage::BotSettingsRepo settings(tmp.db());
    CHECK(!settings.flag("ai_debug"));
    CHECK(settings.flag("ai_debug", true));  // fallback
    settings.set_flag("ai_debug", true);
    CHECK(settings.flag("ai_debug"));
    settings.set_flag("ai_debug", false);
    CHECK(!settings.flag("ai_debug", true));
}

void test_router_own_command() {
    yacm::router::HandlerRegistry r;
    r.set_bot_username("MyBot");
    r.add("ping", [](const yacm::tg::Message&, const std::string&) {});
    yacm::tg::Message m;
    m.text = "/ping";
    CHECK(r.is_own_command(m));
    m.text = "/ping@mybot extra";
    CHECK(r.is_own_command(m));
    m.text = "/ping@OtherBot";
    CHECK(!r.is_own_command(m));
    m.text = "/unknown";
    CHECK(!r.is_own_command(m));
    m.text = "hello";
    CHECK(!r.is_own_command(m));
}

}  // namespace

int main() {
    test_duration();
    test_user_ref();
    test_warn_limit_lang();
    test_mute_tokens();
    test_format();
    test_i18n();
    test_yes_no();
    test_db_migrations();
    test_db_creates_parent_dir();
    test_user_repo();
    test_member_cache();
    test_chat_repo();
    test_warn_repo();
    test_mute_ban_repo();
    test_report_repo();
    test_answer_repo();
    test_audit_repo();
    test_transaction();
    test_roles();
    test_router();
    test_moderation_service();
    test_roles_live();
    test_warn_limit_action();
    test_parse_action();
    test_chat_repo_flood();
    test_flood_tracker();
    test_timed_actions();
    test_ai_protocol();
    test_ai_helpers();
    test_ai_assistant();
    test_ai_rate_limiter();
    test_ai_worker();
    test_chat_repo_ai();
    test_command_throttle();
    test_chat_history();
    test_caption_parsing();
    test_global_bans_and_settings();
    test_router_own_command();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}