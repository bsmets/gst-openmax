/*
 * Copyright (C) 2006-2007 Texas Instruments, Incorporated
 * Copyright (C) 2007-2008 Nokia Corporation.
 * Copyright (C) 2008 NXP. All rights reserved.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 * Contributors:
 * Frederik Vernelen <frederik.vernelen@nxp.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef GSTOMX_UTIL_H
#define GSTOMX_UTIL_H

#include <stdbool.h>
#include <glib.h>
#include <OMX_Core.h>
#include <OMX_Component.h>

#include <async_queue.h>

/* Typedefs. */

typedef struct GOmxCore GOmxCore;
typedef struct GOmxPort GOmxPort;
typedef struct GOmxSem GOmxSem;
typedef struct GOmxImp GOmxImp;
typedef struct GOmxSymbolTable GOmxSymbolTable;
typedef struct GOmxPadData GOmxPadData;
typedef enum GOmxPortType GOmxPortType;

typedef void (*GOmxCb) (GOmxCore *core);
typedef void (*GOmxEventCb) (GOmxCore *core,
                             OMX_EVENTTYPE eEvent,
                             OMX_U32 nData1,
                             OMX_U32 nData2,
                             OMX_PTR pEventData);
typedef void (*GOmxPortCb) (GOmxPort *port);

/* Enums. */

enum GOmxPortType
{
    GOMX_PORT_INPUT,
    GOMX_PORT_OUTPUT
};

/* Structures. */

struct GOmxSymbolTable
{
    OMX_ERRORTYPE (*init) (void);
    OMX_ERRORTYPE (*deinit) (void);
    OMX_ERRORTYPE (*get_handle) (OMX_HANDLETYPE *handle,
                                 OMX_STRING name,
                                 OMX_PTR data,
                                 OMX_CALLBACKTYPE *callbacks);
    OMX_ERRORTYPE (*free_handle) (OMX_HANDLETYPE handle);
    OMX_ERRORTYPE (*setup_tunnel)  (OMX_HANDLETYPE hOutput,
                                    OMX_U32 nPortOutput,
                                    OMX_HANDLETYPE hInput,
                                    OMX_U32 nPortInput);
};

struct GOmxImp
{
    guint client_count;
    void *dl_handle;
    GOmxSymbolTable sym_table;
};

struct GOmxCore
{
    OMX_HANDLETYPE omx_handle;
    OMX_STATETYPE omx_state;
    OMX_ERRORTYPE omx_error;

    GPtrArray *ports;

    gpointer client_data; /**< Placeholder for the client data. */

    GOmxSem *state_sem;
    GOmxSem *port_state_sem;
    GOmxSem *done_sem;
    GOmxSem *flush_sem;

    GOmxCb settings_changed_cb;
    GOmxEventCb event_handler_cb;
    GOmxImp *imp;

    gboolean done;
};

struct GOmxPort
{
    GOmxCore *core;
    GOmxPortType type;

    guint num_buffers;
    gulong buffer_size;
    OMX_BUFFERHEADERTYPE **buffers;

    GMutex *mutex;
    gboolean enabled;
    AsyncQueue *queue;

    gboolean tunneled;
    gboolean linked;
};

struct GOmxSem
{
    GCond *condition;
    GMutex *mutex;
    gint counter;
};

struct GOmxPadData
{
    gboolean setting_tunnel;
};

/* Functions. */

void g_omx_init (void);
void g_omx_deinit (void);

GOmxCore *g_omx_core_new (void);
void g_omx_core_free (GOmxCore *core);
void g_omx_core_init (GOmxCore *core, const gchar *library_name, const gchar *component_name);
void g_omx_core_deinit (GOmxCore *core);
void g_omx_core_prepare (GOmxCore *core);
void g_omx_core_release_buffer (GOmxCore *core, GOmxPort *port);
void g_omx_core_setup_tunnel (GOmxCore *core, GOmxCore *peercore);
void g_omx_core_start (GOmxCore *core);
void g_omx_core_pause (GOmxCore *core);
void g_omx_core_finish (GOmxCore *core);
void g_omx_core_set_done (GOmxCore *core);
void g_omx_core_wait_for_done (GOmxCore *core);
GOmxPort *g_omx_core_setup_port (GOmxCore *core, OMX_PARAM_PORTDEFINITIONTYPE *omx_port);

GOmxPort *g_omx_port_new (GOmxCore *core);
void g_omx_port_free (GOmxPort *port);
void g_omx_port_setup (GOmxPort *port, OMX_PARAM_PORTDEFINITIONTYPE *omx_port);
void g_omx_port_push_buffer (GOmxPort *port, OMX_BUFFERHEADERTYPE *omx_buffer);
OMX_BUFFERHEADERTYPE *g_omx_port_request_buffer (GOmxPort *port);
void g_omx_port_release_buffer (GOmxPort *port, OMX_BUFFERHEADERTYPE *omx_buffer);
void g_omx_port_enable (GOmxPort *port);
void g_omx_port_disable (GOmxPort *port);
void g_omx_port_finish (GOmxPort *port);

GOmxSem *g_omx_sem_new (void);
void g_omx_sem_free (GOmxSem *sem);
void g_omx_sem_down (GOmxSem *sem);
void g_omx_sem_up (GOmxSem *sem);

#endif /* GSTOMX_UTIL_H */
