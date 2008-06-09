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

#ifndef GSTOMX_BASE_AUDIO_SINK_H
#define GSTOMX_BASE_AUDIO_SINK_H

#include <gst/gst.h>

#include <config.h>

G_BEGIN_DECLS

#define GST_OMX_BASE_AUDIO_SINK(obj) (GstOmxBaseAudioSink *) (obj)
#define GST_OMX_BASE_AUDIO_SINK_TYPE (gst_omx_base_audio_sink_get_type ())
#define GST_OMX_BASE_AUDIO_SINK_CLASS(obj) (GstOmxBaseAudioSinkClass *) (obj)

typedef struct GstOmxBaseAudioSink GstOmxBaseAudioSink;
typedef struct GstOmxBaseAudioSinkClass GstOmxBaseAudioSinkClass;
typedef void (*GstOmxBaseAudioSinkCb) (GstOmxBaseAudioSink *self);

#include <gstomx_util.h>

struct GstOmxBaseAudioSink
{
    GstElement element;

    GstPad *sinkpad;
    GOmxPadData sinkpad_data;

    GOmxCore *gomx;
    GOmxPort *in_port;

    GThread *thread;

    char *omx_component;
    char *omx_library;
    gboolean use_timestamps; /** @todo remove; timestamps should always be used */
    gboolean allow_omx_tunnel;
    gboolean initialized;

    GstOmxBaseAudioSinkCb omx_setup;
};

struct GstOmxBaseAudioSinkClass
{
    GstElementClass parent_class;
};

GType gst_omx_base_audio_sink_get_type (void);

G_END_DECLS

#endif /* GSTOMX_BASE_AUDIO_SINK_H */
