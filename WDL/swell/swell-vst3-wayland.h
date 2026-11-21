/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.


  */


#pragma once

#include "swell.h"

struct wl_display;
struct wl_proxy;
struct wl_surface;
struct wl_interface;
struct SWELL_VST3Parent;
struct SWELL_VST3Factory;

void *swell_vst3_factory(SWELL_VST3Factory **slot, void *entry);
void swell_vst3_free_factory(SWELL_VST3Factory *factory);
HWND swell_vst3_find_parent(void *xid);
void swell_vst3_unbind(HWND hwnd);
SWELL_VST3Parent *swell_vst3_parent_view(HWND hwnd);
SWELL_VST3Parent *swell_vst3_create_parent();
void swell_vst3_destroy_parent(SWELL_VST3Parent *parent);
void swell_vst3_position(SWELL_VST3Parent *parent, HWND hwnd);
int swell_vst3_scale();
void swell_vst3_default_cursor(HWND hwnd);
wl_display *swell_vst3_display();
wl_proxy *swell_vst3_parent(SWELL_VST3Parent *view, const wl_interface *type, RECT *rect);
wl_display *swell_wayland_client_open(wl_display *session);
bool swell_wayland_client_close(wl_display *client);
wl_proxy *swell_wayland_client_share(wl_display *client, wl_proxy *object, const wl_interface *type, HWND hwnd);
HWND swell_wayland_client_window(wl_surface *surface);
void swell_wayland_client_pointer_enter(wl_surface *surface, uint32_t seat, uint32_t serial);
void swell_wayland_client_forget(wl_proxy *object);
