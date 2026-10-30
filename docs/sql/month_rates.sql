-- ============================================================
-- Ставка за час на каждый месяц (month_rates)
--
-- Зачем: до этого ставка была одна на всё (HOURLY_RATE), а она меняется
-- от месяца к месяцу. Теперь в Mini App в блоке «Учёт рабочего времени»
-- можно задать ставку для выбранного месяца; при изменении суммы всех
-- отмеченных дней месяца пересчитываются, а колонка E в Google-листе
-- обновляется фоновой выгрузкой.
--
-- Если для месяца строки нет — действует HOURLY_RATE из окружения
-- (по умолчанию 500), то есть старые месяцы ничего не теряют.
--
-- Выполнить один раз: дашборд Supabase -> SQL Editor -> New query -> Run.
-- ============================================================

create table if not exists public.month_rates (
    month      text primary key,                -- '10.2026'
    rate       numeric not null check (rate > 0),
    updated_at timestamptz not null default now()
);

-- Пишет только сервер (service_role обходит RLS), anon-ключ не видит ничего.
alter table public.month_rates enable row level security;

-- Проверка после первой правки ставки в Mini App:
--   select * from public.month_rates order by month;
