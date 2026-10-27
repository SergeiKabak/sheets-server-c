# sheets-server-c

Telegram-бот и Mini App для графика смен: показывает таблицу смен из Google
Sheets, считает рабочее время и заработок, знает, кто сегодня с кем работает
в кабинете. Чистый C + Docker.

## Что умеет

- **График смен** — таблица по месяцам в Mini App (данные — из Google Sheets,
  через буфер Supabase и файловые кэши отвечает за миллисекунды).
- **Коллеги по кабинету** — тап по ячейке кабинета показывает, кто работает
  в этот день там же (`/api/cabinet`).
- **Учёт времени** — кнопки «Пришла»/«Ушла» в боте и в Mini App, заполнение
  любого дня, правка и удаление отметок, итог заработка по месяцам.
  БД (Supabase) — первоисточник, Google-лист «Time tracking» — архив.
- **Светлая и тёмная темы** — кнопка 🌙/☀️ в шапке Mini App. По умолчанию
  берётся тема Telegram (или системная), ручной выбор запоминается
  в `localStorage` и приоритетнее темы Telegram.
- **Диагностика** — `/diag`, `/diag/cabinet`, `/diag/supabase`,
  `/diag/tracking`, `/health`, `/version`.

## Сборка и запуск

```bash
make build      # бинарник server_app (исходники в src/)
make run        # сборка + запуск с подгрузкой .env
```

Переменные окружения (минимум): `BOT_TOKEN`, `SPREADSHEET_ID`,
`GOOGLE_API_KEY`, `FROM_COL`, `TO_COL`, `SEARCH_NAME`, `MINIAPP_URL`, `PORT`.
Для учёта времени: `TRACKING_SHEET_ID`, `GOOGLE_SERVICE_ACCOUNT_JSON`,
`HOURLY_RATE`. Для Supabase: `SUPABASE_URL`, `SUPABASE_SECRET_KEY`.
Подробно — в `docs/TIME_TRACKING_SETUP.md` и `Dockerfile`.

## Тесты

```bash
make test-core       # чистое ядро: даты, смены, JSON, лист (без сети)
make test-supabase   # буфер Supabase и разборы domain-слоя (офлайн)
make test            # сценарии учёта времени с подменённым HTTP-слоем
make test-js         # логика Mini App (нужен jsdom)
```

## Структура кода

Исходники разбиты по слоям, зависимости — только сверху вниз:

```
src/core    чистые функции (нет I/O, нет состояния)
src/domain  модель предметной области
src/infra   адаптеры внешнего мира (HTTP, Google, Supabase, Telegram, ФС)
src/app     сценарии использования
src/web     HTTP-обработчики
src/server  приём соединений и маршрутизация
src/main.c  composition root
```

Подробная карта модулей и принципы — в **`docs/ARCHITECTURE.md`**.

## Документация

| Документ | О чём |
|----------|-------|
| `docs/ARCHITECTURE.md` | слои, модули, принципы (чистое ядро, явные зависимости) |
| `docs/TIME_TRACKING_SETUP.md` | настройка учёта времени: Service Account, переменные, API |
| `docs/SUPABASE_PLAN.md` | переход на «БД первично», этапы, ускорения, замеры |
| `docs/DEPLOY_CABINET.md` | эндпоинт «кто в кабинете» и его фронтенд |
| `supabase/schema.sql` | DDL таблиц Supabase |

## Деплой

Docker на Render: `Dockerfile` собирает `server_app` из `src/` и кладёт
статику Mini App из `web/`. Проверка задеплоенного коммита — `GET /version`.
