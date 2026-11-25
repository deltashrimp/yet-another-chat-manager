#include "i18n/strings.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>

namespace yacm::i18n {
namespace {

using Catalog = std::map<std::string, std::string>;

Catalog make_catalog(std::initializer_list<std::pair<const std::string, std::string>> items) {
    return Catalog(items);
}

}  // namespace

Strings::Strings(std::string default_lang) : default_lang_(std::move(default_lang)) {
    if (default_lang_.empty()) default_lang_ = "en";
    add_builtin_en();
    add_builtin_ru();
}

void Strings::add_builtin_en() {
    data_["en"] = make_catalog({
        {"errors.only_private", "This command is only available in private chats."},
        {"errors.only_groups", "This command is only available in group chats."},
        {"errors.user_not_found", "User not found in cache"},
        {"errors.not_permitted", "You do not have permission to use this command."},
        {"errors.not_enough_rights", "I do not have enough rights to do this in this chat."},
        {"errors.cannot_restrict_owner", "Cannot restrict the chat owner."},
        {"errors.cannot_restrict_admin", "Cannot restrict another administrator."},
        {"errors.cannot_restrict_bot", "Cannot apply moderation actions to the bot itself."},
        {"errors.invalid_duration", "Invalid duration. Use e.g. 30s, 10m, 2h, 3d, 1w."},
        {"errors.moderation_failed", "The moderation action failed. Try again later."},
        {"errors.invalid_warn_limit", "Invalid warn limit. Use a number from 3 to 10."},
        {"errors.invalid_warn_action", "Invalid action. Use ban, kick or mute."},
        {"errors.rules_need_reply", "Reply to a message to use this command."},
        {"errors.invalid_lang", "Unsupported language. Available: {0}"},
        {"errors.report_need_reply", "Reply to a message to report it."},
        {"errors.report_self", "You cannot report your own message."},
        {"errors.reports_disabled", "Reports are disabled in this chat."},
        {"errors.cannot_report_bot", "You cannot report the bot."},
        {"errors.cannot_report_owner", "You cannot report the chat owner."},
        {"errors.cannot_report_admin", "You cannot report an administrator."},
        {"errors.report_already", "You have already reported this message."},
        {"errors.report_already_target", "You have already reported this user."},
        {"errors.invalid_yes_no", "Invalid value. Use yes/no, y/n, on/off or true/false."},
        {"errors.answer_need_args", "Usage: /answer <command> <yes/no y/n on/off true/false>"},
        {"errors.answer_unknown_command", "Unknown command: /{0}"},
        {"errors.answer_forbidden_command", "Cannot toggle answers for /{0}"},
        {"moderation.banned", "✓ Banned {0}"},
        {"moderation.unbanned", "✓ Unbanned {0}"},
        {"moderation.kicked", "✓ Kicked {0}"},
        {"moderation.muted", "✓ Muted {0}"},
        {"moderation.muted_for", "✓ Muted {0} for {1}"},
        {"moderation.unmuted", "✓ Unmuted {0}"},
        {"warns.given", "! Playing with fire, {0}. You have {1} out of {2} warns"},
        {"warns.removed", "✓ False positive, {0}. You now have {1} out of {2} warns"},
        {"warns.list_title", "Warns for {0} ({1}):"},
        {"warns.empty", "No warns."},
        {"warns.no_reason", "No reason"},
        {"warns.limit_ban", "✓ {0} was banned for reaching warn limit"},
        {"warns.limit_kick", "✓ {0} was kicked for reaching warn limit"},
        {"warns.limit_mute", "✓ {0} was muted for reaching warn limit"},
        {"reports.sent", "✓ Sent report to admins"},
        {"reports.admin_notice", "{0} reported {1} in {2}. Link: {3}"},
        {"reports.admin_notice_plain", "{0} reported {1} in {2}."},
        {"reports.on", "on"},
        {"reports.off", "off"},
        {"settings.reports_set", "✓ Reports are {0}"},
        {"settings.answer_set", "✓ Answers are {0} for /{1}"},
        {"answerlist.title", "Answers for {0}:"},
        {"answerlist.line", "{0} > {1}"},
        {"settings.title", "Settings for {0}:"},
        {"settings.language", "Language     > {0}"},
        {"settings.reports", "Reports      > {0}"},
        {"settings.warn_limit", "Warn limit   > {0}"},
        {"settings.warn_action", "Warn action  > {0}"},
        {"settings.rules", "Rules        > {0}"},
        {"settings.rules_set", "set"},
        {"settings.rules_not_set", "not set"},
        {"settings.warn_limit_set", "✓ Set warn limit to {0}"},
        {"settings.warn_action_set", "✓ Set action after warn to {0}"},
        {"settings.lang_changed", "✓ Changed language to {0}"},
        {"lang.name.en", "English"},
        {"lang.name.ru", "Russian"},
        {"id.user", "User > "},
        {"id.id", "ID   > "},
        {"idg.group", "Group > "},
        {"idg.id", "ID    > "},
        {"idg.untitled", "Untitled"},
        {"rules.title", "Rules of this chat:"},
        {"rules.not_set", "Rules are not set yet."},
        {"rules.updated", "✓ Updated rules with {0}"},
        {"rules.updated_plain", "✓ Updated rules"},
        {"rules.this", "this"},
        {"rules.cleared", "✓ Cleared rules"},
        {"general.pong", "Pong!"},
        {"general.version", "yacm v{0} (build {1})"},
        {"help.title", "Available commands:"},
        {"help.dm_title", "Commands available here:"},
        {"help.none", "No commands available."},
        {"commands.start", "Show the welcome message"},
        {"commands.help", "Show this help"},
        {"commands.ping", "Check that the bot is alive"},
        {"commands.version", "Show bot version and build"},
        {"commands.id", "Get a user's Telegram ID"},
        {"commands.idg", "Get this chat's ID"},
        {"commands.admins", "List chat administrators"},
        {"commands.rules", "Show chat rules"},
        {"commands.ban", "Ban a user (owner)"},
        {"commands.unban", "Unban a user (owner)"},
        {"commands.kick", "Kick a user"},
        {"commands.mute", "Mute a user"},
        {"commands.unmute", "Unmute a user"},
        {"commands.warn", "Warn a user"},
        {"commands.unwarn", "Remove a warn"},
        {"commands.warnlist", "List a user's warns"},
        {"commands.warnsetnumber", "Set warn limit (owner)"},
        {"commands.warnsetaction", "Set action after warn limit (owner)"},
        {"commands.ruleset", "Set chat rules from a reply (owner)"},
        {"commands.rulereset", "Clear chat rules (owner)"},
        {"commands.lang", "Change bot language in this chat (owner)"},
        {"commands.report", "Report a message to admins"},
        {"commands.reports", "Enable or disable reports (owner)"},
        {"commands.answer", "Toggle success answers for a command (owner)"},
        {"commands.answerlist", "List per-command answers (owner)"},
        {"commands.settings", "Show chat settings (owner)"},
        {"admins.title", "Admins in this chat:"},
        {"admins.failed", "Could not fetch the admin list right now. Try again later."},
    });
    supported_langs_.push_back("en");
}

void Strings::add_builtin_ru() {
    data_["ru"] = make_catalog({
        {"errors.only_private", "Эта команда доступна только в личных сообщениях."},
        {"errors.only_groups", "Эта команда доступна только в чатах."},
        {"errors.user_not_found", "Пользователь не найден в кэше"},
        {"errors.not_permitted", "У вас нет прав для этой команды."},
        {"errors.not_enough_rights", "У меня недостаточно прав для этого в этом чате."},
        {"errors.cannot_restrict_owner", "Нельзя применить действие к владельцу чата."},
        {"errors.cannot_restrict_admin", "Нельзя применить действие к администратору чата."},
        {"errors.cannot_restrict_bot", "Нельзя применять модерацию к самому боту."},
        {"errors.invalid_duration", "Некорректная длительность. Пример: 30s, 10m, 2h, 3d, 1w."},
        {"errors.moderation_failed", "Не удалось выполнить действие. Попробуйте позже."},
        {"errors.invalid_warn_limit", "Некорректный лимит варнов. Используйте число от 3 до 10."},
        {"errors.invalid_warn_action", "Некорректное действие. Используйте ban, kick или mute."},
        {"errors.rules_need_reply", "Ответьте на сообщение, чтобы использовать эту команду."},
        {"errors.invalid_lang", "Неподдерживаемый язык. Доступно: {0}"},
        {"errors.report_need_reply", "Ответьте на сообщение, чтобы пожаловаться."},
        {"errors.report_self", "Нельзя пожаловаться на собственное сообщение."},
        {"errors.reports_disabled", "Репорты отключены в этом чате."},
        {"errors.cannot_report_bot", "Нельзя пожаловаться на самого бота."},
        {"errors.cannot_report_owner", "Нельзя пожаловаться на владельца чата."},
        {"errors.cannot_report_admin", "Нельзя пожаловаться на администратора чата."},
        {"errors.report_already", "Вы уже жаловались на это сообщение."},
        {"errors.report_already_target", "Вы уже жаловались на этого пользователя."},
        {"errors.invalid_yes_no", "Некорректное значение. Используйте yes/no, y/n, on/off или true/false."},
        {"errors.answer_need_args", "Использование: /answer <команда> <yes/no y/n on/off true/false>"},
        {"errors.answer_unknown_command", "Неизвестная команда: /{0}"},
        {"errors.answer_forbidden_command", "Нельзя отключить ответы для /{0}"},
        {"moderation.banned", "✓ Забанен {0}"},
        {"moderation.unbanned", "✓ Разбанен {0}"},
        {"moderation.kicked", "✓ Кикнут {0}"},
        {"moderation.muted", "✓ Замьючен {0}"},
        {"moderation.muted_for", "✓ Замьючен {0} на {1}"},
        {"moderation.unmuted", "✓ Размьючен {0}"},
        {"warns.given", "! Играешь с огнём, {0}. У тебя {1} из {2} варнов"},
        {"warns.removed", "✓ Ложная тревога, {0}. Теперь у тебя {1} из {2} варнов"},
        {"warns.list_title", "Варны для {0} ({1}):"},
        {"warns.empty", "Варнов нет."},
        {"warns.no_reason", "Без причины"},
        {"warns.limit_ban", "✓ {0} был забанен за превышение лимита варнов"},
        {"warns.limit_kick", "✓ {0} был кикнут за превышение лимита варнов"},
        {"warns.limit_mute", "✓ {0} был замьючен за превышение лимита варнов"},
        {"reports.sent", "✓ Жалоба отправлена администраторам"},
        {"reports.admin_notice", "{0} пожаловался на {1} в {2}. Ссылка: {3}"},
        {"reports.admin_notice_plain", "{0} пожаловался на {1} в {2}."},
        {"reports.on", "включены"},
        {"reports.off", "выключены"},
        {"settings.reports_set", "✓ Репорты: {0}"},
        {"settings.answer_set", "✓ Ответы: {0} для /{1}"},
        {"answerlist.title", "Ответы для {0}:"},
        {"answerlist.line", "{0} > {1}"},
        {"settings.title", "Настройки для {0}:"},
        {"settings.language", "Язык        > {0}"},
        {"settings.reports", "Репорты     > {0}"},
        {"settings.warn_limit", "Лимит варнов > {0}"},
        {"settings.warn_action", "Действие после варнов > {0}"},
        {"settings.rules", "Правила     > {0}"},
        {"settings.rules_set", "заданы"},
        {"settings.rules_not_set", "не заданы"},
        {"settings.warn_limit_set", "✓ Лимит варнов: {0}"},
        {"settings.warn_action_set", "✓ Действие после варнов: {0}"},
        {"settings.lang_changed", "✓ Язык изменён на {0}"},
        {"lang.name.en", "Английский"},
        {"lang.name.ru", "Русский"},
        {"id.user", "Пользователь > "},
        {"id.id", "ID   > "},
        {"idg.group", "Группа > "},
        {"idg.id", "ID    > "},
        {"idg.untitled", "Без названия"},
        {"rules.title", "Правила этого чата:"},
        {"rules.not_set", "Правила пока не заданы."},
        {"rules.updated", "✓ Правила обновлены: {0}"},
        {"rules.updated_plain", "✓ Правила обновлены"},
        {"rules.this", "это сообщение"},
        {"rules.cleared", "✓ Правила сброшены"},
        {"general.pong", "Понг!"},
        {"general.version", "yacm v{0} (сборка {1})"},
        {"help.title", "Доступные команды:"},
        {"help.dm_title", "Команды, доступные здесь:"},
        {"help.none", "Нет доступных команд."},
        {"commands.start", "Показать приветствие"},
        {"commands.help", "Показать справку"},
        {"commands.ping", "Проверка живости бота"},
        {"commands.version", "Версия бота и сборки"},
        {"commands.id", "Telegram ID пользователя"},
        {"commands.idg", "ID текущего чата"},
        {"commands.admins", "Список администраторов чата"},
        {"commands.rules", "Показать правила чата"},
        {"commands.ban", "Забанить пользователя (владелец)"},
        {"commands.unban", "Разбанить пользователя (владелец)"},
        {"commands.kick", "Кикнуть пользователя"},
        {"commands.mute", "Замьютить пользователя"},
        {"commands.unmute", "Размьютить пользователя"},
        {"commands.warn", "Выдать варн"},
        {"commands.unwarn", "Снять варн"},
        {"commands.warnlist", "Список варнов пользователя"},
        {"commands.warnsetnumber", "Лимит варнов (владелец)"},
        {"commands.warnsetaction", "Действие после лимита варнов (владелец)"},
        {"commands.ruleset", "Установить правила чата (владелец)"},
        {"commands.rulereset", "Сбросить правила чата (владелец)"},
        {"commands.lang", "Язык бота в этом чате (владелец)"},
        {"commands.report", "Пожаловаться на сообщение администраторам"},
        {"commands.reports", "Включить или выключить репорты (владелец)"},
        {"commands.answer", "Вкл/выкл ответы для команды (владелец)"},
        {"commands.answerlist", "Ответы по командам (владелец)"},
        {"commands.settings", "Настройки чата (владелец)"},
        {"admins.title", "Администраторы этого чата:"},
        {"admins.failed", "Не удалось получить список администраторов. Попробуйте позже."},
    });
    supported_langs_.push_back("ru");
}

void Strings::load_dir(const std::string& dir) {
    for (const auto& lang : supported_langs_) {
        const std::string path = dir + "/" + lang + ".json";
        std::ifstream in(path);
        if (!in) {
            spdlog::debug("translations file not found: {}", path);
            continue;
        }

        std::ostringstream ss;
        ss << in.rdbuf();

        try {
            auto jobj = nlohmann::json::parse(ss.str());
            auto& catalog = data_[lang];
            for (auto it = jobj.begin(); it != jobj.end(); ++it) {
                if (it.value().is_string()) {
                    catalog[it.key()] = it.value().get<std::string>();
                }
            }
            spdlog::info("loaded translations: {} ({} keys)", path, catalog.size());
        } catch (const std::exception& e) {
            spdlog::warn("failed to load translations {}: {}", path, e.what());
        }
    }
}

std::string Strings::t(const std::string& lang, const std::string& key) const {
    auto lang_it = data_.find(lang);
    if (lang_it != data_.end()) {
        auto it = lang_it->second.find(key);
        if (it != lang_it->second.end()) return it->second;
    }
    auto en_it = data_.find("en");
    if (en_it != data_.end()) {
        auto it = en_it->second.find(key);
        if (it != en_it->second.end()) return it->second;
    }
    return key;
}

std::string Strings::t(const std::string& key) const { return t(default_lang_, key); }

bool Strings::is_supported(const std::string& lang) const {
    return data_.find(lang) != data_.end();
}

std::vector<std::string> Strings::supported_langs() const { return supported_langs_; }

}  // namespace yacm::i18n