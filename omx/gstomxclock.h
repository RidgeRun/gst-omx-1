/*
 * GStreamer
 * Copyright (C) 2016 Melissa Montero <melissa.montero@ridgerun.com>
 * Copyright (C) 2016 Michael Gruner <michael.gruner@ridgerun.com>
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

#ifndef __GST_OMX_CLOCK_H__
#define __GST_OMX_CLOCK_H__

#include <gst/gst.h>
#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
#include "gstomx.h"
#endif

G_BEGIN_DECLS
#define GST_TYPE_OMX_CLOCK \
  (gst_omx_clock_get_type())
#define GST_OMX_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OMX_CLOCK,GstOmxClock))
#define GST_OMX_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OMX_CLOCK,GstOmxClockClass))
#define GST_IS_OMX_CLOCK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OMX_CLOCK))
#define GST_IS_OMX_CLOCK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OMX_CLOCK))
typedef struct _GstOmxClock GstOmxClock;
typedef struct _GstOmxClockClass GstOmxClockClass;

struct _GstOmxClock
{
  GstSystemClock parent;
#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  GstOMXComponent *comp;
#else
  GstClockTime last_tick;
  GstClockTime last_time;
  GstClockTime sys_offset;
  GstClockTime last_time_sent;
  GstClockTimeDiff hw_offset;
  GstClockTime base;
  gboolean first_time;
#endif
};


struct _GstOmxClockClass
{
  GstSystemClockClass parent_class;
};

GType gst_omx_clock_get_type (void);
#ifndef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
void gst_omx_clock_new_tick (GstOmxClock * this, GstClockTime tick);
void gst_omx_clock_reset (GstOmxClock * this);
#endif

G_END_DECLS
#endif /* __GST_OMX_CLOCK_H__ */
