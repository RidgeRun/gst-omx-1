/*
 * Copyright (C) 2013 RidgerRun LLC
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifndef __GST_OMX_VIDEO_FILTER_H__
#define __GST_OMX_VIDEO_FILTER_H__

#include <gst/gst.h>
#include <gst/video/video.h>

#include "gstomx.h"

G_BEGIN_DECLS
#define GST_TYPE_OMX_VIDEO_FILTER \
  (gst_omx_video_filter_get_type())
#define GST_OMX_VIDEO_FILTER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OMX_VIDEO_FILTER,GstOMXVideoFilter))
#define GST_OMX_VIDEO_FILTER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OMX_VIDEO_FILTER,GstOMXVideoFilterClass))
#define GST_OMX_VIDEO_FILTER_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_OMX_VIDEO_FILTER,GstOMXVideoFilterClass))
#define GST_IS_OMX_VIDEO_FILTER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OMX_VIDEO_FILTER))
#define GST_IS_OMX_VIDEO_FILTER_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OMX_VIDEO_FILTER))
/**
 * GST_OMX_VIDEO_FILTER_STREAM_LOCK:
 * @self: omx video filter instance
 *
 * Obtain a lock to protect the video processing function from concurrent access.
 */
#define GST_OMX_VIDEO_FILTER_STREAM_LOCK(self) g_rec_mutex_lock (&GST_OMX_VIDEO_FILTER (self)->stream_lock)
/**
 * GST_OMX_VIDEO_FILTER_STREAM_UNLOCK:
 * @self: omx video filter instance
 *
 * Release the lock that protects the video processing function from concurrent access.
 */
#define GST_OMX_VIDEO_FILTER_STREAM_UNLOCK(self) g_rec_mutex_unlock (&GST_OMX_VIDEO_FILTER (self)->stream_lock)
typedef struct _GstOMXVideoFilter GstOMXVideoFilter;
typedef struct _GstOMXVideoFilterClass GstOMXVideoFilterClass;
typedef struct _GstOMXVideoFilterPrivate GstOMXVideoFilterPrivate;

struct _GstOMXVideoFilter
{
  GstElement parent;

  /* < protected > */
  GstOMXComponent *comp;
  GstOMXPort *in_port;
  GList *out_port;

  /*< protected > */
  GstPad *sinkpad;
  GList *srcpads;

  /* protects all data processing, i.e. is locked
   * in the chain function, finish_frame and when
   * processing serialized events */
  GRecMutex stream_lock;

  /* properties */
  gboolean always_copy;
  guint output_buffers;
  guint input_buffers;
  
  /*< private > */
  GstOMXVideoFilterPrivate *priv;

  void *padding[GST_PADDING_LARGE];
};

/**
 * GstVideoEncoderClass:
 *
 * @num_outputs:    Indicates the amount of src pads in this element.
 * @transform_caps: Optional.
 *                  Given the pad in this direction and the given
 *                  caps, what caps are allowed on the other pad in this
 *                  element. If the element has multiple outputs the
 *                  corresponding srcpad under analysis is provided.
 * @fixate_caps:    Optional.
 *                  Given the @sinkcaps for the sink pad, fixate
 *                  the caps on the @srcpad. The function takes
 *                  ownership of @srccaps and returns a fixated version of
 *                  @srccaps. @srccaps is not guaranteed to be writable.
 * @set_format:     Optional.
 *                  Notifies subclass of incoming data format. @ininfo
 *                  and @outinfo_list are already been set acording
 *                  to the provided and negotiated caps.
 */
struct _GstOMXVideoFilterClass
{
  GstElementClass parent_class;

  GstOMXClassData cdata;
  guint num_outputs;

  /* virtual methods for subclasses */
  GstCaps *(*transform_caps) (GstOMXVideoFilter * self,
      GstPadDirection direction,
      GstPad * srcpad, GstCaps * caps, GstCaps * filter);
  GstCaps *(*fixate_caps) (GstOMXVideoFilter * self,
      GstPad * srcpad, GstCaps * sinkcaps, GstCaps * srccaps);
  gboolean  (*set_format) (GstOMXVideoFilter * self,
      GstCaps * incaps, GstVideoInfo * ininfo, GList * outcaps_list,
      GList * outinfo_list);
};

GType gst_omx_video_filter_get_type (void);

G_END_DECLS
#endif /* __GST_OMX_VIDEO_FILTER_H__ */
