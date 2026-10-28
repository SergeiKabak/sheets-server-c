-- ============================================================
-- Этап 5: структура месяцев в БД (month_meta)
--
-- Зачем: календарю Mini App нужны не только строки графика, но и «форма»
-- листа — ширина строки (дни + 3 служебные колонки) и значение счётчика
-- «Всего смен». Раньше эти два числа жили только в /tmp/months.dat, поэтому
-- после рестарта контейнера (пустой /tmp) и при недоступном Google список
-- месяцев был пустым, хотя все строки лежали в schedule.
--
-- Теперь фоновая синхронизация кладёт структуру сюда, и /api/months и
-- /api/month строятся из БД без обращения к Google.
--
-- Выполнить один раз: дашборд Supabase -> SQL Editor -> New query -> Run.
-- ============================================================

create table if not exists public.month_meta (
    month        text primary key,              -- '10.2026'
    cols         int  not null default 34,      -- ширина строки листа: дни + 3
    total_shifts text not null default '',      -- счётчик «Всего смен» из листа
    updated_at   timestamptz not null default now()
);

-- Пишет только сервер (service_role обходит RLS), anon-ключ не видит ничего.
alter table public.month_meta enable row level security;

-- Проверка: таблица создана и пуста (заполнится в течение 5 минут
-- после деплоя — при первом обходе листов Google).
--   select * from public.month_meta order by month;
