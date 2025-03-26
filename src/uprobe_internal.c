#include "postgres.h"
#include "utils/memutils.h"

#include "frida-gum.h"
#include "uprobe_message_buffer.h"

#include "uprobe_internal.h"

//frida
typedef struct _UprobeListenerNoArgs UprobeListenerNoArgs;

struct _UprobeListenerNoArgs {
    GObject parent;
};

static void uprobe_listener_no_args_iface_init(gpointer g_iface, gpointer iface_data);
#define UPROBE_TYPE_LISTENERNOARGS (uprobe_listener_no_args_get_type())
G_DECLARE_FINAL_TYPE(UprobeListenerNoArgs, uprobe_listener_no_args, UPROBE, LISTENERNOARGS,
             GObject)
G_DEFINE_TYPE_EXTENDED(UprobeListenerNoArgs, uprobe_listener_no_args, G_TYPE_OBJECT, 0,
               G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                         uprobe_listener_no_args_iface_init))




typedef struct _UprobeListenerHasArgs UprobeListenerHasArgs;

struct _UprobeListenerHasArgs {
    GObject parent;
};

static void uprobe_listener_has_args_iface_init(gpointer g_iface, gpointer iface_data);
#define UPROBE_TYPE_LISTENERHASARGS (uprobe_listener_has_args_get_type())
G_DECLARE_FINAL_TYPE(UprobeListenerHasArgs, uprobe_listener_has_args, UPROBE, LISTENERHASARGS,
             GObject)
G_DEFINE_TYPE_EXTENDED(UprobeListenerHasArgs, uprobe_listener_has_args, G_TYPE_OBJECT, 0,
               G_IMPLEMENT_INTERFACE(GUM_TYPE_INVOCATION_LISTENER,
                         uprobe_listener_has_args_iface_init))



//end frida

//global

MemoryContext UprobeMemoryContext = NULL;

static GumInterceptor *interceptor = NULL;


struct Uprobe
{
    bool isAttached;
    GumInvocationListener* listener;
    UprobeAttachInterface* uprobeInterface;
};




void
UprobeDelete(Uprobe* uprobe)
{
    if (uprobe == NULL)
        return;

    if (uprobe->isAttached)
        gum_interceptor_detach(interceptor, uprobe->listener);

    if (uprobe->listener)
        g_object_unref(uprobe->listener);
    uprobe->uprobeInterface->cleanFunc(uprobe->uprobeInterface);
    pfree(uprobe);
}


UPROBE_INIT_RES
UprobeInit(UprobeAttachInterface* uprobeAttach, Uprobe** uprobe)
{
    Uprobe* result;
    int res;
    void* funcAddr;
    MemoryContext old;


    old = MemoryContextSwitchTo(UprobeMemoryContext);
    result = (Uprobe* ) palloc0(sizeof(Uprobe));
    result->uprobeInterface = uprobeAttach;

    MemoryContextSwitchTo(old);
    if (uprobeAttach->numArgs < 0 || uprobeAttach->numArgs > 8)
    {
        UprobeDelete(result);
        return INVALID_NUMBER_OF_ARGS;
    }
    funcAddr = gum_find_function(uprobeAttach->targetSymbol);
    if (funcAddr == NULL)
    {
        UprobeDelete(result);
        return CANNOT_FIND_SYMBOL;
    }
    gum_interceptor_begin_transaction(interceptor);
    if (uprobeAttach->numArgs == 0)
        result->listener = (GumInvocationListener* )g_object_new(uprobe_listener_no_args_get_type(), NULL);
    else
        result->listener = (GumInvocationListener* )g_object_new(uprobe_listener_has_args_get_type(), NULL);
    if (result->listener == NULL)
    {
        gum_interceptor_end_transaction(interceptor);
        UprobeDelete(result);
        return INTERNAL_ERROR;
    }

    res = gum_interceptor_attach(interceptor, funcAddr, result->listener, result);

    if (res < 0)
    {
        gum_interceptor_end_transaction(interceptor);
        UprobeDelete(result);
        return INTERNAL_ERROR;
    }

    gum_interceptor_end_transaction(interceptor);

    result->isAttached = true;
    *uprobe = result;
    return SUCCESS;
}


int UprobeCompare(Uprobe* uprobe, char* func)
{
    return strcmp(uprobe->uprobeInterface->targetSymbol, func);
}


const char*
UprobeGetFunc(Uprobe* uprobe)
{
    return uprobe->uprobeInterface->targetSymbol;
}


void
UprobeCallTimedCallback(Uprobe* uprobe)
{
    if (uprobe->uprobeInterface->timedCallback)
        uprobe->uprobeInterface->timedCallback(uprobe->uprobeInterface);
}


const UprobeAttachInterface*
UprobeGetAttachInterface(Uprobe* uprobe)
{
    return uprobe->uprobeInterface;
}


void
UprobeInternalInit(void)
{
    UprobeMemoryContext = AllocSetContextCreate(TopMemoryContext, "uprobe global context", ALLOCSET_DEFAULT_SIZES);

    // init frida
    gum_init();
    interceptor = gum_interceptor_obtain();
}


void
UprobeInternalFini(void)
{
    MemoryContextDelete(UprobeMemoryContext);

    //fini frida
    g_object_unref(interceptor);
    gum_deinit();
}


//fida code
// arg getting could be slow, will need to make on pure cpu contexts without functions calls
static void
uprobe_listener_on_enter_no_args(GumInvocationListener *listener, GumInvocationContext *ic)
{
    struct Uprobe* hookEntry = gum_invocation_context_get_listener_function_data(ic);
    void (*function)(void*) = hookEntry->uprobeInterface->inFunc;
    function(hookEntry->uprobeInterface->data);
}


static void
uprobe_listener_on_enter_has_args(GumInvocationListener *listener, GumInvocationContext *ic)
{
    struct Uprobe* hookEntry = gum_invocation_context_get_listener_function_data(ic);
    switch (hookEntry->uprobeInterface->numArgs)
    {
    case 1:
        {
            void (*function)(void*, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data, gum_invocation_context_get_nth_argument(ic, 0));
            break;
        }
    case 2:
        {
            void (*function)(void*, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1));
            break;
        }
    case 3:
        {
            void (*function)(void*, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2));
            break;
        }
    case 4:
        {
            void (*function)(void*, gpointer, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2),
                gum_invocation_context_get_nth_argument(ic, 3));
            break;
        }
    case 5:
        {
            void (*function)(void*, gpointer, gpointer, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2),
                gum_invocation_context_get_nth_argument(ic, 3),
                gum_invocation_context_get_nth_argument(ic, 4));
            break;
        }
    case 6:
        {
            void (*function)(void*, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2),
                gum_invocation_context_get_nth_argument(ic, 3),
                gum_invocation_context_get_nth_argument(ic, 4),
                gum_invocation_context_get_nth_argument(ic, 5));
            break;
        }
    case 7:
        {
            void (*function)(void*, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2),
                gum_invocation_context_get_nth_argument(ic, 3),
                gum_invocation_context_get_nth_argument(ic, 4),
                gum_invocation_context_get_nth_argument(ic, 5),
                gum_invocation_context_get_nth_argument(ic, 6));
            break;
        }
    case 8:
        {
            void (*function)(void*, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer, gpointer) = hookEntry->uprobeInterface->inFunc;
            function(hookEntry->uprobeInterface->data,
                gum_invocation_context_get_nth_argument(ic, 0),
                gum_invocation_context_get_nth_argument(ic, 1),
                gum_invocation_context_get_nth_argument(ic, 2),
                gum_invocation_context_get_nth_argument(ic, 3),
                gum_invocation_context_get_nth_argument(ic, 4),
                gum_invocation_context_get_nth_argument(ic, 5),
                gum_invocation_context_get_nth_argument(ic, 6),
                gum_invocation_context_get_nth_argument(ic, 7));
            break;
        }
    default:
        break;
    }
}


static void
uprobe_listener_on_leave(GumInvocationListener *listener, GumInvocationContext *ic)
{
    struct Uprobe* hookEntry = gum_invocation_context_get_listener_function_data(ic);

    if (unlikely(hookEntry->uprobeInterface->needRetVal))
    {
        void (*function)(void*, gpointer) = hookEntry->uprobeInterface->retFunc;
        function(hookEntry->uprobeInterface->data, gum_invocation_context_get_return_value(ic));
    }
    else
    {
        void (*function)(void*) = hookEntry->uprobeInterface->retFunc;
        function(hookEntry->uprobeInterface->data);
    }
}


static void
uprobe_listener_no_args_class_init(UprobeListenerNoArgsClass *klass)
{
    (void)UPROBE_IS_LISTENERNOARGS;
}


static void
uprobe_listener_no_args_iface_init(gpointer g_iface, gpointer iface_data)
{
    GumInvocationListenerInterface *iface = (GumInvocationListenerInterface *)g_iface;
    iface->on_enter = uprobe_listener_on_enter_no_args;
    iface->on_leave = uprobe_listener_on_leave;
}


static void
uprobe_listener_no_args_init(UprobeListenerNoArgs *self)
{
}


static void
uprobe_listener_has_args_class_init(UprobeListenerHasArgsClass *klass)
{
    (void)UPROBE_IS_LISTENERHASARGS;
}


static void
uprobe_listener_has_args_iface_init(gpointer g_iface, gpointer iface_data)
{
    GumInvocationListenerInterface *iface = (GumInvocationListenerInterface *)g_iface;
    iface->on_enter = uprobe_listener_on_enter_has_args;
    iface->on_leave = uprobe_listener_on_leave;
}


static void
uprobe_listener_has_args_init(UprobeListenerHasArgs *self)
{
}
