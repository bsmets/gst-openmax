/*
 * Copyright (C) 2007-2008 Nokia Corporation. All rights reserved.
 * Copyright (C) 2008 NXP. All rights reserved.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include "gstomx_base_audio_sink.h"
#include "gstomx.h"

static gboolean share_input_buffer = FALSE;
static gboolean share_output_buffer = FALSE;

static gboolean send_disable_event (GstOmxBaseAudioSink *self,GstPad *pad);
static gboolean send_enable_event (GstOmxBaseAudioSink *self,GstPad *pad);

enum
{
    ARG_0,
    ARG_COMPONENT_NAME,
    ARG_LIBRARY_NAME,
    ARG_USE_TIMESTAMPS,
    ARG_ALLOW_OMX_TUNNEL,
    ARG_GOMX_CORE
};

static GstElementClass *parent_class = NULL;


static void
setup_ports (GstOmxBaseAudioSink *self)
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
    self->in_port->done = FALSE;

    free (param);
}

static void
disable_tunneled_ports (GstOmxBaseAudioSink *self)
{
    GOmxCore *gomx;
    gomx = self->gomx;
    OMX_ERRORTYPE omx_error = OMX_ErrorNone;

    GOmxPort *in_port = self->in_port;

    if((in_port->tunneled) && (in_port->enabled))
    {
        omx_error = OMX_SendCommand( gomx->omx_handle, OMX_CommandFlush, in_port->port_index, NULL);    
        g_omx_sem_down (gomx->port_state_sem);

        omx_error = OMX_SendCommand( gomx->omx_handle, OMX_CommandPortDisable, in_port->port_index, NULL);

        send_disable_event (self,self->sinkpad); 
        in_port->enabled = FALSE;

        g_omx_sem_down (gomx->port_state_sem);
    }
}

static void
enable_tunneled_ports (GstOmxBaseAudioSink *self)
{
    GOmxCore *gomx;
    gomx = self->gomx;
    OMX_ERRORTYPE omx_error = OMX_ErrorNone;
    GOmxPort *in_port = self->in_port;

    if((in_port->tunneled) && (!(in_port->enabled)))
    {
        omx_error = OMX_SendCommand( gomx->omx_handle, OMX_CommandPortEnable, in_port->port_index, NULL);

        send_enable_event (self,self->sinkpad);
        in_port->enabled = TRUE;

        g_omx_sem_down (gomx->port_state_sem);
    }
}

static GstStateChangeReturn
change_state (GstElement *element,
              GstStateChange transition)
{
    OMX_ERRORTYPE omx_error = OMX_ErrorNone;
    GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
    GstOmxBaseAudioSink *self;
    GOmxCore *gomx;
    GOmxPort *in_port;

    self = GST_OMX_BASE_AUDIO_SINK (element);
    gomx = self->gomx;
    in_port = self->in_port;

    GST_LOG_OBJECT (self, "begin");

    GST_INFO_OBJECT (self, "changing state %s - %s",
                     gst_element_state_get_name (GST_STATE_TRANSITION_CURRENT (transition)),
                     gst_element_state_get_name (GST_STATE_TRANSITION_NEXT (transition)));

    switch (transition)
    {
        case GST_STATE_CHANGE_NULL_TO_READY:
            if (self->initialized == FALSE)
            {
                gst_omx_base_audio_sink_omx_init (self);
            }
            omx_error = OMX_SendCommand (gomx->omx_handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
            g_omx_core_prepare (self->gomx);

            g_omx_sem_down (gomx->state_sem);
            if (self->gomx->omx_error || (omx_error != OMX_ErrorNone))
            {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_READY_TO_PAUSED:

            enable_tunneled_ports(self);
            omx_error = OMX_SendCommand(gomx->omx_handle, OMX_CommandStateSet,OMX_StatePause, NULL);
            g_omx_sem_down (gomx->state_sem);
            if (omx_error != OMX_ErrorNone)
            {
              return GST_STATE_CHANGE_FAILURE;
            }
            break;
        case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
            //we should return GST_STATE_CHANGE_ASYNC here and when the first buffer arrives complete the state change
            omx_error = OMX_SendCommand (gomx->omx_handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
            g_omx_sem_down (gomx->state_sem);
            if (omx_error != OMX_ErrorNone)
            {
                return GST_STATE_CHANGE_FAILURE;
            }

            g_omx_core_start (gomx);
            if(self->gomx->omx_error)
            {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;
        default:
            break;
    }

    ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        return ret;
    }
    switch (transition)
    {
        case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
            omx_error = OMX_SendCommand(gomx->omx_handle, OMX_CommandStateSet,OMX_StatePause, NULL);
            g_omx_sem_down (gomx->state_sem);
            if (omx_error != OMX_ErrorNone)
            {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;

        case GST_STATE_CHANGE_PAUSED_TO_READY:
            if (self->initialized)
            {
                g_omx_port_set_done (self->in_port);
            }
            omx_error = OMX_SendCommand (gomx->omx_handle, OMX_CommandStateSet, OMX_StateIdle, NULL);
            g_omx_sem_down (gomx->state_sem);
            if (omx_error != OMX_ErrorNone)
            {
                return GST_STATE_CHANGE_FAILURE;
            }
            break;

        case GST_STATE_CHANGE_READY_TO_NULL:

            disable_tunneled_ports(self);
            omx_error = OMX_SendCommand( gomx->omx_handle, OMX_CommandFlush, in_port->port_index, NULL);
            g_omx_sem_down (gomx->port_state_sem);
            if (omx_error != OMX_ErrorNone)
            {
              return GST_STATE_CHANGE_FAILURE;
            }

            OMX_SendCommand (gomx->omx_handle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
            g_omx_core_finish(gomx);
            g_omx_sem_down (gomx->state_sem);

            if (self->gomx->omx_error || (omx_error != OMX_ErrorNone))
            {
                g_print("GST_STATE_CHANGE_FAILURE\n");
                return GST_STATE_CHANGE_FAILURE;
            }

            g_omx_core_deinit (self->gomx);
            self->initialized = FALSE;
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
    GstOmxBaseAudioSink *self;

    self = GST_OMX_BASE_AUDIO_SINK (obj);

    g_omx_core_deinit (self->gomx);
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
    GstOmxBaseAudioSink *self;
    GOmxCore *gomx;

    self = GST_OMX_BASE_AUDIO_SINK (obj);
    gomx = self->gomx;

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
        case ARG_ALLOW_OMX_TUNNEL:
            self->allow_omx_tunnel = g_value_get_boolean (value);
            break;
        case ARG_GOMX_CORE:
            gomx = g_value_get_pointer (value);
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
    GstOmxBaseAudioSink *self;
    GOmxCore *gomx;

    self = GST_OMX_BASE_AUDIO_SINK (obj);
    gomx = self->gomx;

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
        case ARG_ALLOW_OMX_TUNNEL:
            g_value_set_boolean (value, self->allow_omx_tunnel);
            break;
        case ARG_GOMX_CORE:
            g_value_set_pointer (value, gomx);
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

    }
}

static GstPad*
find_peer_of_proxypad(GstPad *peer)
{
    GstPad* ghostpad;
    GstPad* peerofghostpad;
    GstPad* targetpad;

    ghostpad = (GstPad*)gst_pad_get_parent(peer);  
    peerofghostpad = gst_pad_get_peer (ghostpad);
    gst_object_unref(ghostpad);

    if(GST_IS_GHOST_PAD (peerofghostpad))
    {
        targetpad = gst_ghost_pad_get_target ((GstGhostPad *) peerofghostpad);
        gst_object_unref(peerofghostpad);
    }
    else
    {
        targetpad = peerofghostpad; 
    }
    return targetpad; 
}

static GstPad* 
find_real_peer (GstPad *pad,
                GstPad *peer)
{
    GstPad* real_peerpad;
    GstElement* gpeerelement;

    gpeerelement = gst_pad_get_parent_element(peer);

    if(GST_IS_GHOST_PAD (peer))
    {
        real_peerpad = gst_ghost_pad_get_target ((GstGhostPad *) peer);
        if( (real_peerpad == NULL)||(real_peerpad == pad) )
        {
            return NULL;
        }
    }
    else if(gpeerelement == NULL)
    {
        real_peerpad = find_peer_of_proxypad(peer);
        if(real_peerpad == NULL)
        {
            return NULL;
        }
    }
    else
    {
        real_peerpad = peer;
        gst_object_ref(real_peerpad);
    }
    return real_peerpad;
}

static GstPadLinkReturn
pad_sink_link  (GstPad *pad,
                GstPad *peer)
{
    bool isOMX = FALSE;
    GOmxCore *gomx;
    GOmxPort *in_port;
    GstOmxBaseAudioSink *self;
    GObject *gpeerobject;
    GstElement* gpeerelement;
    GValue value = { 0, };
    GstPad *peerpad;
    GstGhostPad *ghostpad;

    self = GST_OMX_BASE_AUDIO_SINK (GST_OBJECT_PARENT (pad));
    if(!self->initialized )
    {
        return GST_PAD_LINK_REFUSED;
    }
    gomx = self->gomx;
    in_port = self->in_port;

    if(in_port->tunneled == TRUE)
    {
        return GST_PAD_LINK_OK;//nothing to do , tunnel already linked
    }

    peerpad = find_real_peer(pad,peer);
    if(peerpad == NULL)
    {
        gst_object_unref(peerpad);
        return GST_PAD_LINK_OK;
    }

    gpeerelement = gst_pad_get_parent_element(peerpad);

    in_port->linked = TRUE;

    gpeerobject = G_OBJECT (gpeerelement);
    GObjectClass *g_object_class = G_OBJECT_GET_CLASS(gpeerobject);
    GParamSpec *spec;
    spec = g_object_class_find_property(g_object_class, "Allow-omx-tunnel");
    if(spec != NULL)
    {
        if(spec->value_type == G_TYPE_BOOLEAN)
        {
            GValue tunnelAllowed = { 0, };
            g_value_init (&tunnelAllowed, G_TYPE_BOOLEAN);
            g_object_get_property (G_OBJECT (gpeerelement), "Allow-omx-tunnel", &tunnelAllowed);

            spec = g_object_class_find_property(g_object_class, "GOMX-Core-Pointer");

            if( (spec != NULL) && (g_value_get_boolean (&tunnelAllowed) == TRUE) && (self->allow_omx_tunnel == TRUE) )
            {
                if(spec->value_type == G_TYPE_POINTER)
                {
                    GOmxCore *gpeeromx;
                    GOmxPadData *paddata;
                    g_value_init (&value, G_TYPE_POINTER);
                    OMX_ERRORTYPE retval;
                    g_object_get_property (G_OBJECT (gpeerelement), "GOMX-Core-Pointer", &value);

                    gpeeromx = g_value_get_pointer (&value);

                    //in_port might already be prepared for non-tunneled communication
                    g_omx_core_release_buffer(gomx,in_port);

                    retval = OMX_SendCommand( gomx->omx_handle, OMX_CommandPortDisable, in_port->port_index, NULL);
                    if(retval != OMX_ErrorNone)
                    {
                        gst_object_unref(gpeerelement);
                        gst_object_unref(peerpad);
                        return GST_PAD_LINK_REFUSED; 
                    }

                    in_port->enabled = FALSE;
                    g_omx_sem_down (gomx->port_state_sem);

                    (self->sinkpad_data).setting_tunnel = TRUE;
                    //check if the other OMX component invoked us, or if we need to invoke him
                    paddata = (GOmxPadData*)gst_pad_get_element_private (peerpad);

                    if(paddata->setting_tunnel == TRUE)
                    {
                        if(!(in_port->tunneled))
                        {
                            g_omx_core_setup_tunnel(gpeeromx, gomx);
                        }
                    }
                    else if(peerpad->linkfunc != NULL)
                    {
                        peerpad->linkfunc(peerpad,pad);
                    }
                    (self->sinkpad_data).setting_tunnel = FALSE;

                    if( (gomx->omx_state == OMX_StateExecuting) || (gomx->omx_state == OMX_StatePause) )
                    {
                        enable_tunneled_ports(self);
                    }
                }
            }
        }
    }
    gst_object_unref(gpeerelement);
    gst_object_unref(peerpad);

    return GST_PAD_LINK_OK;
}

static void
pad_unlink  (GstPad *pad)
{
    GstOmxBaseAudioSink *self;
    GOmxPort *in_port;

    self = GST_OMX_BASE_AUDIO_SINK (GST_OBJECT_PARENT (pad));

    in_port = self->in_port;
    if(!self->initialized )
    {
        return;
    }
    in_port->tunneled = FALSE;
    in_port->linked = FALSE;
}

static GstFlowReturn
pad_chain (GstPad *pad,
           GstBuffer *buf)
{
    GOmxCore *gomx;
    GOmxPort *in_port;
    GstOmxBaseAudioSink *self;
    GstFlowReturn ret = GST_FLOW_OK;

    self = GST_OMX_BASE_AUDIO_SINK (GST_OBJECT_PARENT (pad));

    gomx = self->gomx;

    GST_LOG_OBJECT (self, "begin");
    GST_LOG_OBJECT (self, "gst_buffer: size=%lu", GST_BUFFER_SIZE (buf));

    GST_LOG_OBJECT (self, "state: %d", gomx->omx_state);

    in_port = self->in_port;

    if (G_LIKELY (!in_port->done))
    {
        guint buffer_offset = 0;

        if (G_UNLIKELY (gomx->omx_state != OMX_StateExecuting))
        {
            GST_ERROR_OBJECT (self, "Whoa! very wrong");
        }

        while (G_LIKELY (buffer_offset < GST_BUFFER_SIZE (buf)))
        {
            OMX_BUFFERHEADERTYPE *omx_buffer;
            GST_LOG_OBJECT (self, "request buffer");
            omx_buffer = g_omx_port_request_buffer (in_port);
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

                GST_LOG_OBJECT (self, "release_buffer");
                g_omx_port_release_buffer (in_port, omx_buffer);

                buffer_offset += omx_buffer->nFilledLen;
            }
            else
            {
                GST_WARNING_OBJECT (self, "null buffer");
                /* ret = GST_FLOW_ERROR; */
                break;
            }
        }
    }
    else
    {
        GST_WARNING_OBJECT (self, "done");
        ret = GST_FLOW_UNEXPECTED;
    }

leave:

    if (!share_input_buffer)
    {
        gst_buffer_unref (buf);
    }

    GST_LOG_OBJECT (self, "end");

    return ret;
}

static gboolean
send_disable_event (GstOmxBaseAudioSink *self,GstPad *pad)
{
    gboolean ret;
    GstStructure *structure;
    GstEvent *event;
    GstPad *peerpad;

    if(pad == self->sinkpad)
    {
        structure = gst_structure_empty_new  ("DisableOutPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, structure);
    }
    else
    {
        return FALSE;
    }
    peerpad = find_real_peer (pad, pad->peer);
    if(peerpad != NULL)
    {
      //event are not handled by peer when it is in READY state, so we call its eventhandler directly
      //ret = gst_pad_push_event (peerpad, event);
        peerpad->eventfunc(peerpad,event );
        gst_object_unref(peerpad);
    }
    return ret;
}

static gboolean
send_enable_event (GstOmxBaseAudioSink *self,GstPad *pad)
{
    gboolean ret;
    GstStructure *structure;
    GstEvent *event;
    GstPad *peerpad;

    if(pad == self->sinkpad)
    {
        structure = gst_structure_empty_new  ("EnableOutPort");
        event = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, structure);
    }
    else
    {
        return FALSE;
    }

    ret = gst_pad_push_event (pad, event);
    return ret;
}

static gboolean
pad_event (GstPad *pad,
           GstEvent *event)
{
    GstOmxBaseAudioSink *self;
    GOmxCore *gomx;
    gboolean ret;
    GOmxPort *in_port;
    GstMessage *EOSmessage;

    self = GST_OMX_BASE_AUDIO_SINK (GST_OBJECT_PARENT (pad));
    gomx = self->gomx;
    in_port = self->in_port;

    GST_LOG_OBJECT (self, "begin");

    GST_DEBUG_OBJECT (self, "event: %s", GST_EVENT_TYPE_NAME (event));

    switch (GST_EVENT_TYPE (event))
    {
        case GST_EVENT_EOS:
            EOSmessage = gst_message_new_eos (GST_OBJECT(self));
            gst_element_post_message (GST_ELEMENT(self),EOSmessage);
            break;

        case GST_EVENT_FLUSH_START:
            OMX_SendCommand (self->gomx->omx_handle, OMX_CommandFlush, 0, NULL);
            g_omx_sem_down (gomx->port_state_sem);
            break;

        case GST_EVENT_CUSTOM_BOTH_OOB:
        case GST_EVENT_CUSTOM_DOWNSTREAM_OOB:
            if (strcmp(gst_structure_get_name (event->structure),"DisableInPort") == 0)
            {
                OMX_SendCommand( gomx->omx_handle, OMX_CommandPortDisable, in_port->port_index, NULL);
                in_port->enabled = FALSE;
                g_omx_sem_down (gomx->port_state_sem);
            }
            break;

        default:
            break;
    }
    GST_LOG_OBJECT (self, "end");

    return ret;
}

static void
event_handler_cb (GOmxCore *core,
                  OMX_EVENTTYPE eEvent,
                  OMX_U32 nData1,
                  OMX_U32 nData2,
                  OMX_PTR pEventData)
{
    GstOmxBaseAudioSink *self;
    self = GST_OMX_BASE_AUDIO_SINK (core->client_data);
    GOmxPort *in_port = self->in_port;

    switch (eEvent)
    {
        case OMX_EventBufferFlag:
            if(in_port != NULL)
            {
                if ( (nData2 & OMX_BUFFERFLAG_EOS) && (in_port->tunneled) )
                {
                    GstMessage *EOSmessage;
                    EOSmessage = gst_message_new_eos (GST_OBJECT(self));
                    gst_element_post_message (GST_ELEMENT(self),EOSmessage);
                }
            }
            break;
        default:
            break;
    }
}

static void
type_instance_init (GTypeInstance *instance,
                    gpointer g_class)
{
    GstOmxBaseAudioSink *self;
    GstElementClass *element_class;

    element_class = GST_ELEMENT_CLASS (g_class);

    self = GST_OMX_BASE_AUDIO_SINK (instance);

    GST_LOG_OBJECT (self, "begin");

    self->use_timestamps = TRUE;
    self->allow_omx_tunnel = TRUE;

    /* GOmx */
    {
        GOmxCore *gomx;
        self->gomx = gomx = g_omx_core_new ();
        gomx->client_data = self;
        gomx->event_handler_cb = event_handler_cb;
    }

    self->sinkpad =
        gst_pad_new_from_template (gst_element_class_get_pad_template (element_class, "sink"), "sink");

    (self->sinkpad_data).setting_tunnel = FALSE;

    gst_pad_set_chain_function (self->sinkpad, pad_chain);
    gst_pad_set_event_function (self->sinkpad, pad_event);

    gst_pad_set_element_private (self->sinkpad, &(self->sinkpad_data));

    gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

    self->omx_library = g_strdup (DEFAULT_LIBRARY_NAME);

    GST_LOG_OBJECT (self, "end");
}

void
gst_omx_base_audio_sink_omx_init (GstOmxBaseAudioSink *self)
{
    GOmxCore *gomx;

    GST_LOG_OBJECT (self, "begin");

    gomx = self->gomx;

    g_omx_core_init (self->gomx, self->omx_library, self->omx_component);

    gomx = self->gomx;

    GST_INFO_OBJECT (self, "omx: prepare");

    if (self->omx_setup)
    {
        self->omx_setup (self);
    }
    setup_ports (self);

    self->initialized = TRUE;

    GST_LOG_OBJECT (self, "end");
}



GType
gst_omx_base_audio_sink_get_type (void)
{
    static GType type = 0;

    if (G_UNLIKELY (type == 0))
    {
        GTypeInfo *type_info;

        type_info = g_new0 (GTypeInfo, 1);
        type_info->class_size = sizeof (GstOmxBaseAudioSinkClass);
        type_info->class_init = type_class_init;
        type_info->instance_size = sizeof (GstOmxBaseAudioSink);
        type_info->instance_init = type_instance_init;

        type = g_type_register_static (GST_TYPE_ELEMENT, "GstOmxBaseAudioSink", type_info, 0);

        g_free (type_info);
    }

    return type;
}
