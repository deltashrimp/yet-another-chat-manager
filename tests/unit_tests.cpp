#include "i18n/strings.hpp"
#include "router/router.hpp"
#include "services/moderation.hpp"
#include "services/roles.hpp"
#include "services/warns.hpp"
#include "storage/db.hpp"
#include "storage/repos.hpp"
#include "telegram/client.hpp"
#include "telegram/types.hpp"
#include "util/format.hpp"
#include "util/parsers.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
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
    CHECK_EQ(tmp.db().schema_version(), 3);
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
        CHECK_EQ(db.schema_version(), 3);
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
    void ban_chat_member(int64_t chat_id, int64_t user_id,
                         const std::string& reason) override {
        if (fail_actions) throw yacm::tg::TelegramError(403, "forbidden");
        bans.push_back({chat_id, user_id, reason});
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

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}