#ifndef OOPZ_WINDOW_FILTER_H
#define OOPZ_WINDOW_FILTER_H

#include <windows.h>

/* Only affects API addresses explicitly obtained by the caller's resolver. */
extern volatile LONG guard_window_filter_active;
FARPROC window_filter_resolve(HMODULE module, LPCSTR name, FARPROC original);

#endif
