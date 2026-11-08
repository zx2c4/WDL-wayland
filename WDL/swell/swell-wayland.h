#pragma once

#include "swell.h"
#include <gdk/gdk.h>

bool swell_wayland_needs_manual_titlebar(HWND hwnd);
void swell_wayland_ime_cursor_rect(HWND hwnd, GdkRectangle *rect);
void swell_wayland_clear_gl_context();

extern int g_swell_wayland_title_h;
extern int SWELL_WAYLAND_BORDER_WIDTH;
