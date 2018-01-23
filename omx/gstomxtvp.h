/*
 * Copyright (C) 2018 RidgeRun LLC
 *
 * Author: Jose Lopez <jose.lopez@ridgerun.com>
 *
 * Based on gstomx_tvp.h made by:
 *       David Soto <david.soto@ridgerun.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifndef __GST_OMXTVP_H__
#define __GST_OMXTVP_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>
#include "gstomx.h"

G_BEGIN_DECLS

#define GST_TYPE_OMXTVP \
  (gst_omxtvp_get_type())
#define GST_OMXTVP(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OMXTVP,GstOMXTvp))
#define GST_OMXTVP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OMXTVP,GstOMXTvpClass))
#define GST_IS_OMXTVP(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OMXTVP))
#define GST_IS_OMXTVP_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OMXTVP))

#define GST_OMX_TVP_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_OMXTVP,GstOMXTvpClass))

typedef struct _GstOMXTvp      GstOMXTvp;
typedef struct _GstOMXTvpClass GstOMXTvpClass;

struct _GstOMXTvp
{
  GstBaseTransform element;
  GstOMXComponent *comp;
  guint standard;
  gint scan_type;
  gboolean mode_configured;
};

struct _GstOMXTvpClass
{
  GstBaseTransformClass parent_class;
  GstOMXClassData cdata;
};

GType gst_omxtvp_get_type (void);

G_END_DECLS

#endif /* __GST_OMXTVP_H__ */
