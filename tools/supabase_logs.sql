-- BO3 Recomp - error reports from the Android app (android/src/com/bo3recomp/LogUpload.java).
-- Paste it in the project's SQL Editor and run it once.
--
-- When the game fails (it never showed a picture, or it crashed) and the player agreed, the app sends
-- the end of the game's log and of the app's logcat at the next start. Anyone with the app's public
-- key can ADD a report; nobody can read them through the API (no select policy): read them in the
-- dashboard (Table Editor > error_reports). Reports older than 60 days are deleted.

create table if not exists public.error_reports (
    id          bigint      generated always as identity primary key,
    created_at  timestamptz not null default now(),
    app_version text        not null default '',
    mode        text        not null default '',   -- mp / zm
    reason      text        not null default '',   -- "no picture" / the crash
    device      text        not null default '',   -- manufacturer + model
    soc         text        not null default '',
    android     text        not null default '',
    driver      text        not null default '',   -- auto / turnip / stock + what the last run used
    game_log    text        not null default '',
    logcat      text        not null default ''
);

create index if not exists error_reports_created on public.error_reports (created_at);

alter table public.error_reports enable row level security;

drop policy if exists "send error reports" on public.error_reports;
create policy "send error reports" on public.error_reports
    for insert to anon, authenticated
    with check (length(game_log) <= 400000 and length(logcat) <= 400000
                and length(reason) <= 1000 and length(device) <= 200);

-- old reports go away by themselves
create or replace function public.error_reports_cleanup() returns trigger
language plpgsql security definer as $$
begin
    delete from public.error_reports where created_at < now() - interval '60 days';
    return new;
end $$;

drop trigger if exists error_reports_cleanup on public.error_reports;
create trigger error_reports_cleanup after insert on public.error_reports
    for each statement execute function public.error_reports_cleanup();
