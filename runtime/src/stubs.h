#pragma once
#include "guest.h"

// Called by the generated weak import stubs. Logs once per import (all calls with --trace),
// returns 0 in r3.
void ImportUnimplemented(PPCContext& ctx, const char* name);

// Logs an import call when tracing is enabled.
#define TRACE_IMPORT(fmt, ...) do { if (g_logImports) Log("%s(" fmt ")", __func__ + 7, ##__VA_ARGS__); } while (0)
