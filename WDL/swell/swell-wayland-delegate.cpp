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


#include <wayland-client.h>
#include <wayland-server.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <glib.h>
#include "swell-vst3-wayland.h"
#include "../assocarray.h"
#include "../ptrlist.h"
#include "../mutex.h"

namespace {

struct Connection;
struct Global
{
  Connection *connection;
  const wl_interface *type;
  uint32_t name;
  wl_global *global = NULL;
  wl_proxy *parent = NULL, *borrowed = NULL;
  HWND hwnd = NULL;
  uint32_t pointer_serial = 0;
  bool cursor_set = false;
};

struct Object
{
  Connection *connection;
  const wl_interface *type;
  wl_proxy *proxy;
  wl_resource *resource;
  bool borrowed, focused = false, pointer_frame_pending = false;
  Global *global = NULL;
  Object *child = NULL, *parent = NULL;
  WDL_TypedBuf<int32_t> touches;
  Object(Connection *c, const wl_interface *t, wl_proxy *p, wl_resource *r, bool b) :
      connection(c), type(t), proxy(p), resource(r), borrowed(b) {}
};

struct Connection
{
  wl_display *upstream = NULL, *server = NULL, *client = NULL;
  wl_client *peer = NULL;
  wl_registry *registry = NULL, *client_registry = NULL;
  wl_event_queue *queue = NULL;
  int stop = -1;
  GThread *thread = NULL;
  WDL_Mutex mutex;
  WDL_PtrList_DeleteOnDestroy<Global> globals;
  WDL_PtrList<Object> objects;
  WDL_PointerKeyedArray<wl_proxy *, Object *> by_proxy;
  wl_protocol_logger *barrier = NULL;
  struct PeerListener : wl_listener { Connection *connection; } gone = {};

  void clear_objects()
  {
    while (objects.GetSize()) wl_resource_destroy(objects.GetLast()->resource);
  }

  bool start(wl_display *session);
  void run();
  ~Connection();
  Object *add(wl_proxy *proxy, const wl_interface *type, uint32_t id, uint32_t version, bool borrowed);
  wl_proxy *share(wl_proxy *parent, const wl_interface *type, HWND hwnd);
  void forget(wl_proxy *parent);
};

static WDL_PtrList<Connection> clients;

static int destructor_opcode(const wl_interface *type, int version)
{
  for (int i = 0; i < type->method_count; ++i)
  {
    const wl_message &m = type->methods[i];
    if ((!strcmp(m.name, "destroy") || !strcmp(m.name, "release")) && atoi(m.signature) <= version)
      return i;
  }
  return -1;
}

static HWND frame_window(Object *o)
{
  for (int depth = 0; o && depth < o->connection->objects.GetSize(); ++depth)
  {
    if (o->borrowed) return o->proxy ? o->global->hwnd : NULL;
    Object *parent = NULL;
    for (Object *role : o->connection->objects)
      if (role->child == o)
      {
        parent = role->parent;
        break;
      }
    o = parent;
  }
  return NULL;
}

static void dispose_object(wl_resource *resource)
{
  Object *o = (Object *)wl_resource_get_user_data(resource);
  o->connection->objects.DeletePtr(o);
  if (o->type == &wl_surface_interface)
    for (Object *role : o->connection->objects)
    {
      if (role->parent == o) role->parent = NULL;
      if (role->child == o) role->child = NULL;
    }
  if (o->proxy && !o->borrowed)
  {
    o->connection->by_proxy.Delete(o->proxy);
    int opcode = destructor_opcode(o->type, wl_proxy_get_version(o->proxy));
    if (opcode >= 0)
      wl_proxy_marshal_flags(o->proxy, opcode, NULL, wl_proxy_get_version(o->proxy), WL_MARSHAL_FLAG_DESTROY);
    else
      wl_proxy_destroy(o->proxy);
  }
  delete o;
}

static int dispatch(const void *data, void *target, uint32_t opcode, const wl_message *m, wl_argument *in)
{
  Object *o = (Object *)data;
  Connection *c = o->connection;
  const bool request = target == o->resource;
  const char *name = m->name;
  if (request && o->borrowed)
  {
    if (!strcmp(name, "destroy"))
    {
      Global *g = o->global;
      wl_resource_destroy(o->resource);
      c->globals.DeletePtr(g, true);
    }
    else wl_resource_post_error(o->resource, WL_DISPLAY_ERROR_INVALID_METHOD, "the plug-in frame is read-only");
    return 0;
  }

  wl_argument args[20];
  int count = 0, new_id = -1;
  bool deliver = true;
  for (const char *s = m->signature; *s; ++s)
  {
    if ((*s >= '0' && *s <= '9') || *s == '?') continue;
    if (count == (int)(sizeof(args)/sizeof(args[0]))) return -1;
    const int i = count++;
    args[i] = in[i];
    if (*s == 'n') new_id = i;
    else if (*s == 'o')
    {
      if (!in[i].o)
      {
        if (s == m->signature || s[-1] != '?') deliver = false;
      }
      else if (request)
      {
        Object *other = (Object *)wl_resource_get_user_data((wl_resource *)in[i].o);
        if (!other || !other->proxy) deliver = false;
        args[i].o = (wl_object *)(other ? other->proxy : NULL);
      }
      else
      {
        Object *other = c->by_proxy.Get((wl_proxy *)in[i].o);
        if (!other) deliver = false;
        args[i].o = (wl_object *)(other ? other->resource : NULL);
      }
    }
  }

  if (!request && (o->type == &wl_pointer_interface || o->type == &wl_keyboard_interface))
  {
    if (!strcmp(name, "enter"))
    {
      o->focused = deliver;
      if (o->type == &wl_pointer_interface && o->global->pointer_serial != args[0].u)
      {
        o->global->pointer_serial = args[0].u;
        o->global->cursor_set = false;
      }
    }
    else if (!strcmp(name, "leave"))
    {
      deliver = deliver && o->focused;
      o->focused = false;
    }
    else if (!o->focused && strcmp(name, "keymap") && strcmp(name, "repeat_info") && strcmp(name, "frame"))
      deliver = false;
  }
  if (!request && o->type == &wl_pointer_interface &&
      wl_resource_get_version(o->resource) >= WL_POINTER_FRAME_SINCE_VERSION)
  {
    if (opcode == WL_POINTER_FRAME)
    {
      deliver = o->pointer_frame_pending;
      o->pointer_frame_pending = false;
    }
    else if (deliver)
    {
      if (opcode == WL_POINTER_ENTER && o->pointer_frame_pending) wl_pointer_send_frame(o->resource);
      o->pointer_frame_pending = true;
    }
  }
  if (!request && o->type == &wl_touch_interface)
  {
    if (!strcmp(name, "down"))
    {
      if (deliver) o->touches.Add(args[3].i);
    }
    else if (!strcmp(name, "up"))
    {
      int i = o->touches.Find(args[2].i);
      deliver = i >= 0;
      if (deliver) o->touches.Delete(i);
    }
    else if (!strcmp(name, "motion")) deliver = o->touches.Find(args[1].i) >= 0;
    else if (!strcmp(name, "shape") || !strcmp(name, "orientation")) deliver = o->touches.Find(args[0].i) >= 0;
    else if (!strcmp(name, "cancel"))
    {
      deliver = o->touches.GetSize() > 0;
      o->touches.Resize(0, false);
    }
  }

  const wl_interface *child = new_id < 0 ? NULL : m->types[new_id];
  if (request)
  {
    const bool destroy = !strcmp(name, "destroy") || !strcmp(name, "release");
    if (deliver && o->proxy)
    {
      if (((o->type == &wl_pointer_interface && opcode == WL_POINTER_SET_CURSOR) ||
           (o->type == &wp_cursor_shape_device_v1_interface && opcode == WP_CURSOR_SHAPE_DEVICE_V1_SET_SHAPE)) &&
          o->global && o->global->pointer_serial == args[0].u)
        o->global->cursor_set = true;
      const uint32_t version = wl_proxy_get_version(o->proxy);
      const uint32_t id = child ? args[new_id].n : 0;
      if (child) args[new_id].o = NULL;
      wl_proxy *proxy = wl_proxy_marshal_array_flags(o->proxy, opcode, child, version,
          destroy ? WL_MARSHAL_FLAG_DESTROY : 0, args);
      if (child)
      {
        Object *obj = proxy ? c->add(proxy, child, id, version, false) : NULL;
        if (!obj) wl_client_post_no_memory(c->peer);
        else if (child == &wl_pointer_interface) obj->global = o->global;
        else if (child == &wp_cursor_shape_device_v1_interface &&
                 opcode == WP_CURSOR_SHAPE_MANAGER_V1_GET_POINTER)
          obj->global = ((Object *)wl_resource_get_user_data((wl_resource *)in[1].o))->global;
        else if (child == &wl_subsurface_interface)
        {
          obj->child = (Object *)wl_resource_get_user_data((wl_resource *)in[1].o);
          obj->parent = (Object *)wl_resource_get_user_data((wl_resource *)in[2].o);
        }
      }
    }
    else if (child && !c->add(NULL, child, args[new_id].n, wl_resource_get_version(o->resource), false))
      wl_client_post_no_memory(c->peer);
    if (destroy)
    {
      if (o->proxy) c->by_proxy.Delete(o->proxy);
      o->proxy = NULL;
      wl_resource_destroy(o->resource);
    }
  }
  else if (deliver)
  {
    if (child)
    {
      wl_proxy *proxy = (wl_proxy *)in[new_id].o;
      Object *obj = c->add(proxy, child, 0, wl_proxy_get_version(proxy), false);
      if (!obj) wl_client_post_no_memory(c->peer);
      else args[new_id].o = (wl_object *)obj->resource;
      deliver = obj != NULL;
    }
    if (deliver)
    {
      wl_resource_post_event_array(o->resource, opcode, args);
      if (o->type == &wl_callback_interface) wl_resource_destroy(o->resource);
    }
  }
  for (const char *s = m->signature; *s; ++s)
    if ((*s < '0' || *s > '9') && *s != '?')
    {
      if (*s == 'h') close(in->h);
      ++in;
    }
  return 0;
}

Object *Connection::add(wl_proxy *proxy, const wl_interface *type, uint32_t id, uint32_t version, bool borrowed)
{
  wl_resource *resource = wl_resource_create(peer, type, version, id);
  if (!resource) return NULL;
  Object *o = new Object{this, type, proxy, resource, borrowed};
  objects.Add(o);
  wl_resource_set_dispatcher(resource, dispatch, o, o, dispose_object);
  if (proxy && !borrowed)
  {
    by_proxy.Insert(proxy, o);
    wl_proxy_add_dispatcher(proxy, dispatch, o, o);
  }
  return o;
}

static void bind_global(wl_client *client, void *data, uint32_t version, uint32_t id)
{
  Global *g = (Global *)data;
  Connection *c = g->connection;
  const bool borrowed = !g->name;
  wl_proxy *proxy = borrowed ? g->parent : (wl_proxy *)wl_registry_bind(c->registry, g->name, g->type, version);
  Object *o = c->add(proxy, g->type, id, version, borrowed);
  if (!o)
  {
    wl_client_post_no_memory(client);
    return;
  }
  o->global = g;
  if (borrowed)
  {
    wl_global_destroy(g->global);
    g->global = NULL;
  }
}

static void registry_add(void *data, wl_registry *, uint32_t name, const char *iface, uint32_t version)
{
  static const wl_interface *const interfaces[] = {
    &wl_compositor_interface, &wl_subcompositor_interface, &wl_shm_interface, &wl_seat_interface,
    &wl_output_interface, &xdg_wm_base_interface, &zwp_linux_dmabuf_v1_interface, &wp_viewporter_interface,
    &wp_fractional_scale_manager_v1_interface, &wp_cursor_shape_manager_v1_interface,
    &wp_linux_drm_syncobj_manager_v1_interface,
  };
  Connection *c = (Connection *)data;
  for (const wl_interface *type : interfaces)
    if (!strcmp(iface, type->name))
    {
      Global *g = c->globals.Add(new Global{c, type, name});
      g->global = wl_global_create(c->server, type, wdl_min(version, (uint32_t)type->version), g, bind_global);
      break;
    }
}

static void registry_remove(void *data, wl_registry *, uint32_t name)
{
  Connection *c = (Connection *)data;
  for (Global *g : c->globals)
    if (g->name == name && g->global)
    {
      wl_global_destroy(g->global);
      g->global = NULL;
      break;
    }
}

static void sync_display(void *data, wl_protocol_logger_type direction, const wl_protocol_logger_message *msg)
{
  if (direction != WL_PROTOCOL_LOGGER_REQUEST || wl_resource_get_id(msg->resource) != 1 ||
      msg->message_opcode != WL_DISPLAY_SYNC) return;
  Connection *c = (Connection *)data;
  if (wl_display_roundtrip_queue(c->upstream, c->queue) < 0)
    wl_client_post_implementation_error(c->peer, "Wayland compositor disconnected");
}

static void peer_destroyed(wl_listener *listener, void *)
{
  Connection *c = static_cast<Connection::PeerListener *>(listener)->connection;
  c->clear_objects();
  c->peer = NULL;
}

static gpointer server_thread(gpointer data)
{
  ((Connection *)data)->run();
  return NULL;
}

bool Connection::start(wl_display *session)
{
  upstream = session;
  if (!upstream || !(server = wl_display_create()) || !(queue = wl_display_create_queue(upstream))) return false;
  wl_display *wrapper = (wl_display *)wl_proxy_create_wrapper(upstream);
  if (!wrapper) return false;
  wl_proxy_set_queue((wl_proxy *)wrapper, queue);
  registry = wl_display_get_registry(wrapper);
  wl_proxy_wrapper_destroy(wrapper);
  static const wl_registry_listener listener = {registry_add, registry_remove};
  if (!registry || wl_registry_add_listener(registry, &listener, this) < 0 ||
      wl_display_roundtrip_queue(upstream, queue) < 0) return false;

  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) return false;
  peer = wl_client_create(server, fds[0]);
  if (!peer)
  {
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  gone.notify = peer_destroyed;
  gone.connection = this;
  wl_client_add_destroy_listener(peer, &gone);
  barrier = wl_display_add_protocol_logger(server, sync_display, this);
  if (!barrier)
  {
    close(fds[1]);
    return false;
  }
  client = wl_display_connect_to_fd(fds[1]);
  if (!client || !(client_registry = wl_display_get_registry(client)) ||
      (stop = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) < 0) return false;
  thread = g_thread_try_new("vst3-wayland", server_thread, this, NULL);
  return thread != NULL;
}

void Connection::run()
{
  wl_event_loop *loop = wl_display_get_event_loop(server);
  pollfd fds[] = {{wl_event_loop_get_fd(loop), POLLIN, 0}, {wl_display_get_fd(upstream), POLLIN, 0},
      {stop, POLLIN, 0}};
  for (;;)
  {
    {
      WDL_MutexLock guard(&mutex);
      if (wl_display_dispatch_queue_pending(upstream, queue) < 0) break;
      wl_display_flush_clients(server);
    }
    if (wl_display_prepare_read_queue(upstream, queue) < 0) continue;
    fds[1].events = POLLIN;
    if (wl_display_flush(upstream) < 0 && errno == EAGAIN) fds[1].events |= POLLOUT;
    int n;
    do n = poll(fds, 3, -1); while (n < 0 && errno == EINTR);
    if (n > 0 && (fds[1].revents & POLLIN) && !fds[2].revents)
    {
      if (wl_display_read_events(upstream) < 0) break;
    }
    else wl_display_cancel_read(upstream);
    if (n < 0 || fds[2].revents || (fds[1].revents & (POLLERR | POLLHUP))) break;
    if (fds[0].revents)
    {
      WDL_MutexLock guard(&mutex);
      if (wl_event_loop_dispatch(loop, 0) < 0 || !peer) break;
    }
  }
  WDL_MutexLock guard(&mutex);
  if (peer) shutdown(wl_client_get_fd(peer), SHUT_RDWR);
}

wl_proxy *Connection::share(wl_proxy *parent, const wl_interface *type, HWND hwnd)
{
  WDL_MutexLock guard(&mutex);
  if (!peer) return NULL;
  for (Global *g : globals)
    if (!g->name && g->parent == parent) return g->borrowed;
  Global *g = globals.Add(new Global{this, type, 0});
  g->parent = parent;
  g->hwnd = hwnd;
  uint32_t version = wl_proxy_get_version(parent);
  g->global = wl_global_create(server, type, version, g, bind_global);
  if (g->global)
    g->borrowed = (wl_proxy *)wl_registry_bind(client_registry, wl_global_get_name(g->global, peer), type, version);
  if (g->borrowed) return g->borrowed;
  if (g->global) wl_global_destroy(g->global);
  globals.DeletePtr(g, true);
  return NULL;
}

void Connection::forget(wl_proxy *parent)
{
  WDL_MutexLock guard(&mutex);
  for (Object *o : objects)
    if (o->borrowed && o->proxy == parent) o->proxy = NULL;
  for (Global *g : globals)
    if (!g->name && g->parent == parent)
    {
      g->parent = NULL;
      if (g->borrowed)
      {
        const uint32_t version = wl_proxy_get_version(g->borrowed);
        wl_proxy_marshal_flags(g->borrowed, destructor_opcode(g->type, version), NULL, version, WL_MARSHAL_FLAG_DESTROY);
        g->borrowed = NULL;
        wl_display_flush(client);
      }
    }
}

Connection::~Connection()
{
  if (thread)
  {
    uint64_t one = 1;
    while (write(stop, &one, sizeof(one)) < 0 && errno == EINTR) {}
    g_thread_join(thread);
  }
  if (stop >= 0) close(stop);
  for (Global *g : globals)
    if (g->borrowed) wl_proxy_destroy(g->borrowed);
  if (client_registry) wl_registry_destroy(client_registry);
  if (client) wl_display_disconnect(client);
  clear_objects();
  if (barrier) wl_protocol_logger_destroy(barrier);
  if (server)
  {
    wl_display_destroy_clients(server);
    wl_display_destroy(server);
  }
  if (registry) wl_registry_destroy(registry);
  if (queue) wl_event_queue_destroy(queue);
  if (upstream) wl_display_flush(upstream);
}

}

wl_display *swell_wayland_client_open(wl_display *session)
{
  Connection *c = new Connection;
  if (c->start(session))
  {
    clients.Add(c);
    return c->client;
  }
  delete c;
  return NULL;
}

bool swell_wayland_client_close(wl_display *client)
{
  for (Connection *c : clients)
    if (c->client == client)
    {
      clients.DeletePtr(c, true);
      return true;
    }
  return false;
}

wl_proxy *swell_wayland_client_share(wl_display *client, wl_proxy *object, const wl_interface *type, HWND hwnd)
{
  if (object)
    for (Connection *c : clients)
      if (c->client == client) return c->share(object, type, hwnd);
  return NULL;
}

HWND swell_wayland_client_window(wl_surface *surface)
{
  for (Connection *c : clients)
  {
    WDL_MutexLock guard(&c->mutex);
    if (Object *o = c->by_proxy.Get((wl_proxy *)surface)) return frame_window(o);
  }
  return NULL;
}

void swell_wayland_client_pointer_enter(wl_surface *surface, uint32_t seat, uint32_t serial)
{
  for (Connection *c : clients)
  {
    WDL_MutexLock guard(&c->mutex);
    if (Object *o = c->by_proxy.Get((wl_proxy *)surface))
    {
      for (Global *g : c->globals)
        if (g->name == seat && g->cursor_set && g->pointer_serial == serial) return;
      swell_vst3_default_cursor(frame_window(o));
      return;
    }
  }
}

void swell_wayland_client_forget(wl_proxy *object)
{
  if (object)
    for (Connection *c : clients) c->forget(object);
}
