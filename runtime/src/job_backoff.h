#pragma once
// The game's job queue workers (JQ Worker 0..2), hooked in mp/hooks.cpp and zm/hooks.cpp.
void JobBeforeSearch();
void JobAfterSearch(bool found);
void JobAfterPush(bool putBack);
