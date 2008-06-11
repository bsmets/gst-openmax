/*
 * Copyright (C) 2007-2008 Nokia Corporation. All rights reserved.
 * Copyright (C) 2008 NXP. All rights reserved.
 *
 * Author: Felipe Contreras <felipe.contreras@nokia.com>
 * Contributor: Frederik Vernelen <frederik.vernelen@nxp.com>
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

#ifndef GSTOMX_BASE_FILTER_H
#define GSTOMX_BASE_FILTER_H

#include <gst/gst.h>

#include <config.h>

G_BEGIN_DECLS

#define GST_OMX_BASE_FILTER(obj) (GstOmxBaseFilter *) (obj)
#define GST_OMX_BASE_FILTER_TYPE (gst_omx_base_filter_get_type ())
#define GST_OMX_BASE_FILTER_CLASS(obj) (GstOmxBaseFilterClass *) (obj)

typedef struct GstOmxBaseFilter GstOmxBaseFilter;
typedef struct GstOmxBaseFilterClass GstOmxBaseFilterClass;
typedef void (*GstOmxBaseFilterCb) (GstOmxBaseFilter *self);

#include <gstomx_util.h>

struct GstOmxBaseFilter
{
    GstElement element;

    GstPad *sinkpad;
    GstPad *srcpad;
    GOmxPadData sinkpad_data;
    GOmxPadData srcpad_data;

    GOmxCore *gomx;
    GOmxPort *in_port;
    GOmxPort *out_port;

    GThread *thread;

    char *omx_component;
    char *omx_library;
    gboolean use_timestamps; /** @todo remove; timestamps should always be used */
    gboolean allow_omx_tunnel;
    gboolean initialized;

    GstOmxBaseFilterCb omx_setup;
};

struct GstOmxBaseFilterClass
{
    GstElementClass parent_class;
};

GType gst_omx_base_filter_get_type (void);

void gst_omx_base_filter_omx_init (GstOmxBaseFilter *self);

G_END_DECLS

#endif /* GSTOMX_BASE_FILTER_H */
