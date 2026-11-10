-- BO3 Recomp - Supabase schema for cloud saves (runtime/src/cloud.cpp).
-- Paste it in the project's SQL Editor and run it once.
--
-- One row per save file of the profile (content/<profile>/4156091D/...): path, base64 data and a
-- hash. Each row belongs to the Supabase user that wrote it (the account in bo3recomp.ini [online]);
-- row level security lets every user read / write only their own rows.

create table if not exists public.save_files (
    owner      uuid        not null default auth.uid() references auth.users (id) on delete cascade,
    path       text        not null,
    hash       text        not null,
    data       text        not null,
    updated_at timestamptz not null default now(),
    primary key (owner, path)
);

alter table public.save_files enable row level security;

drop policy if exists "own save files" on public.save_files;
create policy "own save files" on public.save_files
    for all
    using (owner = auth.uid())
    with check (owner = auth.uid());

-- updated_at follows every change (uploads are upserts)
create or replace function public.touch_updated_at() returns trigger
language plpgsql as $$
begin
    new.updated_at := now();
    return new;
end $$;

drop trigger if exists save_files_touch on public.save_files;
create trigger save_files_touch before update on public.save_files
    for each row execute function public.touch_updated_at();
