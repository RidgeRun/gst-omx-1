/*
 * GStreamer
 * Copyright (C) 2016 Melissa Montero <melissa.montero@ridgerun.com>
 * Copyright (C) 2016 Michael Gruner <michael.gruner@ridgerun.com
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "gstomxclock.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_clock_debug);
#define GST_CAT_DEFAULT gst_omx_clock_debug

#define gst_omx_clock_parent_class parent_class
G_DEFINE_TYPE (GstOmxClock, gst_omx_clock, GST_TYPE_SYSTEM_CLOCK);

/* object methods */
static GstClockTime gst_omx_clock_get_internal_time (GstClock * clock);
#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
static void gst_omx_clock_dispose (GObject * object);
#endif

static void
gst_omx_clock_class_init (GstOmxClockClass * klass)
{
#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
#endif
  GstClockClass *clock_class = GST_CLOCK_CLASS (klass);

#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  gobject_class->dispose = gst_omx_clock_dispose;
#endif

  clock_class->get_internal_time =
      GST_DEBUG_FUNCPTR (gst_omx_clock_get_internal_time);

  GST_DEBUG_CATEGORY_INIT (gst_omx_clock_debug, "omxclock", 0,
      "OMX Clock Source");
}

static void
gst_omx_clock_init (GstOmxClock * self)
{
  g_object_set (self, "clock-type", GST_CLOCK_TYPE_OTHER, NULL);
#ifndef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  self->last_tick = GST_CLOCK_TIME_NONE;
  self->last_time = GST_CLOCK_TIME_NONE;
  self->base = GST_CLOCK_TIME_NONE;
  self->last_time_sent = GST_CLOCK_TIME_NONE;
  self->hw_offset = 0;
  self->first_time = TRUE;
#else
  self->comp =
      gst_omx_component_new (GST_OBJECT_CAST (self), "/usr/lib/libOMX_Core.so",
      "OMX.TI.VPSSM3.VFCC", NULL, 0);
#endif
}

#ifndef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
static GstClockTime
gst_omx_clock_get_monotonic_time (void)
{
  GTimeVal now;
  gint64 monotonic;

  monotonic = g_get_monotonic_time ();
  now.tv_usec = monotonic % G_USEC_PER_SEC;
  now.tv_sec = monotonic / G_USEC_PER_SEC;

  return GST_TIMEVAL_TO_TIME (now);
}
#endif

static GstClockTime
gst_omx_clock_get_internal_time (GstClock * clock)
{
  GstOmxClock *self;
#ifndef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  GstClockTimeDiff offset;
#else
  OMX_TIME_CONFIG_TIMESTAMPTYPE omxtime;
  OMX_ERRORTYPE err;
#endif
  GstClockTime time = GST_CLOCK_TIME_NONE;

  g_return_val_if_fail (clock, GST_CLOCK_TIME_NONE);
  self = GST_OMX_CLOCK (clock);

#ifdef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
  GST_OMX_INIT_STRUCT (&omxtime);
  err =
      gst_omx_component_get_parameter (self->comp,
      OMX_IndexConfigTimeCurrentMediaTime, &omxtime);
  if (err == OMX_ErrorNone) {
    time = gst_util_uint64_scale (omxtime.nTimestamp, GST_SECOND,
        OMX_TICKS_PER_SECOND) * 1000;
  } else {
    GST_WARNING_OBJECT (self, "Couldn't get time information: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
  }
#else
  if (self->last_time == GST_CLOCK_TIME_NONE) {
    time = (GstClockTime) 0;
    self->last_tick = 0;
    self->last_time = gst_omx_clock_get_monotonic_time ();
  } else {
    offset =
        GST_CLOCK_DIFF (self->last_time, gst_omx_clock_get_monotonic_time ());
    time = self->last_tick + offset + self->hw_offset;
    self->last_time_sent = time;
  }
#endif

  GST_DEBUG_OBJECT (self, "time %" GST_TIME_FORMAT, GST_TIME_ARGS (time));
  return time;
}

#ifndef OMX_VFCC_PARAM_TIMESTAMP_INSTALLED
void
gst_omx_clock_new_tick (GstOmxClock * self, GstClockTime tick)
{
  GstClockTimeDiff offset;
  g_return_if_fail (self);
  GST_OBJECT_LOCK (self);

  if (GST_CLOCK_TIME_NONE == self->base) {
    self->base = tick;
    self->sys_offset =
        GST_CLOCK_DIFF (self->last_time, gst_omx_clock_get_monotonic_time ());
  }

  self->last_tick = tick - self->base + self->sys_offset;
  self->last_time = gst_omx_clock_get_monotonic_time ();

  /* Taking in consideration the difference between the hw clock and the system clock */
  if (self->first_time) {
    offset =
        GST_CLOCK_DIFF (self->last_time, gst_omx_clock_get_monotonic_time ());
    self->hw_offset =
        GST_CLOCK_DIFF (self->last_time_sent, self->last_tick + offset);
    GST_INFO_OBJECT (self,
        "hw clock and system clock Offset: %" GST_TIME_FORMAT,
        GST_TIME_ARGS (self->hw_offset));
    self->first_time = FALSE;
  }

  GST_DEBUG_OBJECT (self,
      "last tick %" GST_TIME_FORMAT ", last time %" GST_TIME_FORMAT,
      GST_TIME_ARGS (self->last_tick), GST_TIME_ARGS (self->last_time));

  GST_OBJECT_UNLOCK (self);
}

void
gst_omx_clock_reset (GstOmxClock * self)
{
  g_return_if_fail (self);
  GST_INFO_OBJECT (self, "Resetting clock");
  self->first_time = TRUE;
  self->hw_offset = 0;
}

#else
static void
gst_omx_clock_dispose (GObject * object)
{
  GstOmxClock *self = GST_OMX_CLOCK (object);

  if (self->comp) {
    gst_omx_component_free (self->comp);
    self->comp = NULL;
  }
}
#endif
