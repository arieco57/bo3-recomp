-- BO3 Recomp - online play over the internet (runtime/src/rendezvous.cpp).
-- Paste it in the project's SQL Editor and run it once.
--
-- Every running game in online mode publishes the public address of its network socket (found with
-- STUN) every 10 s; the games read each other's rows to connect directly (UDP hole punching).
-- Signed-in players can read every row (they need the addresses), but only write their own.

create table if not exists public.online_peers (
    owner       uuid        not null default auth.uid() references auth.users (id) on delete cascade,
    vip         bigint      not null,          -- the game's virtual IP (one row per running game)
    gamertag    text        not null default '',
    public_ip   text        not null default '',
    public_port integer     not null default 0,
    local_ip    text        not null default '',
    local_port  integer     not null default 0,
    updated_at  timestamptz not null default now(),
    primary key (owner, vip)
);

create index if not exists online_peers_updated on public.online_peers (updated_at);

alter table public.online_peers enable row level security;

drop policy if exists "read online peers" on public.online_peers;
create policy "read online peers" on public.online_peers
    for select to authenticated using (true);

drop policy if exists "write own peer rows" on public.online_peers;
create policy "write own peer rows" on public.online_peers
    for all to authenticated
    using (owner = auth.uid())
    with check (owner = auth.uid());

-- updated_at follows every publish (they are upserts); touch_updated_at comes from supabase_schema.sql
create or replace function public.touch_updated_at() returns trigger
language plpgsql as $$
begin
    new.updated_at := now();
    return new;
end $$;

drop trigger if exists online_peers_touch on public.online_peers;
create trigger online_peers_touch before update on public.online_peers
    for each row execute function public.touch_updated_at();

-- rows of games that stopped more than a day ago are useless: deleted by every publish
create or replace function public.online_peers_cleanup() returns trigger
language plpgsql security definer as $$
begin
    delete from public.online_peers where updated_at < now() - interval '1 day';
    return new;
end $$;

drop trigger if exists online_peers_cleanup on public.online_peers;
create trigger online_peers_cleanup after insert on public.online_peers
    for each statement execute function public.online_peers_cleanup();
