#include "swayclip-connection.h"
#include "common/ipc_ct.h"

struct _SwayclipConnection
{
    GObject parent;

    GThread *ipc_thread;
};

G_DEFINE_TYPE(SwayclipConnection, swayclip_connection, G_TYPE_OBJECT)

static void
swayclip_connection_class_init(SwayclipConnectionClass *class)
{
}

static void
swayclip_connection_init(SwayclipConnection *self)
{
}
