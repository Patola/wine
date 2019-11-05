/*
 * COM+ Services
 *
 * Copyright 2013 Alistair Leslie-Hughes
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "ole2.h"
#include "rpcproxy.h"
#include "comsvcs.h"
#include "wine/heap.h"
#include "wine/debug.h"
#include "initguid.h"
#include "comsvcs_classes.h"

WINE_DEFAULT_DEBUG_CHANNEL(comsvcs);

static HINSTANCE COMSVCS_hInstance;

typedef struct dispensermanager
{
    IDispenserManager IDispenserManager_iface;
    LONG ref;
    HANDLE mta_thread, mta_stop_event;
} dispensermanager;

typedef struct holder
{
    IHolder IHolder_iface;
    LONG ref;

    IDispenserDriver *driver;
} holder;

struct new_moniker
{
    IMoniker IMoniker_iface;
    LONG refcount;
    CLSID clsid;
};

static HRESULT new_moniker_parse_displayname(IBindCtx *pbc, LPOLESTR name, ULONG *eaten, IMoniker **ret);

static inline dispensermanager *impl_from_IDispenserManager(IDispenserManager *iface)
{
    return CONTAINING_RECORD(iface, dispensermanager, IDispenserManager_iface);
}

static inline holder *impl_from_IHolder(IHolder *iface)
{
    return CONTAINING_RECORD(iface, holder, IHolder_iface);
}

static struct new_moniker *impl_from_IMoniker(IMoniker *iface)
{
    return CONTAINING_RECORD(iface, struct new_moniker, IMoniker_iface);
}

static HRESULT WINAPI holder_QueryInterface(IHolder *iface, REFIID riid, void **object)
{
    holder *This = impl_from_IHolder(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), object);

    *object = NULL;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IHolder))
    {
        *object = &This->IHolder_iface;
        IUnknown_AddRef( (IUnknown*)*object);

        return S_OK;
    }

    WARN("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),object);
    return E_NOINTERFACE;
}

static ULONG WINAPI holder_AddRef(IHolder *iface)
{
    holder *This = impl_from_IHolder(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI holder_Release(IHolder *iface)
{
    holder *This = impl_from_IHolder(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref)
    {
        heap_free(This);
    }

    return ref;
}

static HRESULT WINAPI holder_AllocResource(IHolder *iface, const RESTYPID typeid, RESID *resid)
{
    holder *This = impl_from_IHolder(iface);
    HRESULT hr;
    TIMEINSECS secs;

    TRACE("(%p)->(%08lx, %p) stub\n", This, typeid, resid);

    hr = IDispenserDriver_CreateResource(This->driver, typeid, resid, &secs);

    TRACE("<- 0x%08x\n", hr);
    return hr;
}

static HRESULT WINAPI holder_FreeResource(IHolder *iface, const RESID resid)
{
    holder *This = impl_from_IHolder(iface);
    HRESULT hr;

    TRACE("(%p)->(%08lx) stub\n", This, resid);

    hr = IDispenserDriver_DestroyResource(This->driver, resid);

    TRACE("<- 0x%08x\n", hr);

    return hr;
}

static HRESULT WINAPI holder_TrackResource(IHolder *iface, const RESID resid)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p)->(%08lx) stub\n", This, resid);

    return E_NOTIMPL;
}

static HRESULT WINAPI holder_TrackResourceS(IHolder *iface, const SRESID resid)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p)->(%s) stub\n", This, debugstr_w(resid));

    return E_NOTIMPL;
}

static HRESULT WINAPI holder_UntrackResource(IHolder *iface, const RESID resid, const BOOL value)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p)->(%08lx, %d) stub\n", This, resid, value);

    return E_NOTIMPL;
}

static HRESULT WINAPI holder_UntrackResourceS(IHolder *iface, const SRESID resid, const BOOL value)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p)->(%s, %d) stub\n", This, debugstr_w(resid), value);

    return E_NOTIMPL;
}

static HRESULT WINAPI holder_Close(IHolder *iface)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p) stub\n", This);

    IDispenserDriver_Release(This->driver);

    return S_OK;
}

static HRESULT WINAPI holder_RequestDestroyResource(IHolder *iface, const RESID resid)
{
    holder *This = impl_from_IHolder(iface);

    FIXME("(%p)->(%08lx) stub\n", This, resid);

    return E_NOTIMPL;
}

struct IHolderVtbl holder_vtbl =
{
    holder_QueryInterface,
    holder_AddRef,
    holder_Release,
    holder_AllocResource,
    holder_FreeResource,
    holder_TrackResource,
    holder_TrackResourceS,
    holder_UntrackResource,
    holder_UntrackResourceS,
    holder_Close,
    holder_RequestDestroyResource
};

static HRESULT create_holder(IDispenserDriver *driver, IHolder **object)
{
    holder *hold;
    HRESULT ret;

    TRACE("(%p)\n", object);

    hold = heap_alloc(sizeof(*hold));
    if (!hold)
    {
        *object = NULL;
        return E_OUTOFMEMORY;
    }

    hold->IHolder_iface.lpVtbl = &holder_vtbl;
    hold->ref = 1;
    hold->driver = driver;

    ret = holder_QueryInterface(&hold->IHolder_iface, &IID_IHolder, (void**)object);
    holder_Release(&hold->IHolder_iface);

    return ret;
}

static HRESULT WINAPI dismanager_QueryInterface(IDispenserManager *iface, REFIID riid, void **object)
{
    dispensermanager *This = impl_from_IDispenserManager(iface);

    TRACE("(%p)->(%s %p)\n", This, debugstr_guid(riid), object);

    *object = NULL;

    if (IsEqualGUID(riid, &IID_IUnknown) ||
        IsEqualGUID(riid, &IID_IDispenserManager))
    {
        *object = &This->IDispenserManager_iface;
        IUnknown_AddRef( (IUnknown*)*object);

        return S_OK;
    }

    WARN("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),object);
    return E_NOINTERFACE;
}

static ULONG WINAPI dismanager_AddRef(IDispenserManager *iface)
{
    dispensermanager *This = impl_from_IDispenserManager(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);
    return ref;
}

static ULONG WINAPI dismanager_Release(IDispenserManager *iface)
{
    dispensermanager *This = impl_from_IDispenserManager(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p)->(%d)\n", This, ref);

    if (!ref)
    {
        if (This->mta_thread)
        {
            SetEvent(This->mta_stop_event);
            WaitForSingleObject(This->mta_thread, INFINITE);
            CloseHandle(This->mta_stop_event);
            CloseHandle(This->mta_thread);
        }
        heap_free(This);
    }

    return ref;
}

static DWORD WINAPI mta_thread_proc(void *arg)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    WaitForSingleObject(arg, INFINITE);
    CoUninitialize();
    return 0;
}

static HRESULT WINAPI dismanager_RegisterDispenser(IDispenserManager *iface, IDispenserDriver *driver,
                        LPCOLESTR name, IHolder **dispenser)
{
    dispensermanager *This = impl_from_IDispenserManager(iface);
    HRESULT hr;

    TRACE("(%p)->(%p, %s, %p)\n", This, driver, debugstr_w(name), dispenser);

    if(!dispenser)
        return E_INVALIDARG;

    hr = create_holder(driver, dispenser);

    if (!This->mta_thread)
    {
        This->mta_stop_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        This->mta_thread = CreateThread(NULL, 0, mta_thread_proc, This->mta_stop_event, 0, NULL);
    }

    TRACE("<-- 0x%08x, %p\n", hr, *dispenser);

    return hr;
}

static HRESULT WINAPI dismanager_GetContext(IDispenserManager *iface, INSTID *id, TRANSID *transid)
{
    dispensermanager *This = impl_from_IDispenserManager(iface);

    FIXME("(%p)->(%p, %p) stub\n", This, id, transid);

    return E_NOTIMPL;
}

struct IDispenserManagerVtbl dismanager_vtbl =
{
    dismanager_QueryInterface,
    dismanager_AddRef,
    dismanager_Release,
    dismanager_RegisterDispenser,
    dismanager_GetContext
};

static HRESULT WINAPI dispenser_manager_cf_CreateInstance(IClassFactory *iface, IUnknown* outer, REFIID riid,
        void **object)
{
    dispensermanager *dismanager;
    HRESULT ret;

    TRACE("(%p %s %p)\n", outer, debugstr_guid(riid), object);

    dismanager = heap_alloc_zero(sizeof(*dismanager));
    if (!dismanager)
    {
        *object = NULL;
        return E_OUTOFMEMORY;
    }

    dismanager->IDispenserManager_iface.lpVtbl = &dismanager_vtbl;
    dismanager->ref = 1;

    ret = dismanager_QueryInterface(&dismanager->IDispenserManager_iface, riid, object);
    dismanager_Release(&dismanager->IDispenserManager_iface);

    return ret;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID lpv)
{
    switch(reason)
    {
    case DLL_WINE_PREATTACH:
        return FALSE;    /* prefer native version */
    case DLL_PROCESS_ATTACH:
        COMSVCS_hInstance = hinst;
        DisableThreadLibraryCalls(hinst);
        break;
    }
    return TRUE;
}

static HRESULT WINAPI comsvcscf_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv )
{
    *ppv = NULL;

    if(IsEqualGUID(&IID_IUnknown, riid)) {
        TRACE("(%p)->(IID_IUnknown %p)\n", iface, ppv);
        *ppv = iface;
    }else if(IsEqualGUID(&IID_IClassFactory, riid)) {
        TRACE("(%p)->(IID_IClassFactory %p)\n", iface, ppv);
        *ppv = iface;
    }

    if(*ppv) {
        IUnknown_AddRef((IUnknown*)*ppv);
        return S_OK;
    }

    WARN("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppv);
    return E_NOINTERFACE;
}

static ULONG WINAPI comsvcscf_AddRef(IClassFactory *iface )
{
    TRACE("(%p)\n", iface);
    return 2;
}

static ULONG WINAPI comsvcscf_Release(IClassFactory *iface )
{
    TRACE("(%p)\n", iface);
    return 1;
}

static HRESULT WINAPI comsvcscf_LockServer(IClassFactory *iface, BOOL fLock)
{
    TRACE("(%p)->(%x)\n", iface, fLock);
    return S_OK;
}

static const IClassFactoryVtbl comsvcscf_vtbl =
{
    comsvcscf_QueryInterface,
    comsvcscf_AddRef,
    comsvcscf_Release,
    dispenser_manager_cf_CreateInstance,
    comsvcscf_LockServer
};

static HRESULT WINAPI new_moniker_QueryInterface(IMoniker* iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualIID(&IID_IUnknown, riid) ||
        IsEqualIID(&IID_IPersist, riid) ||
        IsEqualIID(&IID_IPersistStream, riid) ||
        IsEqualIID(&IID_IMoniker, riid))
    {
        *obj = iface;
    }

    if (*obj)
    {
        IUnknown_AddRef((IUnknown *)*obj);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI new_moniker_AddRef(IMoniker* iface)
{
    struct new_moniker *moniker = impl_from_IMoniker(iface);
    ULONG refcount = InterlockedIncrement(&moniker->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI new_moniker_Release(IMoniker* iface)
{
    struct new_moniker *moniker = impl_from_IMoniker(iface);
    ULONG refcount = InterlockedDecrement(&moniker->refcount);

    TRACE("%p, refcount %u.\n", iface, refcount);

    if (!refcount)
        heap_free(moniker);

    return refcount;
}

static HRESULT WINAPI new_moniker_GetClassID(IMoniker *iface, CLSID *clsid)
{
    FIXME("%p, %p.\n", iface, clsid);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_IsDirty(IMoniker* iface)
{
    FIXME("%p.\n", iface);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_Load(IMoniker *iface, IStream *stream)
{
    FIXME("%p, %p.\n", iface, stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_Save(IMoniker *iface, IStream *stream, BOOL clear_dirty)
{
    FIXME("%p, %p, %d.\n", iface, stream, clear_dirty);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_GetSizeMax(IMoniker *iface, ULARGE_INTEGER *size)
{
    FIXME("%p, %p.\n", iface, size);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_BindToObject(IMoniker *iface, IBindCtx *pbc, IMoniker *pmkToLeft,
        REFIID riid, void **ret)
{
    FIXME("%p, %p, %p, %s, %p.\n", iface, pbc, pmkToLeft, debugstr_guid(riid), ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_BindToStorage(IMoniker *iface, IBindCtx *pbc, IMoniker *pmkToLeft, REFIID riid,
        void **ret)
{
    FIXME("%p, %p, %p, %s, %p.\n", iface, pbc, pmkToLeft, debugstr_guid(riid), ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_Reduce(IMoniker *iface, IBindCtx *pbc, DWORD flags, IMoniker **ppmkToLeft,
        IMoniker **ret)
{
    FIXME("%p, %p, %d, %p, %p.\n", iface, pbc, flags, ppmkToLeft, ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_ComposeWith(IMoniker *iface, IMoniker *mkRight, BOOL fOnlyIfNotGeneric,
        IMoniker **ret)
{
    FIXME("%p, %p, %d, %p.\n", iface, mkRight, fOnlyIfNotGeneric, ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_Enum(IMoniker *iface, BOOL forward, IEnumMoniker **enum_moniker)
{
    FIXME("%p, %d, %p.\n", iface, forward, enum_moniker);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_IsEqual(IMoniker *iface, IMoniker *other_moniker)
{
    FIXME("%p, %p.\n", iface, other_moniker);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_Hash(IMoniker *iface, DWORD *hash)
{
    struct new_moniker *moniker = impl_from_IMoniker(iface);

    TRACE("%p, %p.\n", iface, hash);

    *hash = moniker->clsid.Data1;

    return S_OK;
}

static HRESULT WINAPI new_moniker_IsRunning(IMoniker* iface, IBindCtx *pbc, IMoniker *pmkToLeft,
        IMoniker *pmkNewlyRunning)
{
    FIXME("%p, %p, %p, %p.\n", iface, pbc, pmkToLeft, pmkNewlyRunning);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_GetTimeOfLastChange(IMoniker *iface, IBindCtx *pbc, IMoniker *pmkToLeft,
        FILETIME *itemtime)
{
    TRACE("%p, %p, %p, %p.\n", iface, pbc, pmkToLeft, itemtime);

    return MK_E_UNAVAILABLE;
}

static HRESULT WINAPI new_moniker_Inverse(IMoniker *iface, IMoniker **inverse)
{
    TRACE("%p, %p.\n", iface, inverse);

    return CreateAntiMoniker(inverse);
}

static HRESULT WINAPI new_moniker_CommonPrefixWith(IMoniker *iface, IMoniker *other, IMoniker **ret)
{
    FIXME("%p, %p, %p.\n", iface, other, ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_RelativePathTo(IMoniker *iface, IMoniker *other, IMoniker **ret)
{
    FIXME("%p, %p, %p.\n", iface, other, ret);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_GetDisplayName(IMoniker *iface, IBindCtx *pbc, IMoniker *pmkToLeft,
        LPOLESTR *name)
{
    FIXME("%p, %p, %p, %p.\n", iface, pbc, pmkToLeft, name);

    return E_NOTIMPL;
}

static HRESULT WINAPI new_moniker_ParseDisplayName(IMoniker *iface, IBindCtx *pbc, IMoniker *pmkToLeft,
        LPOLESTR name, ULONG *eaten, IMoniker **ret)
{
    TRACE("%p, %p, %p, %s, %p, %p.\n", iface, pbc, pmkToLeft, debugstr_w(name), eaten, ret);

    return new_moniker_parse_displayname(pbc, name, eaten, ret);
}

static HRESULT WINAPI new_moniker_IsSystemMoniker(IMoniker *iface, DWORD *moniker_type)
{
    TRACE("%p, %p.\n", iface, moniker_type);

    *moniker_type = MKSYS_NONE;

    return S_FALSE;
}

static const IMonikerVtbl new_moniker_vtbl =
{
    new_moniker_QueryInterface,
    new_moniker_AddRef,
    new_moniker_Release,
    new_moniker_GetClassID,
    new_moniker_IsDirty,
    new_moniker_Load,
    new_moniker_Save,
    new_moniker_GetSizeMax,
    new_moniker_BindToObject,
    new_moniker_BindToStorage,
    new_moniker_Reduce,
    new_moniker_ComposeWith,
    new_moniker_Enum,
    new_moniker_IsEqual,
    new_moniker_Hash,
    new_moniker_IsRunning,
    new_moniker_GetTimeOfLastChange,
    new_moniker_Inverse,
    new_moniker_CommonPrefixWith,
    new_moniker_RelativePathTo,
    new_moniker_GetDisplayName,
    new_moniker_ParseDisplayName,
    new_moniker_IsSystemMoniker
};

static const BYTE guid_conv_table[256] =
{
    0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x00 */
    0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x10 */
    0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x20 */
    0,   1,   2,   3,   4,   5,   6, 7, 8, 9, 0, 0, 0, 0, 0, 0, /* 0x30 */
    0, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x40 */
    0,   0,   0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 0x50 */
    0, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf                             /* 0x60 */
};

static BOOL is_valid_hex(WCHAR c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F');
}

static HRESULT guid_from_string(const WCHAR *s, GUID *ret)
{
    BOOL has_brackets;
    GUID guid = { 0 };
    int i;

    memset(ret, 0, sizeof(*ret));

    /* Curly brackets are optional. */
    has_brackets = s[0] == '{';

    if (has_brackets)
        s++;

    for (i = 0; i < 8; i++)
    {
        if (!is_valid_hex(s[i])) return FALSE;
        guid.Data1 = (guid.Data1 << 4) | guid_conv_table[s[i]];
    }
    s += 8;

    if (s[0] != '-') return FALSE;
    s++;

    for (i = 0; i < 4; i++)
    {
        if (!is_valid_hex(s[0])) return FALSE;
        guid.Data2 = (guid.Data2 << 4) | guid_conv_table[s[i]];
    }
    s += 4;

    if (s[0] != '-') return FALSE;
    s++;

    for (i = 0; i < 4; i++)
    {
        if (!is_valid_hex(s[i])) return FALSE;
        guid.Data3 = (guid.Data3 << 4) | guid_conv_table[s[i]];
    }
    s += 4;

    if (s[0] != '-') return FALSE;
    s++;

    for (i = 0; i < 17; i += 2)
    {
        if (i == 4)
        {
            if (s[i] != '-') return FALSE;
            i++;
        }
        if (!is_valid_hex(s[i]) || !is_valid_hex(s[i+1])) return FALSE;
        guid.Data4[i / 2] = guid_conv_table[s[i]] << 4 | guid_conv_table[s[i+1]];
    }
    s += 17;

    if (has_brackets && s[0] != '}')
        return FALSE;

    *ret = guid;

    return TRUE;
}

static HRESULT new_moniker_parse_displayname(IBindCtx *pbc, LPOLESTR name, ULONG *eaten, IMoniker **ret)
{
    struct new_moniker *moniker;
    GUID guid;

    *ret = NULL;

    if (wcsnicmp(name, L"new:", 4))
        return MK_E_SYNTAX;

    if (!guid_from_string(name + 4, &guid))
        return MK_E_SYNTAX;

    moniker = heap_alloc_zero(sizeof(*moniker));
    if (!moniker)
        return E_OUTOFMEMORY;

    moniker->IMoniker_iface.lpVtbl = &new_moniker_vtbl;
    moniker->refcount = 1;
    moniker->clsid = guid;

    *ret = &moniker->IMoniker_iface;

    if (eaten)
        *eaten = lstrlenW(name);

    return S_OK;
}

static HRESULT WINAPI new_moniker_parse_QueryInterface(IParseDisplayName *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (IsEqualIID(riid, &IID_IParseDisplayName) ||
            IsEqualIID(riid, &IID_IUnknown))
    {
        *obj = iface;
        IParseDisplayName_AddRef(iface);
        return S_OK;
    }

    *obj = NULL;
    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI new_moniker_parse_AddRef(IParseDisplayName *iface)
{
    return 2;
}

static ULONG WINAPI new_moniker_parse_Release(IParseDisplayName *iface)
{
    return 1;
}

static HRESULT WINAPI new_moniker_parse_ParseDisplayName(IParseDisplayName *iface, IBindCtx *pbc, LPOLESTR name,
        ULONG *eaten, IMoniker **ret)
{
    TRACE("%p, %p, %s, %p, %p.\n", iface, pbc, debugstr_w(name), eaten, ret);

    return new_moniker_parse_displayname(pbc, name, eaten, ret);
}

static const IParseDisplayNameVtbl new_moniker_parse_vtbl =
{
    new_moniker_parse_QueryInterface,
    new_moniker_parse_AddRef,
    new_moniker_parse_Release,
    new_moniker_parse_ParseDisplayName,
};

static IParseDisplayName new_moniker_parse = { &new_moniker_parse_vtbl };

static HRESULT WINAPI new_moniker_cf_QueryInterface(IClassFactory *iface, REFIID riid, void **obj)
{
    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualGUID(&IID_IUnknown, riid) ||
            IsEqualGUID(&IID_IClassFactory, riid))
    {
        *obj = iface;
    }
    else if (IsEqualIID(&IID_IParseDisplayName, riid))
    {
        *obj = &new_moniker_parse;
    }

    if (*obj)
    {
        IUnknown_AddRef((IUnknown *)*obj);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static HRESULT WINAPI new_moniker_cf_CreateInstance(IClassFactory *iface, IUnknown* outer, REFIID riid, void **object)
{
    TRACE("%p, %p, %s, %p.\n", iface, outer, debugstr_guid(riid), object);

    if (outer)
        FIXME("Aggregation is not supported.\n");

    return IParseDisplayName_QueryInterface(&new_moniker_parse, riid, object);
}

static const IClassFactoryVtbl newmoniker_cf_vtbl =
{
    new_moniker_cf_QueryInterface,
    comsvcscf_AddRef,
    comsvcscf_Release,
    new_moniker_cf_CreateInstance,
    comsvcscf_LockServer
};

static IClassFactory DispenserManageFactory = { &comsvcscf_vtbl };
static IClassFactory NewMonikerFactory = { &newmoniker_cf_vtbl };

/******************************************************************
 * DllGetClassObject
 */
HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void **ppv)
{
    if(IsEqualGUID(&CLSID_DispenserManager, rclsid))
    {
        TRACE("(CLSID_DispenserManager %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&DispenserManageFactory, riid, ppv);
    }
    else if (IsEqualGUID(&CLSID_NewMoniker, rclsid))
    {
        TRACE("(CLSID_NewMoniker %s %p)\n", debugstr_guid(riid), ppv);
        return IClassFactory_QueryInterface(&NewMonikerFactory, riid, ppv);
    }

    FIXME("%s %s %p\n", debugstr_guid(rclsid), debugstr_guid(riid), ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

/******************************************************************
 * DllCanUnloadNow
 */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/***********************************************************************
 *		DllRegisterServer (comsvcs.@)
 */
HRESULT WINAPI DllRegisterServer(void)
{
    return __wine_register_resources( COMSVCS_hInstance );
}

/***********************************************************************
 *		DllUnregisterServer (comsvcs.@)
 */
HRESULT WINAPI DllUnregisterServer(void)
{
    return __wine_unregister_resources( COMSVCS_hInstance );
}
