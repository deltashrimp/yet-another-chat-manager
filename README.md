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
