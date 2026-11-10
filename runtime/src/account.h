// Online account (Supabase Auth + public.profiles, see tools/supabase_accounts.sql): unique
// username (= in-game name), password, recovery email. The first time, a dialog offers to create
// an account, sign in, recover the password or skip; afterwards only a refresh token is kept
// (account.txt in the data folder - never the password).
#pragma once
#include <string>

bool AccountInit(const std::string& url, const std::string& key);   // signed in (asks the first time)
std::string AccountToken();      // a valid access token, "" when signed out
std::string AccountUsername();   // "" when signed out
