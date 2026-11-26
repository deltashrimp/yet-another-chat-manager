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

## Install

### Build with cmake

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
&& cmake --build build -j \
&& ctest --test-dir build --output-on-failure
```

```sh
cp .env.example .env
```

> [!warning]
> Fill in BOT_TOKEN in `.env` file 

```sh
./build/bot # run from repo root (i18n/, resources/ are relative)
```

> [!tip] 
> Run `./build/bot --healthcheck` to verify DB + API access (used as the container healthcheck).

### Docker

Single-container deployment (Dockerfile + docker-compose.yml):

```sh
cp .env.example .env   # fill in BOT_TOKEN
```

```sh
docker compose up -d --build
```

Logs go to stdout (`LOG_LEVEL` from `.env`); no log rotation inside the
container.

---

## Moderation commands

Time format: `30s`, `10m`, `2h`, `3d`, `1w`. Targets are `@username`, a user ID, or a reply.

| Action | Plain | Timed | Delete replied msg | Silent | Undo |
|---|---|---|---|---|---|
| Ban (owner) | `/ban` | `/tban <time>` | `/dban` | `/sban` | `/unban` |
| Mute | `/mute [time]` | `/tmute <time>` | `/dmute` | `/smute` | `/unmute` |
| Kick | `/kick` | — | `/dkick` | `/skick` | — |
| Warn | `/warn` | — | `/dwarn` | `/swarn` | `/unwarn` (`/rmwarn`), `/resetwarn` |

Silent variants delete the command (and the replied message) and send no notice.

**Warnings:** `/warnlist` (`/warns`), `/warnings`, `/setwarnlimit <3-10>` (`/warnsetnumber`),
`/setwarnmode <ban|kick|mute|tban <time>|tmute <time>>` (`/warnsetaction`).

**Antiflood** (off by default, admins exempt): `/flood`, `/setflood <n|off>` (consecutive messages),
`/setfloodtimer <n> <time>|off` (n messages within a time window), `/setfloodmode <action>`,
`/clearflood <on|off>` (delete the whole flood instead of only the excess).

**AI assistant** (optional, off until `AI_API_KEY` is set in `.env`): any OpenAI-compatible
chat-completions API (Groq by default; OpenRouter, OpenAI, llama.cpp, Ollama work too).
`/ai <question>` (or reply to a message with `/ai`), `/aichat <on|off>` allows AI in a chat,
`/aireply <on|off>` lets it answer mentions and replies to the bot, `/aireload` (bot owner)
re-reads `AI_RULES` / `AI_BIO`, which accept literal text or `file:/path`. In private chats
the bot answers every message. Requests run on a background thread and are rate-limited
(`AI_USER_COOLDOWN`, `AI_CHAT_PER_MIN`, `AI_QUEUE_LIMIT`). Every AI request includes the chat's last
5 messages as context (`@user said: text`; photo captions count, images are not read).
`/aidebug` (bot owner) appends input/output tokens and response time to AI answers.

**Bot owner:** `/gbban <id|@user|reply> [reason]` makes the bot ignore a user everywhere
(commands, AI, reports); `/ungbban` lifts it.

**Spam protection:** a user who sends more than 5 commands within 2 seconds is ignored for 10 seconds.

---

## Layout

```
src/
  app/       main, AppContext (composition root), graceful shutdown
  config/    env parsing
  telegram/  types, Bot API client, rate-limited sender
  router/    slash-command registry + parsing
  handlers/  command handlers
  services/  business logic (roles, later moderation/warns)
  storage/   SQLite + repositories
  i18n/      translations
  util/      argument/duration parsers, formatting
```
---

## Developers

Deltashrimp, Big Pickle (AI), Claude (AI)
