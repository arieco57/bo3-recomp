-- BO3 Recomp - online accounts (runtime/src/account.cpp). Run once in the SQL Editor, after
-- supabase_schema.sql.
--
-- Accounts are Supabase Auth users (email = the recovery address, password). Each one has a unique
-- username (case-insensitive), which is also the in-game name. Players sign in with the username:
-- resolve_login() returns the account's email only when the password matches, so emails stay private.

create table if not exists public.profiles (
    owner      uuid        primary key default auth.uid() references auth.users (id) on delete cascade,
    username   text        not null check (username ~ '^[A-Za-z0-9_]{3,15}$'),
    created_at timestamptz not null default now()
);
create unique index if not exists profiles_username_ci on public.profiles (lower(username));

alter table public.profiles enable row level security;

drop policy if exists "read own profile" on public.profiles;
create policy "read own profile" on public.profiles for select using (owner = auth.uid());
drop policy if exists "create own profile" on public.profiles;
create policy "create own profile" on public.profiles for insert with check (owner = auth.uid());

-- Is a username free? (callable before signing up)
create or replace function public.username_available(p_username text) returns boolean
language sql stable security definer set search_path = public as $$
    select not exists (select 1 from public.profiles where lower(username) = lower(p_username));
$$;

-- Username + password -> the account's email (null when the name or the password is wrong).
create or replace function public.resolve_login(p_username text, p_password text) returns text
language plpgsql stable security definer set search_path = public, extensions, auth as $$
declare
    v_email text;
    v_hash  text;
begin
    select u.email, u.encrypted_password into v_email, v_hash
    from public.profiles p join auth.users u on u.id = p.owner
    where lower(p.username) = lower(p_username);
    if v_hash is null or extensions.crypt(p_password, v_hash) <> v_hash then
        return null;
    end if;
    return v_email;
end $$;

revoke all on function public.resolve_login(text, text) from public;
grant execute on function public.resolve_login(text, text) to anon, authenticated;
grant execute on function public.username_available(text) to anon, authenticated;
