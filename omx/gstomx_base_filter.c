/*
 * Copyright (C) 2007-2008 Nokia Corporation.
 * Copyright (C) 2008 NXP.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 * Author: Frederik Vernelen <frederik.vernelen@nxp.com>
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

#include "gstomx_base_filter.h"
#include "gstomx.h"

#include <string.h> /* For memcpy */

static gboolean share_input_buffer = FALSE;
static gboolean share_output_buffer = FALSE;

static void output_loop (gpointer data);

static gboolean send_disable_event (GstOmxBaseFilter *self, GstPad *pad);
static gboolean send_enable_event (GstOmxBaseFilter *self, GstPad *pad);
static GstPad* find_real_peer (GstPad *pad, GstPad *peer);

enum
{
    ARG_0,
    ARG_COMPONENT_NAME,
    ARG_LIBRARY_NAME,
    ARG_USE_TIMESTAMPS,
    ARG_TUNNELING,
    ARG_GOMX_CORE
};

static GstElementClass *parent_class = NULL;

static void
setup_ports (GstOmxBaseFilter *self)
{
    GOmxCore *core;
    OMX_PARAM_PORTDEFINITIONTYPE *param;

    core = self->gomx;

    param = calloc (1, sizeof (OMX_PARAM_PORTDEFINITIONTYPE));
    param->nSize = sizeof (OMX_PARAM_PORTDEFINITIONTYPE);
    param->nVersion.s.nVersionMajor = 1;
    param->nVersion.s.nVersionMinor = 1;

    /* Input port configuration. */

    param->nPortIndex = 0;
    OMX_GetParameter (core->omx_handle, OMX_IndexParamPortDefinition, param);
    self->in_port = g_omx_core_setup_port (core, param);

    /* Output port configuration. */

    param->nPortIndex = 1;
    OMX_GetParameter (core->omx_handle, OMX_IndexParamPortDefinition, param);
    self->out_port = g_omx_core_setup_port (core, param);

    free (param);
}

static void
enable_tunneled_port (GstOmxBaseFilter *self,
                      GOmxPort *port,
                      GstPad *pad)
{
    GOmxCore *gomx;

    gomx = self->gomx;
    OMX_ERRORTYPE omx_error;

    if (port->tunneled && !port->enabled)
    {
        omx_error = OMX_SendCommand (gomx->omx_handle, OMX_CommandPortEnable, port->port_index, NULL);
        send_enable_event (self, pad);
        port->enabled = TRUE;
        g_omx_sem_down (gomx->port_state_sem);
    }
}

static void
disable_tunneled_port (GstOmxBaseFilter *self,
                       GOmxPort *port,
                       GstPad *pad)
{
    GOmxCore *gomx;

    gomx = self->gomx;
    OMX_ERRORTYPE omx_error;

    if (port->tunneled && port->enabled)
    {
        omx_error = OMX_SendCommand (gomx->omx_handle, OMX_CommandPortDisable, port->port_index, NULL);
        send_disable_event (self, pad);
        port->enabled = FALSE;
        g_omx_sem_down (gomx->port_state_sem);
    }
}

static void
disable_tunneled_ports (GstOmxBaseFilter *self)
{
    disable_tunneled_port (self, self->in_port, self->sinkpad);
    disable_tunneled_port (self, self->out_port, self->srcpad);
}

static void
enable_tunneled_ports (GstOmxBaseFilter *self)
{
    enable_tunneled_port (self, self->in_port, self->sinkpad);
    enable_tunneled_port (self, self->out_port, self->srcpad);
}

static GstStateChangeReturn
change_state (GstElement *element,
              GstStateChange transition)
{
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (element);

    GST_LOG_OBJECT (self, "begin");

    GST_INFO_OBJECT (self, "changing state %s - %s",
                     gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
                     gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

    switch (transition)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
            g_omx_core_init (self->gomx, self->omx_library, self->omx_component);
            if (self->gomx->omx_error)
                return GST_STATE_CHANGE_FAILURE;

            GST_INFO_OBJECT (self, "omx: prepare");

            /** @todo this should probably go after doing preparations. */
            if (self->omx_setup)
            {
                self->omx_setup (self);
            }

            setup_ports (self);

            g_omx_core_prepare (self->gomx);

            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            g_omx_port_finish (self->in_port);
            g_omx_port_finish (self->out_port);
            break;

        case GST_STATE_CHANGE_READY_TO_PAUSED:
            enable_tunneled_ports (self);

            if (!self->out_port->tunneled && self->out_port->linked)
                gst_pad_start_task (self->srcpad, output_loop, self->srcpad);

            g_omx_core_start (self->gomx);
            break;

        default:
            break;
    }

    ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

    if (ret == GST_STATE_CHANGE_FAILURE)
        return ret;

    switch (transition)
    {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            gst_pad_pause_task (self->srcpad);
            g_omx_core_finish (self->gomx);
            break;

        case GST_STATE_CHANGE_READY_TO_NULL:
            disable_tunneled_ports (self);
            g_omx_core_deinit (self->gomx);
            if (self->gomx->omx_error)
                return GST_STATE_CHANGE_FAILURE;
            break;

        default:
            break;
    }
    GST_LOG_OBJECT (self, "end");

    return ret;
}

static void
dispose (GObject *obj)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    g_omx_core_free (self->gomx);

    g_free (self->omx_component);
    g_free (self->omx_library);

    G_OBJECT_CLASS (parent_class)->dispose (obj);
}

static void
set_property (GObject *obj,
              guint prop_id,
              const GValue *value,
              GParamSpec *pspec)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    switch (prop_id)
    {
        case ARG_COMPONENT_NAME:
            if (self->omx_component)
            {
                g_free (self->omx_component);
            }
            self->omx_component = g_value_dup_string (value);
            break;
        case ARG_LIBRARY_NAME:
            if (self->omx_library)
            {
                g_free (self->omx_library);
            }
            self->omx_library = g_value_dup_string (value);
            break;
        case ARG_USE_TIMESTAMPS:
            self->use_timestamps = g_value_get_boolean (value);
            break;
        case ARG_TUNNELING:
            self->allow_omx_tunnel = g_value_get_boolean (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
get_property (GObject *obj,
              guint prop_id,
              GValue *value,
              GParamSpec *pspec)
{
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (obj);

    switch (prop_id)
    {
        case ARG_COMPONENT_NAME:
            g_value_set_string (value, self->omx_component);
            break;
        case ARG_LIBRARY_NAME:
            g_value_set_string (value, self->omx_library);
            break;
        case ARG_USE_TIMESTAMPS:
            g_value_set_boolean (value, self->use_timestamps);
            break;
        case ARG_TUNNELING:
            g_value_set_boolean (value, self->allow_omx_tunnel);
            break;
        case ARG_GOMX_CORE:
            g_value_set_pointer (value, self->gomx);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
            break;
    }
}

static void
type_class_init (gpointer g_class,
                 gpointer class_data)
{
    GObjectClass *gobject_class;
    GstElementClass *gstelement_class;

    gobject_class = G_OBJECT_CLASS (g_class);
    gstelement_class = GST_ELEMENT_CLASS (g_class);

    parent_class = g_type_class_ref (GST_TYPE_ELEMENT);

    gobject_class->dispose = dispose;
    gstelement_class->change_state = change_state;

    /* Properties stuff */
    {
        gobject_class->set_property = set_property;
        gobject_class->get_property = get_property;

        g_object_class_install_property (gobject_class, ARG_COMPONENT_NAME,
                                         g_param_spec_string ("component-name", "Component name",
                                                              "Name of the OpenMAX IL component to use",
                                                              NULL, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_LIBRARY_NAME,
                                         g_param_spec_string ("library-name", "Library name",
                                                              "Name of the OpenMAX IL implementation library to use",
                                                              NULL, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_USE_TIMESTAMPS,
                                         g_param_spec_boolean ("use-timestamps", "Use timestamps",
                                                               "Whether or not to use timestamps",
                                                               TRUE, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_TUNNELING,
                                         g_param_spec_boolean ("tunneling", "Allow tunneling",
                                                               "Allow setting up an openmax tunnel with another element",
                                                               TRUE, G_PARAM_READWRITE));

        g_object_class_install_property (gobject_class, ARG_GOMX_CORE,
                                         g_param_spec_pointer ("core-pointer", "GOmx Core Pointer",
                                                               "Pointer used for tunneling",
                                                               G_PARAM_READABLE));
    }
}

static inline GstFlowReturn
push_buffer (GstOmxBaseFilter *self,
             GstBuffer *buf)
{
    GstFlowReturn ret;

    /** @todo check if tainted */
    GST_LOG_OBJECT (self, "begin");
    ret = gst_pad_push (self->srcpad, buf);
    GST_LOG_OBJECT (self, "end");

    return ret;
}

static void
output_loop (gpointer data)
{
    GstPad *pad;
    GOmxCore *gomx;
    GOmxPort *out_port;
    GstOmxBaseFilter *self;
    GstFlowReturn ret = GST_FLOW_OK;

    pad = data;
    self = GST_OMX_BASE_FILTER (gst_pad_get_parent (pad));
    gomx = self->gomx;

    GST_LOG_OBJECT (self, "begin");

    out_port = self->out_port;

    if (G_LIKELY (out_port->enabled))
    {
        OMX_BUFFERHEADERTYPE *omx_buffer;

        GST_LOG_OBJECT (self, "request buffer");
        omx_buffer = g_omx_port_request_buffer (out_port);

        GST_LOG_OBJECT (self, "omx_buffer: %p", omx_buffer);

        if (G_UNLIKELY (!omx_buffer))
        {
            GST_WARNING_OBJECT (self, "null buffer: leaving");
            goto leave;
        }

        GST_DEBUG_OBJECT (self, "omx_buffer: size=%lu, len=%lu, flags=%lu, offset=%lu, timestamp=%lld",
                          omx_buffer->nAllocLen, omx_buffer->nFilledLen, omx_buffer->nFlags,
                          omx_buffer->nOffset, omx_buffer->nTimeStamp);

        if (G_LIKELY (omx_buffer->nFilledLen > 0))
        {
            GstBuffer *buf;

#if 1
            /** @todo remove this check */
            if (G_LIKELY (self->in_port->enabled))
            {
                GstCaps *caps = NULL;

                caps = gst_pad_get_negotiated_caps (self->srcpad);

                if (!caps)
                {
                    /** @todo We shouldn't be doing this. */
                    GST_WARNING_OBJECT (self, "faking settings changed notification");
                    if (gomx->settings_changed_cb)
                        gomx->settings_changed_cb (gomx);
                }
                else
                {
                    GST_LOG_OBJECT (self, "caps already fixed: %" GST_PTR_FORMAT, caps);
                    gst_caps_unref (caps);
                }
            }
#endif

            /* buf is always null when the output buffer pointer isn't shared. */
            buf = omx_buffer->pAppPrivate;

            if (buf && !(omx_buffer->nFlags & OMX_BUFFERFLAG_EOS))
            {
                GST_BUFFER_SIZE (buf) = omx_buffer->nFilledLen;
                if (self->use_timestamps)
                {
                    GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale_int (omx_buffer->nTimeStamp,
                                                                            GST_SECOND,
                                                                            OMX_TICKS_PER_SECOND);
                }

                omx_buffer->pAppPrivate = NULL;
                omx_buffer->pBuffer = NULL;
                omx_buffer->nFilledLen = 0;

                ret = push_buffer (self, buf);

                gst_buffer_unref (buf);
            }
            else
            {
                /* This is only meant for the first OpenMAX buffers,
                 * which need to be pre-allocated. */
                /* Also for the very last one. */
                gst_pad_alloc_buffer_and_set_caps (self->srcpad,
                                                   GST_BUFFER_OFFSET_NONE,
                                                   omx_buffer->nFilledLen,
                                                   GST_PAD_CAPS (self->srcpad),
                                                   &buf);

                if (G_LIKELY (buf))
                {
                    memcpy (GST_BUFFER_DATA (buf), omx_buffer->pBuffer + omx_buffer->nOffset, omx_buffer->nFilledLen);
                    if (self->use_timestamps)
                    {
                        GST_BUFFER_TIMESTAMP (buf) = gst_util_uint64_scale (omx_buffer->nTimeStamp,
                                                                            GST_SECOND,
                                                                            OMX_TICKS_PER_SECOND);
                    }

                    omx_buffer->nFilledLen = 0;

                    if (share_output_buffer)
                    {
                        GST_WARNING_OBJECT (self, "couldn't zero-copy");
                        g_free (omx_buffer->pBuffer);
                        omx_buffer->pBuffer = NULL;
                    }

                    ret = push_buffer (self, buf);
                }
                else
                {
                    GST_WARNING_OBJECT (self, "couldn't allocate buffer of size %d",
                                        omx_buffer->nFilledLen);
                }
            }
        }
        else
        {
            GST_WARNING_OBJECT (self, "empty buffer");
        }

        if (G_UNLIKELY (ret != GST_FLOW_OK))
            goto leave;

        if (G_UNLIKELY (omx_buffer->nFlags & OMX_BUFFERFLAG_EOS))
        {
            GST_DEBUG_OBJECT (self, "got eos");
            g_omx_core_set_done (gomx);
            goto leave;
        }

        if (share_output_buffer &&
            !omx_buffer->pBuffer &&
            omx_buffer->nOffset == 0)
        {
            GstBuffer *buf;
            GstFlowReturn result;

            GST_LOG_OBJECT (self, "allocate buffer");
            result = gst_pad_alloc_buffer_and_set_caps (self->srcpad,
                                                        GST_BUFFER_OFFSET_NONE,
                                                        omx_buffer->nAllocLen,
                                                        GST_PAD_CAPS (self->srcpad),
                                                        &buf);

            if (G_LIKELY (result == GST_FLOW_OK))
            {
                gst_buffer_ref (buf);
                omx_buffer->pAppPrivate = buf;

                omx_buffer->pBuffer = GST_BUFFER_DATA (buf);
                omx_buffer->nAllocLen = GST_BUFFER_SIZE (buf);
            }
            else
            {
                GST_WARNING_OBJECT (self, "could not pad allocate buffer, using malloc");
                omx_buffer->pBuffer = g_malloc (omx_buffer->nAllocLen);
            }
        }

        if (share_output_buffer &&
            !omx_buffer->pBuffer)
        {
            GST_WARNING_OBJECT (self, "no input buffer to share");
        }

        GST_LOG_OBJECT (self, "release_buffer");
        g_omx_port_release_buffer (out_port, omx_buffer);
    }

    self->last_pad_push_return = ret;

leave:

    if (ret != GST_FLOW_OK)
    {
        GST_INFO_OBJECT (self, "pause task, reason:  %s",
                         gst_flow_get_name (self->last_pad_push_return));
        gst_pad_pause_task (self->srcpad);
    }

    GST_LOG_OBJECT (self, "end");

    gst_object_unref (self);
}

static GstPad *
find_peer_of_proxypad (GstPad *peer)
{
    GstPad *ghostpad;
    GstPad *peerofghostpad;
    GstPad *targetpad;

    ghostpad = (GstPad *) gst_pad_get_parent (peer);
    peerofghostpad = gst_pad_get_peer (ghostpad);
    gst_object_unref (ghostpad);

    if (GST_IS_GHOST_PAD (peerofghostpad))
    {
        targetpad = gst_ghost_pad_get_target ((GstGhostPad *) peerofghostpad);
        gst_object_unref (peerofghostpad);
    }
    else
    {
        targetpad = peerofghostpad;
    }

    return targetpad;
}

static GstPad *
find_real_peer (GstPad *pad,
                GstPad *peer)
{
    GstPad *real_peerpad;
    GstElement *gpeerelement;
    GstElement *gthiselement;

    gpeerelement = gst_pad_get_parent_element (peer);
    gthiselement = gst_pad_get_parent_element (pad);

    if (GST_IS_GHOST_PAD (peer))
    {
        real_peerpad = gst_ghost_pad_get_target ((GstGhostPad *) peer);
        if (!real_peerpad || (real_peerpad == pad))
        {
            return NULL;
        }
    }
    else if (!gpeerelement)
    {
        /* we assume that if the peer has no parent, the peer is a proxy pad */
        real_peerpad = find_peer_of_proxypad (peer);
        if (!real_peerpad)
        {
            return NULL;
        }
    }
    else
    {
        real_peerpad = peer;
        gst_object_ref (real_peerpad);
    }

    return real_peerpad;
}

static GstPadLinkReturn
pad_link  (GstPad *pad,
           GstPad *peer,
           GOmxPortType port_type)
{
    GOmxCore *gomx;
    GOmxPort *port;
    GstOmxBaseFilter *self;
    GObject *gpeerobject;
    GstElement* gpeerelement;
    GstPad *peerpad;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    gomx = self->gomx;

    if (port_type == GOMX_PORT_OUTPUT)
        port = self->out_port;
    else
        port = self->in_port;

    if (port->tunneled)
        return GST_PAD_LINK_OK; /* nothing to do, tunnel already linked */

    peerpad = find_real_peer (pad, peer);
    if (!peerpad)
        return GST_PAD_LINK_OK;

    gpeerelement = gst_pad_get_parent_element (peerpad);

    port->linked = TRUE;

    gpeerobject = G_OBJECT (gpeerelement);
    GObjectClass *g_object_class = G_OBJECT_GET_CLASS (gpeerobject);

    GParamSpec *spec;
    spec = g_object_class_find_property (g_object_class, "Allow-omx-tunnel");
    if (spec)
    {
        if (spec->value_type == G_TYPE_BOOLEAN)
        {
            GValue tunnelAllowed = { 0, };
            g_value_init (&tunnelAllowed, G_TYPE_BOOLEAN);
            g_object_get_property (G_OBJECT (gpeerelement), "Allow-omx-tunnel", &tunnelAllowed);

            spec = g_object_class_find_property (g_object_class, "GOMX-Core-Pointer");

            if (spec &&
                (g_value_get_boolean (&tunnelAllowed) == TRUE) &&
                (self->allow_omx_tunnel == TRUE))
            {
                if (spec->value_type == G_TYPE_POINTER)
                {
                    GValue value = { 0, };
                    GOmxCore *gpeeromx;
                    GOmxPadData *paddata;
                    g_value_init (&value, G_TYPE_POINTER);
                    OMX_ERRORTYPE retval;
                    g_object_get_property (G_OBJECT (gpeerelement), "GOMX-Core-Pointer", &value);

                    gpeeromx = g_value_get_pointer (&value);

                    /* port might already be prepared for non-tunneled communication */
                    g_omx_core_release_buffer (gomx, port);

                    retval = OMX_SendCommand (gomx->omx_handle, OMX_CommandPortDisable, port->port_index, NULL);
                    if (retval != OMX_ErrorNone)
                    {
                        gst_object_unref (gpeerelement);
                        gst_object_unref (peerpad);
                        return GST_PAD_LINK_REFUSED;
                    }

                    port->enabled = FALSE;
                    g_omx_sem_down (gomx->port_state_sem);

                    if (port_type == GOMX_PORT_OUTPUT)
                        self->srcpad_data.setting_tunnel = TRUE;
                    else
                        self->sinkpad_data.setting_tunnel = TRUE;

                    /* check if the other OMX component invoked us, or if we
                     * need to invoke him */
                    paddata = (GOmxPadData*) gst_pad_get_element_private (peerpad);
                    if (paddata->setting_tunnel == TRUE)
                    {
                        if (!port->tunneled)
                        {
                            if (port_type == GOMX_PORT_OUTPUT)
                                g_omx_core_setup_tunnel (gomx, gpeeromx);
                            else
                                g_omx_core_setup_tunnel (gpeeromx, gomx);
                        }
                    }
                    else if (peer->linkfunc)
                    {
                        peerpad->linkfunc (peerpad, pad);
                    }

                    if (port_type == GOMX_PORT_OUTPUT)
                        self->srcpad_data.setting_tunnel = FALSE;
                    else
                        self->sinkpad_data.setting_tunnel = FALSE;

                    if (gomx->omx_state == OMX_StateExecuting)
                        enable_tunneled_ports (self);
                }
            }
        }
    }

    gst_object_unref (gpeerelement);
    gst_object_unref (peerpad);

    return GST_PAD_LINK_OK;
}

static GstPadLinkReturn
pad_source_link (GstPad *pad,
                 GstPad *peer)
{
    return pad_link (pad, peer, GOMX_PORT_OUTPUT);
}

static GstPadLinkReturn
pad_sink_link (GstPad *pad,
               GstPad *peer)
{
    return pad_link (pad, peer, GOMX_PORT_INPUT);
}

static void
pad_unlink (GstPad *pad)
{
    GstOmxBaseFilter *self;
    GOmxPort *in_port;
    GOmxPort *out_port;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    in_port = self->in_port;
    out_port = self->out_port;

    if (pad == self->sinkpad)
    {
        in_port->tunneled = FALSE;
        in_port->linked = FALSE;
    }
    else if (pad == self->srcpad)
    {
        out_port->tunneled = FALSE;
        out_port->linked = FALSE;
    }
}

static GstFlowReturn
pad_chain (GstPad *pad,
           GstBuffer *buf)
{
    GOmxCore *gomx;
    GOmxPort *in_port;
    GstOmxBaseFilter *self;
    GstFlowReturn ret = GST_FLOW_OK;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));

    gomx = self->gomx;

    GST_LOG_OBJECT (self, "begin");
    GST_LOG_OBJECT (self, "gst_buffer: size=%lu", GST_BUFFER_SIZE (buf));

    GST_LOG_OBJECT (self, "state: %d", gomx->omx_state);

    in_port = self->in_port;

    if (G_UNLIKELY (in_port->tunneled))
        return GST_FLOW_OK;

    if (G_LIKELY (in_port->enabled))
    {
        guint buffer_offset = 0;

        while (G_LIKELY (buffer_offset < GST_BUFFER_SIZE (buf)))
        {
            OMX_BUFFERHEADERTYPE *omx_buffer;

            if (self->last_pad_push_return != GST_FLOW_OK)
            {
                goto out_flushing;
            }

            GST_LOG_OBJECT (self, "request buffer");
            omx_buffer = g_omx_port_request_buffer (in_port);

            GST_LOG_OBJECT (self, "omx_buffer: %p", omx_buffer);

            if (G_LIKELY (omx_buffer))
            {
                GST_DEBUG_OBJECT (self, "omx_buffer: size=%lu, len=%lu, flags=%lu, offset=%lu, timestamp=%lld",
                                  omx_buffer->nAllocLen, omx_buffer->nFilledLen, omx_buffer->nFlags,
                                  omx_buffer->nOffset, omx_buffer->nTimeStamp);

                if (omx_buffer->nOffset == 0 &&
                    share_input_buffer)
                {
                    {
                        GstBuffer *old_buf;
                        old_buf = omx_buffer->pAppPrivate;

                        if (old_buf)
                        {
                            gst_buffer_unref (old_buf);
                        }
                        else if (omx_buffer->pBuffer)
                        {
                            g_free (omx_buffer->pBuffer);
                        }
                    }

                    omx_buffer->pBuffer = GST_BUFFER_DATA (buf);
                    omx_buffer->nAllocLen = GST_BUFFER_SIZE (buf);
                    omx_buffer->nFilledLen = GST_BUFFER_SIZE (buf);
                    omx_buffer->pAppPrivate = buf;
                }
                else
                {
                    omx_buffer->nFilledLen = MIN (GST_BUFFER_SIZE (buf) - buffer_offset,
                                                  omx_buffer->nAllocLen - omx_buffer->nOffset);
                    memcpy (omx_buffer->pBuffer + omx_buffer->nOffset, GST_BUFFER_DATA (buf) + buffer_offset, omx_buffer->nFilledLen);
                }

                if (self->use_timestamps)
                {
                    omx_buffer->nTimeStamp = gst_util_uint64_scale_int (GST_BUFFER_TIMESTAMP (buf),
                                                                        OMX_TICKS_PER_SECOND,
                                                                        GST_SECOND);
                }

                buffer_offset += omx_buffer->nFilledLen;

                GST_LOG_OBJECT (self, "release_buffer");
                /** @todo untaint buffer */
                g_omx_port_release_buffer (in_port, omx_buffer);
            }
            else
            {
                GST_WARNING_OBJECT (self, "null buffer");
                goto out_flushing;
            }
        }
    }
    else
    {
        GST_WARNING_OBJECT (self, "done");
        ret = GST_FLOW_UNEXPECTED;
    }

    if (!share_input_buffer)
    {
        gst_buffer_unref (buf);
    }

    GST_LOG_OBJECT (self, "end");

    return ret;

    /* special conditions */
out_flushing:
    {
        gst_buffer_unref (buf);
        return self->last_pad_push_return;
    }
}


static gboolean
send_disable_event (GstOmxBaseFilter *self,
                    GstPad *pad)
{
    gboolean ret = FALSE;
    GstStructure *structure;
    GstEvent *event;
    GstPad *peerpad;

    if (pad == self->sinkpad)
    {
        structure = gst_structure_empty_new ("DisableOutPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, structure);
    }
    else if (pad == self->srcpad)
    {
        structure = gst_structure_empty_new ("DisableInPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM_OOB, structure);
    }
    else
    {
        return FALSE;
    }

    GST_DEBUG_OBJECT (self, "structure name = %s",
                      gst_structure_get_name (structure));
    peerpad = find_real_peer (pad, pad->peer);

    if (peerpad)
    {
        /* events are not handled by peer when it is in READY state, so we call its
         * eventhandler directly */
        peerpad->eventfunc (peerpad, event);
        gst_object_unref (peerpad);
        ret = TRUE;
    }

    return ret;
}

static gboolean
send_enable_event (GstOmxBaseFilter *self,
                   GstPad *pad)
{
    GstStructure *structure;
    GstEvent *event;

    if (pad == self->sinkpad)
    {
        structure = gst_structure_empty_new ("EnableOutPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, structure);
    }
    else if (pad == self->srcpad)
    {
        structure = gst_structure_empty_new ("EnableInPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM_OOB, structure);
    }
    else
    {
        return FALSE;
    }

    return gst_pad_push_event (pad, event);
}

static gboolean
pad_event (GstPad *pad,
           GstEvent *event)
{
    GstOmxBaseFilter *self;
    GOmxCore *gomx;
    gboolean ret = TRUE;
    GOmxPort *in_port;
    GOmxPort *out_port;
    OMX_ERRORTYPE retval;

    self = GST_OMX_BASE_FILTER (GST_OBJECT_PARENT (pad));
    gomx = self->gomx;
    in_port = self->in_port;
    out_port = self->out_port;

    GST_LOG_OBJECT (self, "begin");

    GST_INFO_OBJECT (self, "event: %s", GST_EVENT_TYPE_NAME (event));

    switch (GST_EVENT_TYPE (event))
    {
        case GST_EVENT_EOS:
            {
                GOmxCore *gomx;

                gomx = self->gomx;

                /* send buffer with eos flag */
                /** @todo move to util */
                {
                    OMX_BUFFERHEADERTYPE *omx_buffer;

                    GST_LOG_OBJECT (self, "request buffer");
                    omx_buffer = g_omx_port_request_buffer (self->in_port);

                    omx_buffer->nFlags |= OMX_BUFFERFLAG_EOS;

                    GST_LOG_OBJECT (self, "release_buffer");
                    /* foo_buffer_untaint (omx_buffer); */
                    g_omx_port_release_buffer (self->in_port, omx_buffer);
                }

                /* Wait for the output port to get the EOS. */
                g_omx_core_wait_for_done (gomx);
            }

            ret = gst_pad_push_event (self->srcpad, event);
            break;

        case GST_EVENT_FLUSH_START:
            /* unlock loops */
            g_omx_port_disable (self->in_port);
            g_omx_port_disable (self->out_port);

            gst_pad_pause_task (self->srcpad);

            /* flush all buffers */
            OMX_SendCommand (self->gomx->omx_handle, OMX_CommandFlush, OMX_ALL, NULL);

            ret = gst_pad_push_event (self->srcpad, event);
            break;

        case GST_EVENT_FLUSH_STOP:
            ret = gst_pad_push_event (self->srcpad, event);
            self->last_pad_push_return = GST_FLOW_OK;

            g_omx_sem_down (self->gomx->flush_sem);

            gst_pad_start_task (self->srcpad, output_loop, self->srcpad);

            g_omx_port_enable (self->in_port);
            g_omx_port_enable (self->out_port);

            break;

        case GST_EVENT_NEWSEGMENT:
            ret = gst_pad_push_event (self->srcpad, event);
            break;

        case GST_EVENT_CUSTOM_DOWNSTREAM_OOB:
            if (strcmp (gst_structure_get_name (event->structure), "DisableInPort") == 0)
            {
                OMX_SendCommand (gomx->omx_handle, OMX_CommandFlush, out_port->port_index, NULL);
                g_omx_sem_down (gomx->port_state_sem);

                disable_tunneled_port (self, out_port, self->srcpad);

                OMX_SendCommand (gomx->omx_handle, OMX_CommandPortDisable, in_port->port_index, NULL);
                in_port->enabled = FALSE;
                g_omx_sem_down (gomx->port_state_sem);
            }
            else if (strcmp (gst_structure_get_name (event->structure), "EnableInPort") == 0)
            {
                enable_tunneled_port (self, out_port, self->srcpad);

                OMX_SendCommand (gomx->omx_handle, OMX_CommandPortEnable, in_port->port_index, NULL);
                in_port->enabled = TRUE;
                g_omx_sem_down (gomx->port_state_sem);
            }
            break;

        case GST_EVENT_CUSTOM_UPSTREAM:
            if (strcmp (gst_structure_get_name (event->structure), "DisableOutPort") == 0)
            {
                if (gomx->omx_state != OMX_StateExecuting)
                {
                    retval = OMX_SendCommand (gomx->omx_handle, OMX_CommandFlush, out_port->port_index, NULL);
                    g_omx_sem_down (gomx->port_state_sem);
                }
                disable_tunneled_port (self, in_port, self->sinkpad);

                OMX_SendCommand (gomx->omx_handle, OMX_CommandPortDisable, out_port->port_index, NULL);
                out_port->enabled = FALSE;
                g_omx_sem_down (gomx->port_state_sem);
            }
            else if (strcmp (gst_structure_get_name (event->structure), "EnableOutPort") == 0)
            {
                enable_tunneled_port (self, in_port, self->sinkpad);

                OMX_SendCommand (gomx->omx_handle, OMX_CommandPortEnable, out_port->port_index, NULL);
                out_port->enabled = TRUE;
                g_omx_sem_down (gomx->port_state_sem);
            }
            break;

        default:
            ret = gst_pad_push_event (self->srcpad, event);
            break;
    }

    GST_LOG_OBJECT (self, "end");

    return ret;
}

static gboolean
activate_push (GstPad *pad,
               gboolean active)
{
    gboolean result = TRUE;
    GstOmxBaseFilter *self;

    self = GST_OMX_BASE_FILTER (gst_pad_get_parent (pad));

    if (active)
    {
        GST_DEBUG_OBJECT (self, "activate");
        self->last_pad_push_return = GST_FLOW_OK;

        /* we do not start the task yet if the pad is not connected */
        if (gst_pad_is_linked (pad))
        {
            /** @todo link callback function also needed */
            g_omx_port_enable (self->in_port);
            g_omx_port_enable (self->out_port);

            result = gst_pad_start_task (pad, output_loop, pad);
        }
    }
    else
    {
        GST_DEBUG_OBJECT (self, "deactivate");

        /* flush all buffers */
        OMX_SendCommand (self->gomx->omx_handle, OMX_CommandFlush, OMX_ALL, NULL);

        /* unlock loops */
        g_omx_port_disable (self->in_port);
        g_omx_port_disable (self->out_port);

        /* make sure streaming finishes */
        result = gst_pad_stop_task (pad);
    }

    gst_object_unref (self);

    return result;
}

static void
type_instance_init (GTypeInstance *instance,
                    gpointer g_class)
{
    GstOmxBaseFilter *self;
    GstElementClass *element_class;

    element_class = GST_ELEMENT_CLASS (g_class);

    self = GST_OMX_BASE_FILTER (instance);

    GST_LOG_OBJECT (self, "begin");

    self->use_timestamps = TRUE;
    self->allow_omx_tunnel = TRUE;

    /* GOmx */
    {
        GOmxCore *gomx;
        self->gomx = gomx = g_omx_core_new ();
        gomx->client_data = self;
    }

    self->sinkpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template (element_class, "sink"), "sink");

    gst_pad_set_chain_function (self->sinkpad, pad_chain);
    gst_pad_set_event_function (self->sinkpad, pad_event);
    /* note that when linking against ghost-pads, the link function of the
     * ghost pad should call this link function when the ghost pad gets linked
     * to another ghost- or proxypad. */
    gst_pad_set_link_function (self->sinkpad, pad_sink_link);
    gst_pad_set_unlink_function (self->sinkpad, pad_unlink);
    gst_pad_set_element_private (self->sinkpad, &self->sinkpad_data);

    self->srcpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template (element_class, "src"), "src");
    self->srcpad_data.setting_tunnel = FALSE;

    gst_pad_set_activatepush_function (self->srcpad, activate_push);

    /* note that when linking against ghost-pads, the link function of the
     * ghost pad should call this link function when the ghost pad gets linked
     * to another ghost- or proxypad. */
    gst_pad_set_link_function (self->srcpad, pad_source_link);
    gst_pad_set_event_function (self->srcpad, pad_event);
    gst_pad_set_unlink_function (self->srcpad, pad_unlink);
    gst_pad_set_element_private (self->srcpad, &self->srcpad_data);
    gst_pad_use_fixed_caps (self->srcpad);

    gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
    gst_element_add_pad (GST_ELEMENT (self), self->srcpad);

    self->omx_library = g_strdup (DEFAULT_LIBRARY_NAME);

    GST_LOG_OBJECT (self, "end");
}

GType
gst_omx_base_filter_get_type (void)
{
    static GType type = 0;

    if (G_UNLIKELY (type == 0))
    {
        GTypeInfo *type_info;

        type_info = g_new0 (GTypeInfo, 1);
        type_info->class_size = sizeof (GstOmxBaseFilterClass);
        type_info->class_init = type_class_init;
        type_info->instance_size = sizeof (GstOmxBaseFilter);
        type_info->instance_init = type_instance_init;

        type = g_type_register_static (GST_TYPE_ELEMENT, "GstOmxBaseFilter", type_info, 0);

        g_free (type_info);
    }

    return type;
}
