# Настройка учёта рабочего времени (Time tracking)

Кнопки «Пришла» / «Ушла» в Telegram Web App пишут записи в таблицу
**Time tracking** (`1zhWZRFI6GYYnh6PNQn37KiSyPLvjkSwJz-CKwF0xEnw`).

## Структура листа

Вкладка создаётся автоматически и называется по текущему месяцу и году
(например, `Октябрь 2026`). Колонки:

| A | B | C | D | E | F |
|---|---|---|---|---|---|
| Дата | Время прихода | Время ухода | Время в часах | Ставка в час | Сумма за день |

Блоки разных сотрудников разделены пустой строкой. Перед блоком стоит строка
с ФИО сотрудника.

## Почему нужен Service Account

Google Sheets API **запрещает запись по API key** (`401: API keys are not
supported by this API`). Поэтому для записи используется OAuth2 — JWT (RS256),
подписанный приватным ключом Service Account.

## Настройка (один раз)

### 1. Создать Service Account

1. Откройте https://console.cloud.google.com/
2. Создайте проект (или выберите существующий).
3. **IAM & Admin → Service Accounts → Create service account**
4. Имя, например `sheets-tracker`. Нажмите **Create and continue**.
5. Роль (**Grant this service account access to the project**) — **не нужна**,
   снимите галочку или оставьте пустым, нажмите **Continue**.
6. На экране ключей: **Create new key → JSON**. Скачается файл
   вида `sheets-tracker-xxxxx.json`.

### 2. Дать доступ к таблице

1. Откройте таблицу Time tracking:
   https://docs.google.com/spreadsheets/d/1zhWZRFI6GYYnh6PNQn37KiSyPLvjkSwJz-CKwF0xEnw/edit
2. Кнопка **Поделиться** (Share) → **Добавить пользователей и группы**.
3. В поле вставьте email сервисного аккаунта:
   `sheets-tracker@ВАШ_ПРОЕКТ.iam.gserviceaccount.com`
4. Роль: **Редактор** (Editor).
5. Нажмите **Отправить** / **Готово**.

> Важно: если таблица закрыта («Ограниченный доступ»), пригласите и себя.

### 3. Задать переменные окружения в Render

В панели Render: **ваш сервис → Environment → Add Environment Variable**

| Переменная | Значение |
|------------|----------|
| `TRACKING_SHEET_ID` | `1zhWZRFI6GYYnh6PNQn37KiSyPLvjkSwJz-CKwF0xEnw` |
| `GOOGLE_SERVICE_ACCOUNT_JSON` | **Весь содержимое скачанного JSON-файла** (одной строкой) |
| `EMPLOYEE_MAP` | `{"123456789":"Бинкевич Татьяна Эдуардовна"}` |
| `EMPLOYEE_RATES` | `{"Бинкевич Татьяна Эдуардовна":"500"}` |
| `HOURLY_RATE` | `500` (ставка по умолчанию, если нет в `EMPLOYEE_RATES`) |
| `TZ_OFFSET_SECONDS` | `10800` (Москва = UTC+3). По умолчанию 10800 |

Содержимое JSON-файла сервисного аккаунта — **одна длинная строка**. Пример:

```
{"type":"service_account","project_id":"my-project","private_key_id":"abc...","private_key":"-----BEGIN PRIVATE KEY-----\nMIIEvQ...\n-----END PRIVATE KEY-----\n","client_email":"sheets-tracker@my-project.iam.gserviceaccount.com","client_id":"123","token_uri":"https://oauth2.googleapis.com/token"}
```

Переносы строк внутри JSON уже экранированы как `\n` — вставляйте как есть.

### 4. Как узнать chat_id сотрудника

chat_id — это числовой ID пользователя в Telegram. Проще всего:
1. Создайте бота через [@BotFather](https://t.me/BotFather) (или используйте
   существующего), отправьте ему `/start`.
2. Откройте `https://api.telegram.org/bot<ТОКЕН>/getUpdates` — в ответе будет
   `"chat":{"id":123456789,...}`.

Либо: сотрудник открывает Mini App, нажимает кнопку, а сервер пишет в лог
`[track] chat <id> not in EMPLOYEE_MAP, using telegram name '<ФИО>'` — значит,
в `EMPLOYEE_MAP` нужно добавить этот ID с правильным ФИО.

### 5. Деплой

После добавления переменных Render пересоберёт сервис. Затем проверьте:

```bash
# должно вернуть JSON с ok:false и error:unauthorized (без initData) —
# значит эндпоинт живой
curl "https://sheets-server-c.onrender.com/api/tracking/summary"

# проверка доступа к таблице (нужен OAuth — проверится при первом приходе)
```

## Как это работает

1. Telegram Web App отправляет на сервер `initData` (подписанные данные).
2. Сервер проверяет подпись HMAC-SHA256 (ключ — токен бота) — так нельзя
   подделать сотрудника.
3. По `EMPLOYEE_MAP` определяется ФИО и ставка.
4. Сервер находит вкладку текущего месяца (создаёт, если её нет), ищет строку
   сотрудника и его строку за сегодня:
   - **«Пришла»** — записывает время прихода (или заполняет существующую строку).
   - **«Ушла»** — записывает время ухода, считает часы и сумму.
5. Часы = (уход − приход) в часах, Сумма = часы × ставка.

## Ограничения

- Одна запись на сотрудника в день (повторное «Пришла» вернёт ошибку 409).
- Данные пишутся в таблицу с задержкой на стороне Google (обычно мгновенно).
- Сервер хранит access_token в памяти и обновляет его раз в ~58 минут.
