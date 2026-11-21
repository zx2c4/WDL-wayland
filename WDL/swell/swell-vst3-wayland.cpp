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


#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/gui/iwaylandframe.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#define WDL_WCHAR Steinberg::char16
#include "../wdlutf8.h"
#undef WDL_WCHAR
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include <ffi.h>
#include <glib.h>
#include <glib-unix.h>
#include <poll.h>
#include "swell-vst3-wayland.h"
#include "swell-wayland.h"
#include "swell-internal.h"
#include "../ptrlist.h"

struct SWELL_VST3Factory
{
  ffi_cif cif;
  ffi_closure *closure;
  void *entry;
  Steinberg::IPluginFactory *(*original)();
  int id;
  bool native_host = false;
  Steinberg::IPtr<Steinberg::FUnknown> host;
};

namespace {
using namespace Steinberg;
using namespace Steinberg::Vst;

static bool equal(FIDString a, const TUID b)
{
  return a && !memcmp(a, b, sizeof(TUID));
}

template<class T> static IPtr<T> query(FUnknown *object, const TUID interface_id)
{
  T *ptr = NULL;
  if (object) object->queryInterface(interface_id, (void **)&ptr);
  return owned(ptr);
}

template<class T> static tresult give(T *ptr, void **out)
{
  *out = ptr;
  if (!ptr) return kNoInterface;
  ptr->addRef();
  return kResultOk;
}

template<class T> class RefCounted : public T
{
  gint refs = 1;
protected:
  virtual ~RefCounted() {}
public:
  uint32 addRef() override { return g_atomic_int_add(&refs, 1) + 1; }
  uint32 release() override
  {
    const uint32 count = g_atomic_int_add(&refs, -1) - 1;
    if (!count) delete this;
    return count;
  }
};

class Host : public RefCounted<IWaylandHost>
{
public:
  struct Owner
  {
    int module, instance;
  };
  struct Watch
  {
    IPtr<Linux::IEventHandler> handler;
    GSource *source;
    Watch *next;
    Watch(Linux::IEventHandler *h, int fd, Watch *tail)
        : handler(h), source(g_unix_fd_source_new(fd, G_IO_IN)), next(tail)
    {
      g_source_set_callback(source, G_SOURCE_FUNC(readable), h, NULL);
      g_source_attach(source, NULL);
    }
    ~Watch()
    {
      g_source_destroy(source);
      g_source_unref(source);
    }
    static gboolean readable(gint fd, GIOCondition condition, gpointer data)
    {
      struct pollfd pfd = {fd, POLLIN, 0};
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)))
      {
        IPtr<Linux::IEventHandler> handler((Linux::IEventHandler *)data);
        handler->onFDIsSet(fd);
      }
      return !(condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL));
    }
  };
  struct Client
  {
    wl_display *display;
    Owner owner;
    bool early;
    Watch *watches;
  };
  struct Scope
  {
    Owner owner;
    wl_display *offer = NULL;
    Scope *previous;
    explicit Scope(Owner o) : owner(o), previous(current) { current = this; }
    ~Scope() { current = previous; }
  };
  static thread_local Scope *current;
  WDL_TypedBuf<Client> clients;
  static Host &get()
  {
    static Host host;
    return host;
  }
  static int nextId()
  {
    static gint last;
    return g_atomic_int_add(&last, 1) + 1;
  }

  wl_display *connect(Owner owner, bool early)
  {
    wl_display *display = swell_wayland_client_open(swell_vst3_display());
    if (display && !clients.Add({display, owner, early, NULL}))
    {
      swell_wayland_client_close(display);
      return NULL;
    }
    return display;
  }
  tresult registerHandler(Linux::IRunLoop *loop, Linux::IEventHandler *handler, int fd)
  {
    if (!handler || fd < 0) return kInvalidArgument;
    for (Client &c : clients)
      if (wl_display_get_fd(c.display) == fd)
      {
        c.watches = new Watch(handler, fd, c.watches);
        return kResultOk;
      }
    return loop->registerEventHandler(handler, fd);
  }
  tresult unregisterHandler(Linux::IRunLoop *loop, Linux::IEventHandler *handler)
  {
    for (Client &c : clients)
      for (Watch **p = &c.watches; *p; p = &(*p)->next)
        if ((*p)->handler == handler)
        {
          Watch *w = *p;
          *p = w->next;
          delete w;
          return kResultOk;
        }
    return loop->unregisterEventHandler(handler);
  }
  wl_display *existing(Owner owner, bool &ambiguous)
  {
    wl_display *found = NULL;
    int best = 0;
    bool unscoped = false;
    ambiguous = false;
    for (const Client &c : clients)
    {
      if (!c.early) continue;
      if (!c.owner.module)
      {
        unscoped = true;
        continue;
      }
      if (c.owner.module != owner.module ||
          (c.owner.instance && c.owner.instance != owner.instance)) continue;
      const int rank = c.owner.instance ? 2 : 1;
      if (rank < best) continue;
      ambiguous = rank == best;
      best = rank;
      found = c.display;
    }
    if (!found && unscoped) ambiguous = true;
    return ambiguous ? NULL : found;
  }
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(interface_id, FUnknown_iid) || equal(interface_id, IWaylandHost_iid)) return give(this, out);
    *out = NULL;
    return kNoInterface;
  }
  wl_display *openWaylandConnection() override
  {
    if (current && current->offer)
    {
      wl_display *display = current->offer;
      current->offer = NULL;
      return display;
    }
    return connect(current ? current->owner : Owner{}, true);
  }
  tresult closeWaylandConnection(wl_display *display) override
  {
    for (int i = 0; i < clients.GetSize(); ++i)
      if (clients.Get()[i].display == display)
      {
        Watch *w = clients.Get()[i].watches;
        clients.Delete(i);
        while (w)
        {
          Watch *next = w->next;
          delete w;
          w = next;
        }
        return swell_wayland_client_close(display) ? kResultOk : kResultFalse;
      }
    return kInvalidArgument;
  }
  ~Host()
  {
    while (clients.GetSize()) closeWaylandConnection(clients.Get()[clients.GetSize()-1].display);
  }
};

thread_local Host::Scope *Host::current;

template<class T> class WithRunLoop : public RefCounted<T>, public Linux::IRunLoop
{
  IPtr<Linux::IRunLoop> loop;
protected:
  tresult queryHost(FUnknown *ctx, const TUID id, void **out)
  {
    if (!equal(id, Linux::IRunLoop_iid)) return ctx->queryInterface(id, out);
    if (!loop) loop = query<Linux::IRunLoop>(ctx, id);
    return give(loop ? (Linux::IRunLoop *)this : NULL, out);
  }
public:
  uint32 addRef() override { return RefCounted<T>::addRef(); }
  uint32 release() override { return RefCounted<T>::release(); }
  tresult registerEventHandler(Linux::IEventHandler *handler, int fd) override
  {
    return Host::get().registerHandler(loop, handler, fd);
  }
  tresult unregisterEventHandler(Linux::IEventHandler *handler) override
  {
    return Host::get().unregisterHandler(loop, handler);
  }
  tresult registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval interval) override
  {
    return loop->registerTimer(handler, interval);
  }
  tresult unregisterTimer(Linux::ITimerHandler *handler) override { return loop->unregisterTimer(handler); }
};

class Application : public WithRunLoop<IHostApplication>
{
  IPtr<FUnknown> original;
  IPtr<IHostApplication> app;
public:
  explicit Application(FUnknown *ctx) : original(ctx), app(query<IHostApplication>(ctx, IHostApplication_iid)) {}
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(interface_id, IHostApplication_iid) || equal(interface_id, FUnknown_iid))
      return give((IHostApplication *)this, out);
    if (equal(interface_id, IWaylandHost_iid)) return give(&Host::get(), out);
    return queryHost(original, interface_id, out);
  }
  tresult getName(String128 name) override
  {
    if (app) return app->getName(name);
    WDL_MBtoWideStr(name, g_swell_appname ? g_swell_appname : "SWELL", sizeof(String128));
    return kResultOk;
  }
  tresult createInstance(TUID cid, TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(cid, IWaylandHost_iid)) return Host::get().queryInterface(interface_id, out);
    *out = NULL;
    return app ? app->createInstance(cid, interface_id, out) : kNoInterface;
  }
};

static IPtr<FUnknown> context(FUnknown *ctx)
{
  return owned<IHostApplication>(ctx ? new Application(ctx) : NULL);
}

static bool hosts_wayland(FUnknown *ctx)
{
  if (query<IWaylandHost>(ctx, IWaylandHost_iid)) return true;
  IPtr<IHostApplication> app = query<IHostApplication>(ctx, IHostApplication_iid);
  if (!app) return false;
  TUID id;
  memcpy(id, IWaylandHost_iid, sizeof(id));
  IWaylandHost *host = NULL;
  app->createInstance(id, id, (void **)&host);
  if (!host) return false;
  host->release();
  return true;
}

static int32 scale_coord(int32 value, int from, int to)
{
  const int64_t n = (int64_t)value * to + (value < 0 ? -from/2 : from/2);
  return (int32)wdl_max((int64_t)INT32_MIN, wdl_min(n / from, (int64_t)INT32_MAX));
}

static ViewRect scale_rect(const ViewRect &r, int from, int to)
{
  return ViewRect(scale_coord(r.left, from, to), scale_coord(r.top, from, to),
      scale_coord(r.right, from, to), scale_coord(r.bottom, from, to));
}

class Frame : public WithRunLoop<IPlugFrame>, public IWaylandFrame
{
  IPtr<IPlugFrame> original;
  IPlugView *outer, *inner;
  HWND parent = NULL;
  wl_display *implicit = NULL;
public:
  bool native = true;
  int scale() const { return native ? swell_vst3_scale() : SWELL_WAYLAND_SCALE_ONE; }
  Frame(IPlugFrame *f, IPlugView *v, IPlugView *wrapped) : original(f), outer(v), inner(wrapped) {}
  uint32 addRef() override { return RefCounted::addRef(); }
  uint32 release() override { return RefCounted::release(); }
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(interface_id, FUnknown_iid) || equal(interface_id, IPlugFrame_iid)) return give((IPlugFrame *)this, out);
    if (equal(interface_id, IWaylandFrame_iid)) return give((IWaylandFrame *)this, out);
    if (equal(interface_id, IWaylandHost_iid)) return give(&Host::get(), out);
    return queryHost(original, interface_id, out);
  }
  tresult resizeView(IPlugView *view, ViewRect *size) override
  {
    if (!outer || view != inner || !size) return kInvalidArgument;
    ViewRect pixels = scale_rect(*size, SWELL_WAYLAND_SCALE_ONE, scale());
    return original->resizeView(outer, &pixels);
  }
  void setParent(HWND hwnd)
  {
    if (implicit) Host::get().closeWaylandConnection(implicit);
    implicit = NULL;
    if (hwnd) hwnd->Retain();
    if (parent)
    {
      swell_vst3_unbind(parent);
      parent->Release();
    }
    parent = hwnd;
  }
  void ownConnection(wl_display *display) { implicit = display; }
  void detach() { outer = NULL; setParent(NULL); }
  ~Frame() { setParent(NULL); }

  wl_proxy *share(wl_display *display, const wl_interface *type, RECT *r = NULL)
  {
    if (!parent || parent->m_hashaddestroy) return NULL;
    wl_proxy *object = swell_vst3_parent(swell_vst3_parent_view(parent), type, r);
    return swell_wayland_client_share(display, object, type, parent);
  }
  wl_surface *getWaylandSurface(wl_display *display) override
  {
    return (wl_surface *)share(display, &wl_surface_interface);
  }
  xdg_surface *getParentSurface(ViewRect &size, wl_display *display) override
  {
    RECT r = {};
    xdg_surface *surface = (xdg_surface *)share(display, &xdg_surface_interface, &r);
    if (surface) size = ViewRect(r.left, r.top, r.right, r.bottom);
    return surface;
  }
  xdg_toplevel *getParentToplevel(wl_display *display) override
  {
    return (xdg_toplevel *)share(display, &xdg_toplevel_interface);
  }
};

class View : public RefCounted<IPlugView>, public IPlugViewContentScaleSupport
{
  IPtr<IPlugView> original;
  IPtr<Frame> frame;
  IPtr<IPlugViewContentScaleSupport> scaling;
  Host::Owner owner;
  float requested_scale = 0;
  int scale() const { return frame ? frame->scale() : swell_vst3_scale(); }
public:
  uint32 addRef() override { return RefCounted::addRef(); }
  uint32 release() override { return RefCounted::release(); }
  View(IPlugView *v, Host::Owner o) : original(owned(v)),
      scaling(query<IPlugViewContentScaleSupport>(v, IPlugViewContentScaleSupport_iid)), owner(o) {}
  ~View() { if (frame) frame->detach(); }
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(interface_id, FUnknown_iid) || equal(interface_id, IPlugView_iid)) return give((IPlugView *)this, out);
    if (scaling && equal(interface_id, IPlugViewContentScaleSupport_iid))
      return give((IPlugViewContentScaleSupport *)this, out);
    return original->queryInterface(interface_id, out);
  }
  tresult setFrame(IPlugFrame *f) override
  {
    Host::Scope scope(owner);
    IPtr<Frame> next = owned(f ? new Frame(f, this, original) : NULL);
    const tresult result = original->setFrame(next);
    if (result == kResultOk || !f)
    {
      if (frame) frame->detach();
      frame = next;
    }
    else if (next) next->detach();
    return result;
  }
  tresult isPlatformTypeSupported(FIDString type) override
  {
    if (type && !strcmp(type, kPlatformTypeX11EmbedWindowID)) return kResultOk;
    return original->isPlatformTypeSupported(type);
  }
  tresult attached(void *parent, FIDString type) override
  {
    if (frame && type && !strcmp(type, kPlatformTypeX11EmbedWindowID))
    {
      HWND hwnd = swell_vst3_find_parent(parent);
      if (hwnd)
      {
        IPtr<Frame> f = frame;
        f->setParent(hwnd);
        f->native = true;
        Host::Scope scope(owner);
        bool ambiguous = false;
        wl_display *client = Host::get().existing(owner, ambiguous);
        if (!client && !ambiguous) client = scope.offer = Host::get().connect(owner, false);
        wl_surface *surface = client ? f->getWaylandSurface(client) : NULL;
        tresult result = surface ? original->attached(surface, kPlatformTypeWaylandSurfaceID) : kResultFalse;
        if (scope.offer) f->ownConnection(scope.offer);
        if (result == kResultOk) return result;
        f->setParent(NULL);
      }
      if (original->isPlatformTypeSupported(type) != kResultOk) return kResultFalse;
      if (frame) frame->native = false;
      if (scaling && requested_scale > 0) scaling->setContentScaleFactor(requested_scale);
    }
    return original->attached(parent, type);
  }
  tresult removed() override
  {
    const tresult result = original->removed();
    if (frame) frame->setParent(NULL);
    return result;
  }
  tresult onWheel(float distance) override { return original->onWheel(distance); }
  tresult onKeyDown(char16 ch, int16 key, int16 mods) override { return original->onKeyDown(ch, key, mods); }
  tresult onKeyUp(char16 ch, int16 key, int16 mods) override { return original->onKeyUp(ch, key, mods); }
  tresult setContentScaleFactor(ScaleFactor factor) override
  {
    requested_scale = factor;
    return scaling->setContentScaleFactor(factor * SWELL_WAYLAND_SCALE_ONE / scale());
  }
  tresult getSize(ViewRect *size) override
  {
    tresult result = original->getSize(size);
    if (result == kResultOk && size) *size = scale_rect(*size, SWELL_WAYLAND_SCALE_ONE, scale());
    return result;
  }
  tresult onSize(ViewRect *size) override
  {
    if (!size) return kInvalidArgument;
    ViewRect logical = scale_rect(*size, scale(), SWELL_WAYLAND_SCALE_ONE);
    return original->onSize(&logical);
  }
  tresult onFocus(TBool state) override { return original->onFocus(state); }
  tresult canResize() override { return original->canResize(); }
  tresult checkSizeConstraint(ViewRect *size) override
  {
    if (!size) return kInvalidArgument;
    const int s = scale();
    ViewRect logical = scale_rect(*size, s, SWELL_WAYLAND_SCALE_ONE);
    tresult result = original->checkSizeConstraint(&logical);
    if (result == kResultOk) *size = scale_rect(logical, SWELL_WAYLAND_SCALE_ONE, s);
    return result;
  }
};

class Instance : public RefCounted<IComponent>
{
  class Controller : public IEditController
  {
  public:
    Instance &owner;
    IPtr<IEditController> ctl;
    Controller(Instance &i, FUnknown *obj) : owner(i), ctl(query<IEditController>(obj, IEditController_iid)) {}
    uint32 addRef() override { return owner.addRef(); }
    uint32 release() override { return owner.release(); }
    tresult queryInterface(const TUID interface_id, void **out) override { return owner.queryInterface(interface_id, out); }
    tresult initialize(FUnknown *ctx) override
    {
      return owner.initialize_plugin(ctl, ctx, owner.controller_host);
    }
    tresult terminate() override { return ctl->terminate(); }
    tresult setComponentState(IBStream *s) override { return ctl->setComponentState(s); }
    tresult setState(IBStream *s) override { return ctl->setState(s); }
    tresult getState(IBStream *s) override { return ctl->getState(s); }
    int32 getParameterCount() override { return ctl->getParameterCount(); }
    tresult getParameterInfo(int32 i, ParameterInfo &info) override { return ctl->getParameterInfo(i, info); }
    tresult getParamStringByValue(ParamID id, ParamValue value, String128 s) override
    {
      return ctl->getParamStringByValue(id, value, s);
    }
    tresult getParamValueByString(ParamID id, TChar *s, ParamValue &value) override
    {
      return ctl->getParamValueByString(id, s, value);
    }
    ParamValue normalizedParamToPlain(ParamID id, ParamValue v) override { return ctl->normalizedParamToPlain(id, v); }
    ParamValue plainParamToNormalized(ParamID id, ParamValue v) override { return ctl->plainParamToNormalized(id, v); }
    ParamValue getParamNormalized(ParamID id) override { return ctl->getParamNormalized(id); }
    tresult setParamNormalized(ParamID id, ParamValue v) override { return ctl->setParamNormalized(id, v); }
    tresult setComponentHandler(IComponentHandler *h) override { return ctl->setComponentHandler(h); }
    IPlugView *createView(FIDString name) override
    {
      Host::Scope scope(owner.id);
      IPlugView *view = ctl->createView(name);
      if (view && !owner.native_host && view->isPlatformTypeSupported(kPlatformTypeWaylandSurfaceID) == kResultOk &&
          view->isPlatformTypeSupported("") != kResultOk)
        return new View(view, owner.id);
      return view;
    }
  };

  IPtr<FUnknown> component_host, controller_host;
  IPtr<FUnknown> original;
  IPtr<IComponent> component;
  Host::Owner id;
  bool native_host = false;
  Controller controller;

  tresult initialize_plugin(IPluginBase *plugin, FUnknown *ctx, IPtr<FUnknown> &host)
  {
    native_host = hosts_wayland(ctx);
    IPtr<FUnknown> next = context(native_host ? NULL : ctx);
    Host::Scope scope(id);
    const tresult result = plugin->initialize(native_host ? ctx : next.get());
    if (result == kResultOk) host = next;
    return result;
  }
public:
  Instance(FUnknown *obj, int module) : original(obj), component(query<IComponent>(obj, IComponent_iid)),
      id{module, Host::nextId()}, controller(*this, obj) {}
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (component && (equal(interface_id, FUnknown_iid) || equal(interface_id, IPluginBase_iid) || equal(interface_id, IComponent_iid)))
      return give((IComponent *)this, out);
    if (controller.ctl && (equal(interface_id, FUnknown_iid) || equal(interface_id, IPluginBase_iid) || equal(interface_id, IEditController_iid)))
      return give((IEditController *)&controller, out);
    return original->queryInterface(interface_id, out);
  }
  tresult initialize(FUnknown *ctx) override
  {
    return initialize_plugin(component, ctx, component_host);
  }
  tresult terminate() override { return component->terminate(); }
  tresult getControllerClassId(TUID cid) override { return component->getControllerClassId(cid); }
  tresult setIoMode(IoMode mode) override { return component->setIoMode(mode); }
  int32 getBusCount(MediaType type, BusDirection dir) override { return component->getBusCount(type, dir); }
  tresult getBusInfo(MediaType type, BusDirection dir, int32 i, BusInfo &info) override
  {
    return component->getBusInfo(type, dir, i, info);
  }
  tresult getRoutingInfo(RoutingInfo &in, RoutingInfo &out) override { return component->getRoutingInfo(in, out); }
  tresult activateBus(MediaType type, BusDirection dir, int32 i, TBool state) override
  {
    return component->activateBus(type, dir, i, state);
  }
  tresult setActive(TBool state) override { return component->setActive(state); }
  tresult setState(IBStream *s) override { return component->setState(s); }
  tresult getState(IBStream *s) override { return component->getState(s); }
};

class Factory : public RefCounted<IPluginFactory3>
{
  IPtr<IPluginFactory> factory;
  IPtr<IPluginFactory2> factory2;
  IPtr<IPluginFactory3> factory3;
  SWELL_VST3Factory &module;
public:
  Factory(IPluginFactory *f, SWELL_VST3Factory &m)
    : factory(owned(f)), factory2(query<IPluginFactory2>(f, IPluginFactory2_iid)),
      factory3(query<IPluginFactory3>(f, IPluginFactory3_iid)), module(m) {}
  tresult queryInterface(const TUID interface_id, void **out) override
  {
    if (!out) return kInvalidArgument;
    if (equal(interface_id, FUnknown_iid) || equal(interface_id, IPluginFactory_iid) ||
        (factory2 && equal(interface_id, IPluginFactory2_iid)) || (factory3 && equal(interface_id, IPluginFactory3_iid)))
      return give(this, out);
    return factory->queryInterface(interface_id, out);
  }
  tresult getFactoryInfo(PFactoryInfo *p) override { return factory->getFactoryInfo(p); }
  int32 countClasses() override { return factory->countClasses(); }
  tresult getClassInfo(int32 i, PClassInfo *p) override { return factory->getClassInfo(i, p); }
  tresult getClassInfo2(int32 i, PClassInfo2 *p) override { return factory2->getClassInfo2(i, p); }
  tresult getClassInfoUnicode(int32 i, PClassInfoW *p) override { return factory3->getClassInfoUnicode(i, p); }
  tresult setHostContext(FUnknown *ctx) override
  {
    module.native_host = hosts_wayland(ctx);
    IPtr<FUnknown> next = context(module.native_host ? NULL : ctx);
    Host::Scope scope({module.id, 0});
    const tresult result = factory3->setHostContext(module.native_host ? ctx : next.get());
    if (result == kResultOk) module.host = next;
    return result;
  }
  tresult createInstance(FIDString cid, FIDString interface_id, void **out) override
  {
    tresult result = factory->createInstance(cid, interface_id, out);
    if (!module.native_host && result == kResultOk && out && *out &&
        (equal(interface_id, IComponent_iid) || equal(interface_id, IEditController_iid) ||
         equal(interface_id, FUnknown_iid) || equal(interface_id, IPluginBase_iid)))
    {
      IPtr<FUnknown> obj = owned((FUnknown *)*out);
      IPtr<Instance> instance = owned(new Instance(obj, module.id));
      result = instance->queryInterface(interface_id, out);
    }
    return result;
  }
};
}

static void get_factory(ffi_cif *, void *result, void **, void *data)
{
  SWELL_VST3Factory *module = (SWELL_VST3Factory *)data;
  Steinberg::IPluginFactory *factory = module->original();
  *(Steinberg::IPluginFactory **)result = factory ? new Factory(factory, *module) : NULL;
}

void *swell_vst3_factory(SWELL_VST3Factory **slot, void *entry)
{
  if (*slot) return (*slot)->entry;
  SWELL_VST3Factory *f = new SWELL_VST3Factory;
  f->id = Host::nextId();
  f->original = (Steinberg::IPluginFactory *(*)())entry;
  f->closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &f->entry);
  if (f->closure && ffi_prep_cif(&f->cif, FFI_DEFAULT_ABI, 0, &ffi_type_pointer, NULL) == FFI_OK &&
      ffi_prep_closure_loc(f->closure, &f->cif, get_factory, f, f->entry) == FFI_OK)
  {
    *slot = f;
    return f->entry;
  }
  swell_vst3_free_factory(f);
  return entry;
}

void swell_vst3_free_factory(SWELL_VST3Factory *factory)
{
  if (!factory) return;
  if (factory->closure) ffi_closure_free(factory->closure);
  delete factory;
}
