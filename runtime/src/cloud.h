// Cloud saves (Supabase): the profile's save packages (content/<profile>/4156091D/...) are kept in
// a Supabase table, so PC and phone share the same progress. Configured in bo3recomp.ini [online]
// (supabase_url, supabase_key = the project's anon key, email, password); off when not set.
#pragma once

void CloudStartupSync();   // blocking, before the game reads its saves: download what changed elsewhere
void CloudFlush();         // upload local changes now (app closing / going to the background)
