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

static void
gst_omx_clock_class_init (GstOmxClockClass * klass)
{
  GstClockClass *clock_class;

  clock_class = GST_CLOCK_CLASS (klass);

  clock_class->get_internal_time =
      GST_DEBUG_FUNCPTR (gst_omx_clock_get_internal_time);

  GST_DEBUG_CATEGORY_INIT (gst_omx_clock_debug, "omxclock", 0,
      "OMX Clock Source");
}

static void
gst_omx_clock_init (GstOmxClock * self)
{
  self->last_tick = GST_CLOCK_TIME_NONE;
  self->last_time = GST_CLOCK_TIME_NONE;
  self->base = GST_CLOCK_TIME_NONE;
  self->last_time_sent = GST_CLOCK_TIME_NONE;
  self->hw_offset = 0;
  self->first_time = TRUE;
}

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

static GstClockTime
gst_omx_clock_get_internal_time (GstClock * clock)
{
  GstOmxClock *self;
  GstClockTimeDiff offset;
  GstClockTime time;

  self = GST_OMX_CLOCK (clock);

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

  GST_DEBUG_OBJECT (self, "time %" GST_TIME_FORMAT, GST_TIME_ARGS (time));
  return time;
}

void
gst_omx_clock_new_tick (GstOmxClock * self, GstClockTime tick)
{
  GstClockTimeDiff offset;
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
  GST_INFO_OBJECT (self, "Resetting clock");
  self->first_time = TRUE;
  self->hw_offset = 0;
}
