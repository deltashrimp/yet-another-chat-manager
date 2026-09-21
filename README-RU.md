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
