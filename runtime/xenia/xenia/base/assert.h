// Minimal shim for Xenia's xenia/base/assert.h so the vendored GPU headers compile.
#pragma once
#include <cassert>
#define assert_always(...) assert(false)
#define assert_true(x, ...) assert(x)
#define assert_false(x, ...) assert(!(x))
#define assert_zero(x, ...) assert((x) == 0)
#define assert_not_zero(x, ...) assert((x) != 0)
#define assert_null(x, ...) assert((x) == nullptr)
#define assert_not_null(x, ...) assert((x) != nullptr)
#define assert_unhandled_case(x) assert(false)
#define assert_size(t, s) static_assert(sizeof(t) == (s), "bad size: " #t)
#define static_assert_size(t, s) static_assert(sizeof(t) == (s), "bad size: " #t)
