# yet-another-chat-manager

<div align="center">
    <img alt="YACM logo" src="./resources/images/Banner.png">
</div>

<br>

<div align="center"> 
    <a href="./README-RU.md"> <img alt="RUSSIAN" src="./resources/images/buttons/Russian.png" style="width: 130px; height: auto;"> </a>
    <a href="./README.md"> <img alt="ENGLISH" src="./resources/images/buttons/English.png" style="width: 130px; height: auto;"> </a>
</div>

---

## Установка

### Сборка через Cmake

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
&& cmake --build build -j \
&& ctest --test-dir build --output-on-failure
```

```sh
cp .env.example .env
```

> [!warning]
> Вставьте токен бота в поле BOT_TOKEN в файле `.env` 

```sh
./build/bot # Выполните от корня проекта (i18n/, resources/ относительны текущей директории)
```

> [!tip] 
> Выполните `./build/bot --healthcheck`, чтобы проверить доступ к БД + API (healthcheck контейнера).

### Docker

```sh
cp .env.example .env   # вставьте BOT_TOKEN
```

```sh
docker compose up -d --build
```

Логи идут в stdout (`LOG_LEVEL` настраивается в `.env`). Никаких логов нет внутри контейнера.

---

## Команды модерации

Формат времени: `30s`, `10m`, `2h`, `3d`, `1w`. Цель: `@username`, ID пользователя или ответ на сообщение.

| Действие | Обычное | На время | С удалением сообщения | Тихое | Отмена |
|---|---|---|---|---|---|
| Бан (владелец) | `/ban` | `/tban <время>` | `/dban` | `/sban` | `/unban` |
| Мьют | `/mute [время]` | `/tmute <время>` | `/dmute` | `/smute` | `/unmute` |
| Кик | `/kick` | — | `/dkick` | `/skick` | — |
| Варн | `/warn` | — | `/dwarn` | `/swarn` | `/unwarn` (`/rmwarn`), `/resetwarn` |

Тихие команды удаляют саму команду (и сообщение, на которое ответили) и ничего не пишут в чат.

**Варны:** `/warnlist` (`/warns`), `/warnings`, `/setwarnlimit <3-10>` (`/warnsetnumber`),
`/setwarnmode <ban|kick|mute|tban <время>|tmute <время>>` (`/warnsetaction`).

**Антифлуд** (по умолчанию выключен, на админов не действует): `/flood`, `/setflood <n|off>` (сообщений подряд),
`/setfloodtimer <n> <время>|off` (n сообщений за время), `/setfloodmode <действие>`,
`/clearflood <on|off>` (удалять весь флуд, а не только лишние сообщения).

**ИИ-ассистент** (по желанию, выключен, пока в `.env` не задан `AI_API_KEY`): любой
OpenAI-совместимый chat-completions API (по умолчанию Groq; подойдут OpenRouter, OpenAI, llama.cpp, Ollama).
`/ai <вопрос>` (или ответ на сообщение командой `/ai`), `/aichat <on|off>` разрешает ИИ в чате,
`/aireply <on|off>` разрешает отвечать на упоминания и ответы боту, `/aireload` (владелец бота)
перечитывает `AI_RULES` / `AI_BIO` (текст или `file:/путь`). В личке бот отвечает на каждое сообщение.
Запросы выполняются в фоновом потоке и ограничены (`AI_USER_COOLDOWN`, `AI_CHAT_PER_MIN`, `AI_QUEUE_LIMIT`).
К каждому запросу добавляются последние 5 сообщений чата (`@user said: текст`; подписи к фото учитываются,
сами картинки не читаются). `/aidebug` (владелец бота) добавляет к ответам ИИ число токенов и время ответа.

**Владелец бота:** `/gbban <id|@user|ответ> [причина]` — бот игнорирует пользователя везде
(команды, ИИ, репорты); `/ungbban` снимает бан.

**Защита от спама:** кто отправит больше 5 команд за 2 секунды, игнорируется 10 секунд.

---

## Дерево проекта

```
src/
  app/       main, AppContext (composition root), graceful shutdown
  config/    парсинг env
  telegram/  типы, клиент Bot API, рейт лимиты
  router/    регистрация слеш команд и парсинг
  handlers/  хендлеры команд
  services/  логика модерации (роли, варны)
  storage/   SQLite
  i18n/      переводы
  util/      парсеры аргументов и периодов, форматирование
```

---

## Разработчи

Deltashrimp, Big Pickle (ИИ), Claude (ИИ)
