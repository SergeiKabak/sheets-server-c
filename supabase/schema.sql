-- ============================================================
-- Supabase: схема для графика смен (этап 1) и учёта времени (этап 3)
-- Выполнить ОДИН РАЗ в дашборде Supabase -> SQL Editor -> New query -> Run.
--
-- Данные сюда пишет сервер (SUPABASE_SECRET_KEY / service_role),
-- anon-ключ (SUPABASE_PUBLISHABLE_KEY) через RLS ничего не видит.
-- ============================================================

-- График: буфер из Google Sheets (все сотрудники — нужен для «с кем работаю»)
create table if not exists schedule (
  month      text   not null,                 -- '10.2026'
  day        int    not null check (day between 1 and 31),
  employee   text   not null,                 -- наследование имени пустой строки листа
  shift_time text   not null default '',      -- '7:30-14:40'
  cabinet    text   not null,                 -- 'K2.1'
  updated_at timestamptz not null default now(),
  primary key (month, day, employee, shift_time)
);

create index if not exists schedule_cab_idx on schedule (month, day, cabinet);
create index if not exists schedule_emp_idx on schedule (month, employee, day);

-- «Форма» месяца: ширина строки листа и счётчик «Всего смен».
-- Нужна на холодном старте (пустой /tmp) и при недоступном Google, чтобы
-- календарь строился из schedule без чтения листов.
create table if not exists month_meta (
  month        text primary key,              -- '10.2026'
  cols         int  not null default 34,      -- ширина строки: дни + 3
  total_shifts text not null default '',      -- значение счётчика из листа
  updated_at   timestamptz not null default now()
);

-- Журнал синхронизации: «пульс» каждые ~10 минут + ошибки
create table if not exists sync_log (
  id         bigint generated always as identity primary key,
  kind       text not null default '',
  month      text not null default '',
  row_count  int  not null default 0,
  ok         boolean not null default true,
  error      text not null default '',
  at         timestamptz not null default now()
);
create index if not exists sync_log_at_idx on sync_log (at desc);

-- Учёт времени (деньги) — источник истины с этапа 3 плана
create table if not exists time_entries (
  month      text not null,
  day        int  not null check (day between 1 and 31),
  t_in       text not null,
  t_out      text not null,
  hours      numeric,
  amount     numeric,
  source     text not null default 'app',     -- app | bot | import
  updated_at timestamptz not null default now(),
  primary key (month, day)
);

-- RLS: закрыть всё, кроме service_role (он обходит RLS).
-- Анонимный ключ из Mini App в коде не используется и доступа не получит.
alter table schedule     enable row level security;
alter table month_meta   enable row level security;
alter table sync_log     enable row level security;
alter table time_entries enable row level security;
