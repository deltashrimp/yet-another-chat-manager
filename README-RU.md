# yet-another-chat-manager

<div align="center">

    [🇷🇺 Русский](./README-RU.md) 
    [🇬🇧 English](./README.md)

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
cp .env.example .env   # fill in BOT_TOKEN
```

```sh
docker compose up -d --build
```

Логи идут в stdout (`LOG_LEVEL` from `.env`). Никаких логов внутри контейнера.

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
  storage/   SQLite + репозитории
  i18n/      переводы
  util/      парсеры аргументов и периодов, форматирование
```
