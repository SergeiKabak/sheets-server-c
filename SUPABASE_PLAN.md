# План: график и учёт времени в Supabase (PostgreSQL)

Цель: буфер графика из Google Sheets в SQL для мгновенного доступа в будущем,
надёжный учёт времени и независимость от квот/удалений бесплатных планов.

## Схема данных

DDL: **`supabase/schema.sql`** (выполняется один раз в дашборде Supabase → SQL Editor).

| Таблица | Назначение |
|---|---|
| `schedule` | буфер графика: месяц/день/сотрудник/смена/кабинет, PK `(month, day, employee, shift_time)` |
| `sync_log` | «пульс» синхронизации каждые ~10 мин + ошибки |
| `time_entries` | учёт времени (деньги) — этап 3 |

RLS включён: анонимный ключ ничего не видит, пишет только сервер
(`SUPABASE_SECRET_KEY` = service_role, только в окружении сервера).

## Архитектура потоков

```
Google Sheets (график правят люди)
   │  каждые 60 с: background_updater → sb_row(...) при обходе листов
   ▼
Supabase schedule   ← этап 1 (ГОТОВО): upsert + уборка устаревшего
   ├─► этап 2 (план): чтение /api/month и коллег из SQL (клиент не меняется)
   └─► этап 4 (план): ночной экспорт time_entries в Google Sheets (страховка)
Supabase time_entries ◄─ этап 3 (план): fill/clock пишут сюда (write-through)
```

Реализация этапа 1 (через существующий curl-слой, без libpq):
- `supabase.c/h` — накопление строк по месяцам, дедуп PK, экранирование JSON;
- `sb_flush()` на каждый цикл: `POST /rest/v1/schedule` (upsert, merge-duplicates),
  `DELETE ... month=eq.X & updated_at=lt.<cutoff>` (уборка),
  очистка месяцев, пропавших из Google;
- строки собираются и для самого сотрудника (`sb_add_month_rows`), и для коллег
  (в том же цикле, что и индекс кабинетов);
- вкл/выкл: `SUPABASE_URL` + `SUPABASE_SECRET_KEY`, принудительное выключение `SUPABASE_SYNC=0`;
- при недоступности БД — ошибки в лог, сервис работает как раньше (файловые кэши не тронуты).

## Чеклист этапов

- [x] **Этап 0.** DDL — `supabase/schema.sql` *(ждём запуска в SQL Editor)*
- [x] **Этап 1.** Запись графика в Supabase каждые 60 с (`supabase.c`, `background_updater`)
- [x] **Побочно исправлено:** `curl_mini.h` — 5 неверных констант curl (см. ниже)
- [ ] **Этап 0b.** Выполнить `supabase/schema.sql` в дашборде Supabase
- [ ] **Этап 0c.** Добавить `SUPABASE_URL` и `SUPABASE_SECRET_KEY` в окружение Render
- [ ] **Этап 1b.** Проверить в дашборде: строки `schedule` растут, `sync_log` пульсирует
- [ ] **Этап 2.** Чтение `/api/month` и коллег из SQL (фоллбэк на файлы/Google)
- [ ] **Этап 3.** `time_entries`: запись fill/clock + чтение `/api/tracking/days`
- [ ] **Этап 4.** Ночной экспорт `time_entries` → Google Sheets (страховка free-плана)
- [ ] **Этап 5.** Чистка файловых кэшей `/tmp` и фоновых чтений Google

## Ключевой баг, найденный при этапе 1: `curl_mini.h`

Шим `<curl/curl.h>` содержал 5 неверных констант (сверены с настоящим curl.h):

| Константа | было | стало (реальное) |
|---|---|---|
| `CURLINFO_RESPONSE_CODE` | 2097 | **2097154** |
| `CURLOPT_HEADERFUNCTION` | 20027 | **20079** |
| `CURLOPT_POSTFIELDSIZE` | 10014 | **60** |
| `CURLOPT_SSL_VERIFYPEER` | 81 | **64** |
| `CURLOPT_POST` | 41 | **47** |

Следствие: `curl_easy_getinfo(CURLINFO_RESPONSE_CODE)` проваливался, код ответа
всегда был 0, а `set_http_code()` превращал 0 в **200** — все проверки
`status == 200` видели всегда успешный ответ. Это же объясняет исторические баги
«Google вернул 200, а в теле ошибка» (их правили парсингом тела в `resp_has_error`).
Тесты (`tests/test_tracking_fill.c`) подменяют HTTP и возвращают настоящие
статусы — после починки продакшн совпал с тем, что проверяют тесты.

## Проверки (пройдены)

- `gcc -O2 -Wall -Wextra` (флаги Docker) и clang: 0 предупреждений;
- `make test-supabase` — офлайн-тест буфера (дедуп, экранирование, учёт ошибок flush);
- `tests/test_tracking_fill` — ALL TESTS PASSED;
- `tests/test_miniapp` (jsdom) — 55 проверок PASSED;
- живой прогон: 4 месяца → **8090 строк** собрано, все вызовы корректно
  отчитываются `HTTP 404` (таблиц ещё нет); Google-путь — `cache refreshed: 4/4 months`.

## Безопасность

- `.env` в `.gitignore` ✓ и добавлен в `.dockerignore` ✓ (раньше попадал в контекст сборки);
- `SUPABASE_SECRET_KEY` — только сервер, в HTML/JS Mini App не попадает;
- анонимный ключ (`SUPABASE_PUBLISHABLE_KEY`) закрыт RLS;
- приложение при падении БД продолжает работать на файловых кэшах.
