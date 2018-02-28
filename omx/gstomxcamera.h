/*
 * Copyright (C) 2013 RidgerRun LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2 of the License.
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

#ifndef __GST_OMX_CAMERA_H__
#define __GST_OMX_CAMERA_H__

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/base/gstpushsrc.h>
#include "gstomx.h"

G_BEGIN_DECLS
#define GST_TYPE_OMX_CAMERA \
  (gst_omx_camera_get_type())
#define GST_OMX_CAMERA(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_OMX_CAMERA,GstOMXCamera))
#define GST_OMX_CAMERA_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_OMX_CAMERA,GstOMXCameraClass))
#define GST_OMX_CAMERA_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),GST_TYPE_OMX_CAMERA,GstOMXCameraClass))
#define GST_IS_OMX_CAMERA(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_OMX_CAMERA))
#define GST_IS_OMX_CAMERA_CLASS(obj) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_OMX_CAMERA))
typedef struct _GstOMXCamera GstOMXCamera;
typedef struct _GstOMXCameraClass GstOMXCameraClass;
typedef struct _GstOMXCameraPrivate GstOMXCameraPrivate;

struct _GstOMXCamera
{
  GstPushSrc parent;

  /* < protected > */
  GstOMXComponent *comp;
  GstOMXPort *outport;;

  /* Format */
  GstCaps *probed_caps;

  /* TRUE if the component is configured and saw
   * the first buffer */
  gboolean started;
  /* Draining state */
  gboolean drained;
  /* TRUE if we are using upstream input buffers */
  gboolean sharing;

  /* Ouput buffer pool */
  GstBufferPool *outpool;
  guint imagesize;
  guint minbuffers;
  guint maxbuffers;

  guint64 offset;
  GstClockTime duration;
  GstClockTime running_time;
  GstClockTime omx_delay;

  /* properties */
  gboolean always_copy;
  gint interface;
  gint capt_mode;
  gint vip_mode;
  gint scan_type;
  gint num_buffers;
  guint skip_frames;

  /*< private > */
  GstOMXCameraPrivate *priv;

  void *padding[GST_PADDING_LARGE];
};

struct _GstOMXCameraClass
{
  GstPushSrcClass parent_class;

  GstOMXClassData cdata;
};

GType gst_omx_camera_get_type (void);

G_END_DECLS
#endif /* __GST_OMX_CAMERA_H__ */
