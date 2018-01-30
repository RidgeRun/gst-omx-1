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

/**
 * SECTION:gstomxvideofilter
 * @short_description: Base class for video filters
 * @see_also:
 *
 * This base class is for video filters processing raw video data.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideoutils.h>
#include <string.h>

#include "OMX_TI_Common.h"
#include <omx_vfpc.h>
#include <OMX_TI_Index.h>

#include "gstomxbufferpool.h"
#include "gstomxvideofilter.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_video_filter_debug_category);
#define GST_CAT_DEFAULT gst_omx_video_filter_debug_category

#define GST_OMX_VIDEO_FILTER_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_OMX_VIDEO_FILTER, \
        GstOMXVideoFilterPrivate))

struct _GstOMXVideoFilterPrivate
{
  /* Video format */
  GstVideoInfo input_info;
  GList *output_info;
  GList *output_pool;
  GHashTable *frames;           /* Protected with OBJECT_LOCK */

  /* TRUE if the component is configured and saw
   * the first buffer */
  gboolean started;
  /* Draining state */
  gboolean drained;
  /* TRUE if we are using upstream input buffers */
  gboolean sharing;
  GstFlowReturn downstream_flow_ret;

};

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* called to create new GstVideoInfo struct initialized with
 * @caps parsed data.
 */
static GstVideoInfo *
_new_video_info (GstCaps * caps)
{
  GstVideoInfo *info;
  info = g_slice_new0 (GstVideoInfo);
  gst_video_info_init (info);
  if (G_UNLIKELY (!gst_video_info_from_caps (info, caps)))
    goto parse_fail;

  return info;

parse_fail:
  {
    g_slice_free (GstVideoInfo, info);
    return NULL;
  }
}

/* called to free the GstVideoInfo created with _new_video_info
 */
static void
_free_video_info (GstVideoInfo * info)
{
  g_slice_free (GstVideoInfo, info);
}

/* called to add a GstVideoCodecFrame to the pad pending list given
 * in @value*/
static void
add_frame_to_pad_array (gpointer key, gpointer value, gpointer user_data)
{
  GstVideoCodecFrame *frame = (GstVideoCodecFrame *) user_data;
  gst_video_codec_frame_ref (frame);
  g_ptr_array_add ((GPtrArray *) value, frame);
}

/* called to free @value pad frame pending list, unref each
 * frame before freeing */
static void
free_frame_array (gpointer key, gpointer value, gpointer user_data)
{
  g_ptr_array_foreach ((GPtrArray *) value, (GFunc) gst_video_codec_frame_unref,
      NULL);
  g_ptr_array_free ((GPtrArray *) value, FALSE);
}


static GstElementClass *parent_class = NULL;

static void gst_omx_video_filter_class_init (GstOMXVideoFilterClass * klass);
static void gst_omx_video_filter_init (GstOMXVideoFilter * self,
    GstOMXVideoFilterClass * klass);

static void gst_omx_video_filter_finalize (GObject * object);
static void gst_omx_video_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_video_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_omx_video_filter_set_caps (GstOMXVideoFilter * self,
    GstCaps * incaps);
static GstCaps *gst_omx_video_filter_sink_get_caps (GstOMXVideoFilter * self,
    GstCaps * filter);
static GstStateChangeReturn gst_omx_video_filter_change_state (GstElement *
    element, GstStateChange transition);
static gboolean gst_omx_video_filter_sink_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static gboolean gst_omx_video_filter_sink_query (GstPad * pad,
    GstObject * parent, GstQuery * query);
static GstFlowReturn gst_omx_video_filter_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buf);

static GstFlowReturn gst_omx_video_filter_drain (GstOMXVideoFilter * self);

static GstCaps *gst_omx_video_filter_default_transform_caps (GstOMXVideoFilter *
    self, GstPadDirection direction, GstPad * srcpad, GstCaps * caps,
    GstCaps * filter);
static GstCaps *gst_omx_video_filter_default_fixate_caps (GstOMXVideoFilter *
    self, GstPad * srcpad, GstCaps * sinkcaps, GstCaps * srccaps);

static gboolean gst_omx_video_filter_stop (GstOMXVideoFilter * self);
static gboolean gst_omx_video_filter_shutdown (GstOMXVideoFilter * self);

gint gst_omx_video_filter_get_buffer_size (GstVideoFormat format, gint stride,
    gint height);
OMX_COLOR_FORMATTYPE gst_omx_video_filter_get_color_format (GstVideoFormat
    format);

enum
{
  PROP_0,
  PROP_ALWAYS_COPY,
  PROP_OUTPUT_BUFFERS,
  PROP_INPUT_BUFFERS
};

#define GST_OMX_VIDEO_FILTER_ALWAYS_COPY_DEFAULT FALSE
#define GST_OMX_VIDEO_FILTER_OUTPUT_BUFFERS_DEFAULT 6
#define GST_OMX_VIDEO_FILTER_INPUT_BUFFERS_DEFAULT 6

/* we can't use G_DEFINE_ABSTRACT_TYPE because we need the klass in the _init
 * method to get to the padtemplates */
GType
gst_omx_video_filter_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    static const GTypeInfo info = {
      sizeof (GstOMXVideoFilterClass),
      NULL,
      NULL,
      (GClassInitFunc) gst_omx_video_filter_class_init,
      NULL,
      NULL,
      sizeof (GstOMXVideoFilter),
      0,
      (GInstanceInitFunc) gst_omx_video_filter_init,
    };
    const GInterfaceInfo preset_interface_info = {
      NULL,                     /* interface_init */
      NULL,                     /* interface_finalize */
      NULL                      /* interface_data */
    };

    _type = g_type_register_static (GST_TYPE_ELEMENT,
        "GstOMXVideoFilter", &info, G_TYPE_FLAG_ABSTRACT);
    g_type_add_interface_static (_type, GST_TYPE_PRESET,
        &preset_interface_info);
    g_once_init_leave (&type, _type);
  }
  return type;
}


static void
gst_omx_video_filter_class_init (GstOMXVideoFilterClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_omx_video_filter_debug_category,
      "omxvideofilter", 0, "OMX video filter base class");

  parent_class = g_type_class_peek_parent (klass);

  g_type_class_add_private (klass, sizeof (GstOMXVideoFilterPrivate));

  gobject_class->finalize = gst_omx_video_filter_finalize;
  gobject_class->set_property = gst_omx_video_filter_set_property;
  gobject_class->get_property = gst_omx_video_filter_get_property;

  g_object_class_install_property (gobject_class, PROP_ALWAYS_COPY,
      g_param_spec_boolean ("always-copy", "Always copy",
          "If the buffer will be used or not directly for the OpenMax component",
          GST_OMX_VIDEO_FILTER_ALWAYS_COPY_DEFAULT, G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class, PROP_OUTPUT_BUFFERS,
      g_param_spec_uint ("output-buffers", "Output buffers",
          "The amount of OMX output buffers",
          1, 16, GST_OMX_VIDEO_FILTER_OUTPUT_BUFFERS_DEFAULT,
          G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_INPUT_BUFFERS,
      g_param_spec_uint ("input-buffers", "Input buffers",
          "The amount of OMX input buffers",
          1, 16, GST_OMX_VIDEO_FILTER_INPUT_BUFFERS_DEFAULT,
          G_PARAM_READWRITE));

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_change_state);


  klass->cdata.type = GST_OMX_COMPONENT_TYPE_FILTER;
  klass->cdata.default_sink_template_caps = "video/x-raw, "
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;

  klass->transform_caps =
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_default_transform_caps);
  klass->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_default_fixate_caps);
}

static void
gst_omx_video_filter_reset (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *g;

  GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);

  priv->drained = TRUE;

  /* Free video frames from src pad pending list */
  if (priv->frames) {
    g_hash_table_foreach (priv->frames, (GHFunc) free_frame_array, NULL);
    g_hash_table_destroy (priv->frames);
  }
  priv->frames = NULL;

  for (g = priv->output_info; g; g = g->next) {
    _free_video_info ((GstVideoInfo *) g->data);
  }
  g_list_free (priv->output_info);
  priv->output_info = NULL;

  gst_video_info_init (&priv->input_info);

  GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
}

static void
gst_omx_video_filter_init (GstOMXVideoFilter * self,
    GstOMXVideoFilterClass * klass)
{
  GstOMXVideoFilterPrivate *priv;
  GstPadTemplate *pad_template;
  GstPad *pad;
  gint i;

  g_return_if_fail (klass->num_outputs > 0);

  priv = self->priv = GST_OMX_VIDEO_FILTER_GET_PRIVATE (self);

  pad_template =
      gst_element_class_get_pad_template (GST_ELEMENT_CLASS (klass), "sink");
  g_return_if_fail (pad_template != NULL);

  self->sinkpad = pad = gst_pad_new_from_template (pad_template, "sink");
  gst_pad_set_chain_function (pad,
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_chain));
  gst_pad_set_event_function (pad,
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_sink_event));
  gst_pad_set_query_function (pad,
      GST_DEBUG_FUNCPTR (gst_omx_video_filter_sink_query));
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);

  for (i = 0; i < klass->num_outputs; i++) {
    gchar *padname;

    if (klass->num_outputs == 1)
      padname = g_strdup ("src");
    else
      padname = g_strdup_printf ("src%d", i);

    pad_template =
        gst_element_class_get_pad_template (GST_ELEMENT_CLASS (klass), padname);
    g_return_if_fail (pad_template != NULL);
    pad = gst_pad_new_from_template (pad_template, padname);
    g_free (padname);

    self->srcpads = g_list_append (self->srcpads, pad);
    gst_element_add_pad (GST_ELEMENT (self), pad);
  }

  priv->frames = NULL;
  priv->output_info = NULL;
  priv->output_pool = NULL;
  priv->started = FALSE;
  priv->sharing = FALSE;

  self->out_port = NULL;

  self->always_copy = GST_OMX_VIDEO_FILTER_ALWAYS_COPY_DEFAULT;
  self->input_buffers = GST_OMX_VIDEO_FILTER_INPUT_BUFFERS_DEFAULT;
  self->output_buffers = GST_OMX_VIDEO_FILTER_OUTPUT_BUFFERS_DEFAULT;

  gst_omx_video_filter_reset (self);

  g_rec_mutex_init (&self->stream_lock);
}

static GstVideoCodecFrame *
gst_omx_video_filter_new_frame (GstOMXVideoFilter * self, GstBuffer * buf,
    GstClockTime pts, GstClockTime dts, GstClockTime duration)
{
  GstVideoCodecFrame *frame;

  frame = g_slice_new0 (GstVideoCodecFrame);
  frame->ref_count = 1;

  frame->input_buffer = buf;
  frame->pts = pts;
  frame->dts = dts;
  frame->duration = duration;
  frame->abidata.ABI.ts = pts;

  return frame;
}

static GstFlowReturn
gst_omx_video_filter_finish_frame (GstOMXVideoFilter * self, GstPad * srcpad,
    GstVideoCodecFrame * frame)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstFlowReturn ret = GST_FLOW_OK;
  GPtrArray *frames;

  GST_LOG_OBJECT (self,
      "finish frame fpn %d", frame->presentation_frame_number);

  GST_LOG_OBJECT (self, "frame PTS %" GST_TIME_FORMAT
      ", DTS %" GST_TIME_FORMAT, GST_TIME_ARGS (frame->pts),
      GST_TIME_ARGS (frame->dts));

  GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);

  /* no buffer data means this frame is skipped/dropped */
  if (!frame->output_buffer) {
    GST_DEBUG_OBJECT (self, "skipping frame %" GST_TIME_FORMAT,
        GST_TIME_ARGS (frame->pts));
    goto done;
  }

  GST_BUFFER_PTS (frame->output_buffer) = frame->pts;
  GST_BUFFER_DTS (frame->output_buffer) = frame->dts;
  GST_BUFFER_DURATION (frame->output_buffer) = frame->duration;

  if (G_UNLIKELY (GST_BUFFER_IS_DISCONT (frame->input_buffer))) {
    GST_LOG_OBJECT (self, "marking discont");
    GST_BUFFER_FLAG_SET (frame->output_buffer, GST_BUFFER_FLAG_DISCONT);
  }

  /* A reference always needs to be owned by the frame on the buffer.
   * For that reason, we use a complete sub-buffer (zero-cost) to push
   * downstream.
   * The original buffer will be free-ed only when downstream AND the
   * current implementation are done with the frame. */
  if (ret == GST_FLOW_OK)
    ret = gst_pad_push (srcpad, gst_buffer_ref (frame->output_buffer));

done:
  /* handed out */

  /* unref once from the list */
  frames = g_hash_table_lookup (priv->frames, srcpad);
  if (frames) {
    g_ptr_array_remove (frames, frame);
    gst_video_codec_frame_unref (frame);
  }
  /* unref because this function takes ownership */
  gst_video_codec_frame_unref (frame);

  GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);

  return ret;
}

#define MAX_FRAME_DIST_TICKS  (5 * OMX_TICKS_PER_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)
static GstVideoCodecFrame *
gst_omx_video_filter_find_nearest_frame (GstOMXVideoFilter * self, GstPad * pad,
    GstOMXBuffer * buf)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *l, *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GPtrArray *frames;
  gint i;

  frames = g_hash_table_lookup (priv->frames, pad);

  for (i = 0; i < frames->len; i++) {
    GstVideoCodecFrame *tmp = frames->pdata[i];
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > buf->omx_buf->nTimeStamp)
      diff = timestamp - buf->omx_buf->nTimeStamp;
    else
      diff = buf->omx_buf->nTimeStamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((buf->omx_buf->nTimeStamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (i = 0; i < frames->len; i++) {
      GstVideoCodecFrame *tmp = frames->pdata[i];
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_ticks;

      /* This happens for frames that were just added but
       * which were not passed to the component yet. Ignore
       * them here!
       */
      if (!id)
        continue;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_ticks = 0;
      else
        diff_ticks = best_timestamp - id->timestamp;

      if (diff_ticks > MAX_FRAME_DIST_TICKS) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }

  if (finish_frames) {
    g_warning ("Too old frames, bug in self -- please file a bug");
    for (l = finish_frames; l; l = l->next) {
      gst_omx_video_filter_finish_frame (GST_OMX_VIDEO_FILTER (self), pad,
          l->data);
    }
  }

  if (best)
    gst_video_codec_frame_ref (best);

  return best;
}


static void
buffer_free (gpointer data)
{
  GstOMXBuffer *buf = (GstOMXBuffer *) data;
  OMX_ERRORTYPE err;

  GST_LOG ("Releasing buffer %p", buf->omx_buf->pBuffer);

  err = gst_omx_port_release_buffer (buf->port, buf);
  if (err != OMX_ErrorNone)
    GST_ERROR ("Failed to relase output buffer to component: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
}


static GstFlowReturn
gst_omx_video_filter_handle_output_frame (GstOMXVideoFilter * self,
    GstOMXPort * port, GstPad * srcpad, GstOMXBuffer * buf,
    GstVideoCodecFrame * frame)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstBufferPool *outpool;
  gint idx, n, i;

  if (buf->omx_buf->nFilledLen > 0) {
    GstBuffer *outbuf;
    GstMapInfo map = GST_MAP_INFO_INIT;

    GST_LOG_OBJECT (self, "Handling output data");

    if (self->always_copy) {
      outbuf = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

      gst_buffer_map (outbuf, &map, GST_MAP_WRITE);
      memcpy (map.data,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          buf->omx_buf->nFilledLen);
      gst_buffer_unmap (outbuf, &map);
    } else {
      GstBufferPoolAcquireParams params = { 0, };
      idx = g_list_index (self->srcpads, srcpad);
      outpool = g_list_nth_data (priv->output_pool, idx);

      n = port->buffers->len;
      for (i = 0; i < n; i++) {
        GstOMXBuffer *tmp = g_ptr_array_index (port->buffers, i);

        if (tmp == buf)
          break;
      }
      g_assert (i != n);

      GST_OMX_BUFFER_POOL (outpool)->current_buffer_index = i;
      flow_ret = gst_buffer_pool_acquire_buffer (outpool, &outbuf, &params);
      if (flow_ret != GST_FLOW_OK) {
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
    }

    GST_BUFFER_TIMESTAMP (outbuf) =
        gst_util_uint64_scale (buf->omx_buf->nTimeStamp, GST_SECOND,
        OMX_TICKS_PER_SECOND);
    if (buf->omx_buf->nTickCount != 0)
      GST_BUFFER_DURATION (outbuf) =
          gst_util_uint64_scale (buf->omx_buf->nTickCount, GST_SECOND,
          OMX_TICKS_PER_SECOND);

    if (frame) {
      frame->output_buffer = outbuf;
      flow_ret = gst_omx_video_filter_finish_frame (self, srcpad, frame);
    } else {
      GST_ERROR_OBJECT (self, "No corresponding frame found");
      flow_ret = gst_pad_push (srcpad, outbuf);
    }
  } else if (frame != NULL) {
    flow_ret = gst_omx_video_filter_finish_frame (self, srcpad, frame);
  }

  return flow_ret;

invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Cannot acquire output buffer from pool"));
    return flow_ret;
  }
}

static void
gst_omx_video_filter_output_loop (GstPad * pad)
{
  GstOMXVideoFilter *self;
  GstOMXVideoFilterPrivate *priv;
  GstOMXVideoFilterClass *klass;
  GstOMXPort *port;
  GstOMXBuffer *buf = NULL;
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstOMXAcquireBufferReturn acq_return;
  OMX_ERRORTYPE err;

  self = GST_OMX_VIDEO_FILTER (gst_pad_get_parent (pad));
  priv = self->priv;
  klass = GST_OMX_VIDEO_FILTER_GET_CLASS (self);
  port = (GstOMXPort *) gst_pad_get_element_private (pad);

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK) {
    return;
  }
  /* This prevents a deadlock between the srcpad stream
   * lock and the videocodec stream lock, if ::reset()
   * is called at the wrong time
   */
  if (gst_omx_port_is_flushing (port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (port, buf);
    goto flushing;
  }

  GST_LOG_OBJECT (self, "Handling buffer: 0x%08x %" G_GUINT64_FORMAT,
      (guint) buf->omx_buf->nFlags, (guint64) buf->omx_buf->nTimeStamp);

  GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);
  frame = gst_omx_video_filter_find_nearest_frame (self, pad, buf);

  flow_ret =
      gst_omx_video_filter_handle_output_frame (self, port, pad, buf, frame);

  GST_LOG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));

  if (self->always_copy) {
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  priv->downstream_flow_ret = flow_ret;

  GST_LOG_OBJECT (self, "Read frame from component");

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
  return;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->comp),
            gst_omx_component_get_last_error (self->comp)));
    gst_pad_push_event (pad, gst_event_new_eos ());
    gst_pad_pause_task (pad);
    priv->downstream_flow_ret = GST_FLOW_ERROR;
    priv->started = FALSE;
    return;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (pad);
    priv->downstream_flow_ret = GST_FLOW_FLUSHING;
    priv->started = FALSE;
    return;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");

      gst_pad_push_event (pad, gst_event_new_eos ());
      gst_pad_pause_task (pad);
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Internal data stream error."),
          ("stream stopped, reason %s", gst_flow_get_name (flow_ret)));

      gst_pad_push_event (pad, gst_event_new_eos ());
      gst_pad_pause_task (pad);
    }
    priv->started = FALSE;
    GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
    return;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (pad, gst_event_new_eos ());
    gst_pad_pause_task (pad);
    priv->downstream_flow_ret = GST_FLOW_ERROR;
    priv->started = FALSE;
    GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
    return;
  }
}

static gboolean
gst_omx_video_filter_drain (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *srcpad, *outport;
  if (self->in_port)
    gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, TRUE);
  for (outport = self->out_port; outport; outport = outport->next)
    gst_omx_port_set_flushing (outport->data, 5 * GST_SECOND, TRUE);

  if (!gst_omx_video_filter_stop (self))
    return FALSE;

  /* Create hash table to manage pending frames per src pad */
  priv->frames = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next)
    g_hash_table_insert (priv->frames, srcpad->data, g_ptr_array_new ());

  if (!gst_omx_video_filter_shutdown (self))
    return FALSE;
  GST_DEBUG_OBJECT (self, "Filter drained and disabled");

  priv->downstream_flow_ret = GST_FLOW_OK;
  return TRUE;
}


static GstCaps *
gst_omx_video_filter_default_transform_caps (GstOMXVideoFilter * self,
    GstPadDirection direction, GstPad * srcpad, GstCaps * caps,
    GstCaps * filter)
{
  GstCaps *ret;

  GST_DEBUG_OBJECT (self, "identity from: %" GST_PTR_FORMAT, caps);
  /* no transform function, use the identity transform */
  if (filter) {
    ret = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
  } else {
    ret = gst_caps_ref (caps);
  }
  return ret;
}

static GstCaps *
gst_omx_video_filter_default_fixate_caps (GstOMXVideoFilter * self,
    GstPad * srcpad, GstCaps * sinkcaps, GstCaps * srccaps)
{
  GstCaps *intersection, *caps;
  gchar *caps_str = NULL;

  /* First try passthrough */
  intersection =
      gst_caps_intersect_full (sinkcaps, srccaps, GST_CAPS_INTERSECT_FIRST);
  caps =
      gst_caps_is_empty (intersection) ? gst_caps_ref (srccaps) :
      gst_caps_ref (intersection);
  gst_caps_unref (intersection);

  caps = gst_caps_fixate (caps);

  GST_DEBUG_OBJECT (self, "fixated to (%" GST_PTR_FORMAT "): %s", caps,
      caps_str = gst_caps_to_string (caps));
  if (caps_str)
    g_free (caps_str);

  return caps;
}


/* given @caps on the src or sink pad (given by @direction)
 * calculate the possible caps on the other pad. When the
 * element has multiple outputs, @srcpad indicates the output
 * pad that is being analysed.
 *
 * Returns new caps, unref after usage.
 */
static GstCaps *
gst_omx_video_filter_transform_caps (GstOMXVideoFilter * self,
    GstPadDirection direction, GstPad * srcpad, GstCaps * caps,
    GstCaps * filter)
{
  GstCaps *ret = NULL;
  GstOMXVideoFilterClass *klass = GST_OMX_VIDEO_FILTER_GET_CLASS (self);

  if (caps == NULL)
    return NULL;

  /* if there is a custom transform function, use this */
  if (klass->transform_caps) {

    GST_DEBUG_OBJECT (self, "transform caps (direction = %d) %s pad",
        direction, GST_OBJECT_NAME (srcpad));

    GST_LOG_OBJECT (self, "from: %" GST_PTR_FORMAT, caps);
    ret = klass->transform_caps (self, direction, srcpad, caps, filter);
    GST_LOG_OBJECT (self, "  to: %" GST_PTR_FORMAT " %s", ret,
        gst_caps_to_string (ret));

    if (filter) {
      if (!gst_caps_is_subset (ret, filter)) {
        GstCaps *intersection;

        GST_ERROR_OBJECT (self,
            "transform_caps returned caps %" GST_PTR_FORMAT
            " which are not a real subset of the filter caps %"
            GST_PTR_FORMAT, ret, filter);
        intersection =
            gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref (ret);
        ret = intersection;
      }
    }
  }

  GST_LOG_OBJECT (self, "to: %" GST_PTR_FORMAT, ret);

  return ret;
}


/* get the caps that can be handled by sinkpad. We perform:
 *
 *  - take the caps of peer of otherpad,
 *  - filter against the padtemplate of otherpad,
 *  - calculate all transforms of remaining caps
 *  - filter against template of @pad
 *
 * If there is no peer, we simply return the caps of the sink pad template.
 */
static GstCaps *
gst_omx_video_filter_proxy_get_caps (GstOMXVideoFilter * self, GstCaps * caps,
    GstCaps * filter)
{
  GstCaps *peercaps, *filtercaps, *temp = NULL, *temp2 = NULL, *peerfilter =
      NULL;
  GstCaps *templ, *otempl;
  GstCaps *sinkcaps;
  GstPad *pad, *otherpad;
  GList *srcpad;
  gchar *caps_str = NULL;

  templ =
      caps ? gst_caps_ref (caps) :
      gst_pad_get_pad_template_caps (self->sinkpad);

  pad = self->sinkpad;
  sinkcaps = gst_caps_new_any ();

  GST_DEBUG_OBJECT (self, "filter caps  (%" GST_PTR_FORMAT "): %s",
      filter, caps_str = gst_caps_to_string (filter));
  if (caps_str)
    g_free (caps_str);

  /* filtered against our padtemplate of this pad */
  GST_DEBUG_OBJECT (pad,
      "intersecting against template  (%" GST_PTR_FORMAT "): %s", templ,
      caps_str = gst_caps_to_string (templ));
  if (caps_str)
    g_free (caps_str);

  if (filter)
    filtercaps =
        gst_caps_intersect_full (filter, templ, GST_CAPS_INTERSECT_FIRST);
  else
    filtercaps = gst_caps_ref (templ);

  GST_DEBUG_OBJECT (self, "intersected  (%" GST_PTR_FORMAT "): %s",
      filtercaps, caps_str = gst_caps_to_string (filtercaps));
  if (caps_str)
    g_free (caps_str);

  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next) {
    otherpad = srcpad->data;
    otempl = gst_pad_get_pad_template_caps (otherpad);

    /* first prepare the filter to be send onwards. We need to filter and
     * transform it to valid caps for the otherpad. */

    /* then see what we can transform this to */
    peerfilter = gst_omx_video_filter_transform_caps (self,
        gst_pad_get_direction (pad), otherpad, filtercaps, NULL);

    GST_DEBUG_OBJECT (self, "transformed  (%" GST_PTR_FORMAT "): %s",
        peerfilter, caps_str = gst_caps_to_string (peerfilter));
    if (caps_str)
      g_free (caps_str);

    /* and filter against the template of the other pad */
    GST_DEBUG_OBJECT (otherpad,
        "intersecting against template  %" GST_PTR_FORMAT " %s", otempl,
        caps_str = gst_caps_to_string (otempl));
    if (caps_str)
      g_free (caps_str);
    /* We keep the caps sorted like the returned caps */
    if (peerfilter) {
      temp =
          gst_caps_intersect_full (peerfilter, otempl,
          GST_CAPS_INTERSECT_FIRST);
      GST_DEBUG_OBJECT (self, "intersected  %" GST_PTR_FORMAT " %s", temp,
          caps_str = gst_caps_to_string (temp));
      if (caps_str)
        g_free (caps_str);
      gst_caps_unref (peerfilter);
      peerfilter = temp;
    } else {
      peerfilter = gst_caps_ref (otempl);
    }

    /* query the peer with the transformed filter */
    peercaps = gst_pad_peer_query_caps (otherpad, peerfilter);

    if (peerfilter)
      gst_caps_unref (peerfilter);

    if (peercaps) {
      GST_DEBUG_OBJECT (otherpad, "peer caps  (%" GST_PTR_FORMAT "): %s",
          peercaps, caps_str = gst_caps_to_string (peercaps));
      if (caps_str)
        g_free (caps_str);
      /* filtered against our padtemplate on the other side */
      temp =
          gst_caps_intersect_full (peercaps, otempl, GST_CAPS_INTERSECT_FIRST);
      GST_DEBUG_OBJECT (self,
          "intersected with %s template: (%" GST_PTR_FORMAT ") %s",
          GST_OBJECT_NAME (otherpad), temp, caps_str =
          gst_caps_to_string (temp));
      if (caps_str)
        g_free (caps_str);
    } else {
      temp = gst_caps_ref (otempl);
    }

    /* then see what we can transform this to */
    temp2 = gst_omx_video_filter_transform_caps (self,
        gst_pad_get_direction (otherpad), pad, temp, filter);
    GST_DEBUG_OBJECT (self, "transformed  %" GST_PTR_FORMAT " %s", temp2,
        caps_str = gst_caps_to_string (temp2));
    if (caps_str)
      g_free (caps_str);
    gst_caps_unref (temp);

    /* filtered against the total pads caps */
    temp = gst_caps_intersect (temp2, sinkcaps);
    gst_caps_unref (temp2);
    gst_caps_unref (sinkcaps);
    sinkcaps = temp;

    if (peercaps) {
      /* Now try if we can put the untransformed downstream caps first */
      temp =
          gst_caps_intersect_full (peercaps, sinkcaps,
          GST_CAPS_INTERSECT_FIRST);
      if (!gst_caps_is_empty (temp)) {
        sinkcaps = gst_caps_merge (temp, sinkcaps);
      } else {
        gst_caps_unref (temp);
      }
    }

    if (peercaps)
      gst_caps_unref (peercaps);
  }

  if (sinkcaps) {
    /* and filter against the template of this pad */
    /* We keep the caps sorted like the returned caps */
    temp = gst_caps_intersect_full (sinkcaps, templ, GST_CAPS_INTERSECT_FIRST);
    GST_DEBUG_OBJECT (pad, "intersected with sink templ %" GST_PTR_FORMAT,
        temp);
    gst_caps_unref (sinkcaps);
    sinkcaps = temp;
  } else {
    gst_caps_unref (sinkcaps);
    /* no peer or the peer can do anything, our padtemplate is enough then */
    sinkcaps = gst_caps_ref (filtercaps);
  }

  GST_DEBUG_OBJECT (self, "returning  %" GST_PTR_FORMAT, sinkcaps);
  gst_caps_unref (templ);
  gst_caps_unref (otempl);
  gst_caps_unref (filtercaps);

  return sinkcaps;
}

static GstCaps *
gst_omx_video_filter_sink_get_caps (GstOMXVideoFilter * self, GstCaps * filter)
{
  GstCaps *caps = NULL;
  gchar *caps_str = NULL;

  caps = gst_omx_video_filter_proxy_get_caps (self, NULL, filter);

  GST_LOG_OBJECT (self, "Returning caps %" GST_PTR_FORMAT " %s", caps,
      caps_str = gst_caps_to_string (caps));
  if (caps_str)
    g_free (caps_str);

  return caps;
}

static gboolean
gst_omx_video_filter_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (parent);
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
    {
      GstCaps *filter, *caps;

      gst_query_parse_caps (query, &filter);
      caps = gst_omx_video_filter_sink_get_caps (self, filter);
      gst_query_set_caps_result (query, caps);
      gst_caps_unref (caps);
      ret = TRUE;
      break;
    }

    default:
      ret = gst_pad_query_default (pad, parent, query);
      break;
  }
  return ret;
}

static void
gst_omx_video_filter_finalize (GObject * object)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (object);
  GST_LOG_OBJECT (self, "finalize");

  g_rec_mutex_clear (&self->stream_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

OMX_COLOR_FORMATTYPE
gst_omx_video_filter_get_color_format (GstVideoFormat format)
{
  OMX_COLOR_FORMATTYPE omx_format;

  switch (format) {
    case GST_VIDEO_FORMAT_YUY2:
      omx_format = OMX_COLOR_FormatYCbYCr;
      break;
    case GST_VIDEO_FORMAT_I420:
      omx_format = OMX_COLOR_FormatYUV420Planar;
      break;
    case GST_VIDEO_FORMAT_NV12:
      omx_format = OMX_COLOR_FormatYUV420SemiPlanar;
      break;
    default:
      omx_format = OMX_COLOR_FormatUnused;
      break;
  }
  return omx_format;
}

gint
gst_omx_video_filter_get_buffer_size (GstVideoFormat format, gint stride,
    gint height)
{
  gint buffer_size;

  switch (format) {
    case GST_VIDEO_FORMAT_YUY2:
      buffer_size = stride * height;
      break;
    case GST_VIDEO_FORMAT_I420:
      buffer_size = stride * height + 2 * ((stride >> 1) * ((height + 1) >> 2));
      break;
    case GST_VIDEO_FORMAT_NV12:
      buffer_size = (stride * height * 3) >> 1;
      break;
    default:
      buffer_size = 0;
      break;
  }
  return buffer_size;
}

static gboolean
gst_omx_video_filter_set_format (GstOMXVideoFilter * self, GstCaps * incaps,
    GstVideoInfo * ininfo, GList * outcaps_list, GList * outinfo_list)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstOMXVideoFilterClass *klass = GST_OMX_VIDEO_FILTER_GET_CLASS (self);
  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_PARAM_BUFFER_MEMORYTYPE mem_type;
  OMX_ERRORTYPE err;
  GstVideoInfo *outinfo;
  GstStructure *config;
  GList *outport, *srcinfo, *srccaps, *outpool;
  gint i;

  GST_DEBUG_OBJECT (self, "Setting new format");

  needs_disable =
      gst_omx_component_get_state (self->comp,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;
  /* If the component is not in Loaded state and a real format change happens
   * we have to disable the port and re-allocate all buffers. If no real
   * format change happened we can just exit here.
   */
  if (needs_disable) {
    GST_DEBUG_OBJECT (self, "Need to disable and drain element");
    if (!gst_omx_video_filter_drain (self))
      goto drain_failed;
  }

  /* Set memory type on ports to raw memory */

  GST_OMX_INIT_STRUCT (&mem_type);
  mem_type.nPortIndex = self->in_port->index;
  mem_type.eBufMemoryType = OMX_BUFFER_MEMORY_DEFAULT;
  err = gst_omx_component_set_parameter (self->comp,
      OMX_TI_IndexParamBuffMemType, &mem_type);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Failed to set memory type on port %lu: %s (0x%08x)",
        mem_type.nPortIndex, gst_omx_error_to_string (err), err);
    return FALSE;
  }

  for (i = OMX_VFPC_OUTPUT_PORT_START_INDEX; i < klass->num_outputs; i++) {
    OMX_PARAM_BUFFER_MEMORYTYPE mem_type;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&mem_type);
    mem_type.nPortIndex = i;
    mem_type.eBufMemoryType = OMX_BUFFER_MEMORY_DEFAULT;
    err = gst_omx_component_get_parameter (self->comp,
        OMX_TI_IndexParamBuffMemType, &mem_type);
    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (self,
          "Failed to set memory type on port %lu: %s (0x%08x)",
          mem_type.nPortIndex, gst_omx_error_to_string (err), err);
      return FALSE;
    }
  }

  /* Set input height/width and color format */
  gst_omx_port_get_port_definition (self->in_port, &port_def);

  port_def.format.video.nFrameWidth = GST_VIDEO_INFO_WIDTH (ininfo);
  port_def.format.video.nFrameHeight = GST_VIDEO_INFO_HEIGHT (ininfo);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  /* Transform gstreamer video format to OMX color format */
  port_def.format.video.eColorFormat =
      gst_omx_video_filter_get_color_format (ininfo->finfo->format);
  if (port_def.format.video.eColorFormat == OMX_COLOR_FormatUnused) {
    GST_ERROR_OBJECT (self, "Unsupported format %s",
        gst_video_format_to_string (ininfo->finfo->format));
    return FALSE;
  }
  port_def.nBufferAlignment = 0;
  port_def.bBuffersContiguous = 0;
  port_def.nBufferCountActual = self->input_buffers;
  port_def.format.video.nStride = GST_VIDEO_INFO_PLANE_STRIDE (ininfo, 0);
  /* Calculating input buffer size */
  port_def.nBufferSize =
      gst_omx_video_filter_get_buffer_size (ininfo->finfo->format,
      port_def.format.video.nStride, port_def.format.video.nFrameHeight);

  GST_DEBUG_OBJECT (self, "Setting inport port definition");
  if (gst_omx_port_update_port_definition (self->in_port,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  /* set output height/width and color format */
  srcinfo = outinfo_list;
  srccaps = outcaps_list;
  outpool = priv->output_pool;
  for (outport = self->out_port; outport; outport = outport->next) {
    outinfo = srcinfo->data;
    gst_omx_port_get_port_definition (outport->data, &port_def);
    port_def.format.video.nFrameWidth = GST_VIDEO_INFO_WIDTH (outinfo);
    port_def.format.video.nFrameHeight = GST_VIDEO_INFO_HEIGHT (outinfo);
    port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    port_def.format.video.eColorFormat =
        gst_omx_video_filter_get_color_format (outinfo->finfo->format);
    if (port_def.format.video.eColorFormat == OMX_COLOR_FormatUnused) {
      GST_ERROR_OBJECT (self, "Unsupported format %s",
          gst_video_format_to_string (outinfo->finfo->format));
      return FALSE;
    }
    port_def.nBufferAlignment = 0;
    port_def.bBuffersContiguous = 0;
    port_def.nBufferCountActual = self->output_buffers;
    /* scalar buffer pitch should be multiple of 16 */
    port_def.format.video.nStride =
        ((port_def.format.video.nFrameWidth + 15) & 0xfffffff0) * 2;

    GST_DEBUG_OBJECT (self, "Updating outport port definition");
    if (gst_omx_port_update_port_definition (outport->data,
            &port_def) != OMX_ErrorNone)
      return FALSE;

    /* Configure output pool */
    config = gst_buffer_pool_get_config (outpool->data);
    gst_buffer_pool_config_set_params (config, srccaps->data,
        port_def.nBufferSize, port_def.nBufferCountActual,
        port_def.nBufferCountActual);
    if (!gst_buffer_pool_set_config (outpool->data, config))
      goto config_pool_failed;

    srcinfo = srcinfo->next;
    srccaps = srccaps->next;
    outpool = outpool->next;
  }

  if (klass->set_format) {
    if (!klass->set_format (self, incaps, ininfo, outcaps_list, outinfo_list)) {
      GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
      return FALSE;
    }
  }

  return TRUE;

drain_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to reconfigure, cannot drain component");
    return FALSE;
  }
config_pool_failed:
  {
    GST_INFO_OBJECT (self, "Failed to set config on output pool");
    return FALSE;
  }
}

/* given a fixed @caps on @pad, create the best possible caps for the
 * other pad.
 * @caps must be fixed when calling this function.
 *
 * This function calls the transform caps vmethod of the basetransform to figure
 * out the possible target formats. It then tries to select the best format from
 * this list by:
 *
 * - attempt passthrough if the target caps is a superset of the input caps
 * - fixating by using peer caps
 * - fixating with transform fixate function
 * - fixating with pad fixate functions.
 *
 * this function returns a caps that can be transformed into and is accepted by
 * the peer element.
 */
static GstCaps *
gst_omx_video_filter_find_transform (GstOMXVideoFilter * self,
    GstCaps * caps, GstPad * srcpad)
{
  GstOMXVideoFilterClass *klass = GST_OMX_VIDEO_FILTER_GET_CLASS (self);
  GstPad *srcpeer;
  GstCaps *srccaps;
  gboolean is_fixed;
  gchar *caps_str = NULL;
  /* caps must be fixed here, this is a programming error if it's not */
  g_return_val_if_fail (gst_caps_is_fixed (caps), NULL);

  srcpeer = gst_pad_get_peer (srcpad);

  /* see how we can transform the input caps */
  srccaps = gst_omx_video_filter_transform_caps (self,
      GST_PAD_DIRECTION (self->sinkpad), srcpad, caps, NULL);

  GST_DEBUG_OBJECT (self,
      "transformed  (%" GST_PTR_FORMAT "): %s", srccaps, caps_str =
      gst_caps_to_string (srccaps));
  if (caps_str)
    g_free (caps_str);
  /* The caps we can actually output is the intersection of the transformed
   * caps with the pad template for the pad */
  if (srccaps) {
    GstCaps *intersect, *templ_caps;

    templ_caps = gst_pad_get_pad_template_caps (srcpad);
    GST_DEBUG_OBJECT (self,
        "intersecting against padtemplate (%" GST_PTR_FORMAT "): %s",
        templ_caps, caps_str = gst_caps_to_string (templ_caps));
    if (caps_str)
      g_free (caps_str);

    intersect =
        gst_caps_intersect_full (srccaps, templ_caps, GST_CAPS_INTERSECT_FIRST);

    GST_DEBUG_OBJECT (self,
        "intersected (%" GST_PTR_FORMAT "): %s", intersect, caps_str =
        gst_caps_to_string (intersect));
    if (caps_str)
      g_free (caps_str);
    gst_caps_unref (srccaps);
    gst_caps_unref (templ_caps);
    srccaps = intersect;
  }

  /* check if transform is empty */
  if (!srccaps || gst_caps_is_empty (srccaps))
    goto no_transform;

  /* if the othercaps are not fixed, we need to fixate them, first attempt
   * is by attempting passthrough if the othercaps are a superset of caps. */
  /* FIXME. maybe the caps is not fixed because it has multiple structures of
   * fixed caps */
  is_fixed = gst_caps_is_fixed (srccaps);
  if (!is_fixed) {
    GST_DEBUG_OBJECT (self,
        "transform returned non fixed  (%" GST_PTR_FORMAT ") %s", srccaps,
        caps_str = gst_caps_to_string (srccaps));
    if (caps_str)
      g_free (caps_str);
    /* Now let's see what the peer suggests based on our transformed caps */
    if (srcpeer) {
      GstCaps *peercaps, *intersection, *templ_caps;

      GST_DEBUG_OBJECT (self,
          "Checking peer caps with filter %" GST_PTR_FORMAT, srccaps);

      peercaps = gst_pad_query_caps (srcpeer, srccaps);
      GST_DEBUG_OBJECT (self, "Resulted in (%" GST_PTR_FORMAT "): %s", peercaps,
          caps_str = gst_caps_to_string (peercaps));
      if (!gst_caps_is_empty (peercaps)) {
        templ_caps = gst_pad_get_pad_template_caps (srcpad);

        GST_DEBUG_OBJECT (self,
            "Intersecting with template caps %" GST_PTR_FORMAT, templ_caps);

        intersection =
            gst_caps_intersect_full (peercaps, templ_caps,
            GST_CAPS_INTERSECT_FIRST);
        GST_DEBUG_OBJECT (self, "Intersection (%" GST_PTR_FORMAT "): %s",
            intersection, caps_str = gst_caps_to_string (intersection));
        gst_caps_unref (peercaps);
        gst_caps_unref (templ_caps);
        peercaps = intersection;

        GST_DEBUG_OBJECT (self,
            "Intersecting with transformed caps %" GST_PTR_FORMAT, srccaps);
        intersection =
            gst_caps_intersect_full (peercaps, srccaps,
            GST_CAPS_INTERSECT_FIRST);
        GST_DEBUG_OBJECT (self, "Intersection (%" GST_PTR_FORMAT "): %s",
            intersection, caps_str = gst_caps_to_string (intersection));
        gst_caps_unref (peercaps);
        gst_caps_unref (srccaps);
        srccaps = intersection;
      } else {
        gst_caps_unref (srccaps);
        srccaps = peercaps;
      }

      is_fixed = gst_caps_is_fixed (srccaps);
    } else {
      GST_DEBUG_OBJECT (self, "no peer, doing passthrough");
      gst_caps_unref (srccaps);
      srccaps = gst_caps_ref (caps);
      is_fixed = TRUE;
    }
  }
  if (gst_caps_is_empty (srccaps))
    goto no_transform_possible;

  GST_DEBUG_OBJECT (self, "have %sfixed caps %" GST_PTR_FORMAT,
      (is_fixed ? "" : "non-"), srccaps);

  /* second attempt at fixation, call the fixate vmethod */
  /* caps could be fixed but the subclass may want to add fields */
  if (klass->fixate_caps) {
    GST_DEBUG_OBJECT (self, "calling fixate_caps for %" GST_PTR_FORMAT
        " using caps %" GST_PTR_FORMAT " on pad %s:%s", srccaps, caps,
        GST_DEBUG_PAD_NAME (srcpad));
    /* note that we pass the complete array of structures to the fixate
     * function, it needs to truncate itself */
    srccaps = klass->fixate_caps (self, srcpad, caps, srccaps);
    is_fixed = gst_caps_is_fixed (srccaps);
    GST_DEBUG_OBJECT (self, "after fixating %" GST_PTR_FORMAT, srccaps);
  }

  /* caps should be fixed now, if not we have to fail. */
  if (!is_fixed)
    goto could_not_fixate;

  /* and peer should accept */
  if (srcpeer && !gst_pad_query_accept_caps (srcpeer, srccaps))
    goto peer_no_accept;

  GST_DEBUG_OBJECT (self, "Input caps were %" GST_PTR_FORMAT
      ", and got final caps %" GST_PTR_FORMAT, caps, srccaps);

  if (srcpeer)
    gst_object_unref (srcpeer);

  return srccaps;

  /* ERRORS */
no_transform:
  {
    GST_DEBUG_OBJECT (self,
        "transform returned useless  %" GST_PTR_FORMAT, srccaps);
    goto error_cleanup;
  }
no_transform_possible:
  {
    GST_DEBUG_OBJECT (self,
        "transform could not transform %" GST_PTR_FORMAT
        " in anything we support", caps);
    goto error_cleanup;
  }
could_not_fixate:
  {
    GST_DEBUG_OBJECT (self, "failed to fixate %" GST_PTR_FORMAT, srccaps);
    goto error_cleanup;
  }
peer_no_accept:
  {
    GST_DEBUG_OBJECT (self, "failed to get peer of %" GST_PTR_FORMAT
        " to accept %" GST_PTR_FORMAT, srcpad, srccaps);
    goto error_cleanup;
  }
error_cleanup:
  {
    if (srcpeer)
      gst_object_unref (srcpeer);
    if (srccaps)
      gst_caps_unref (srccaps);
    return NULL;
  }
}

/* called when new caps arrive on the sink pad,
 * We try to find the best caps for the other side using our _find_transform()
 * function. If there are caps, we configure the transform for this new
 * transformation.
 */
static gboolean
gst_omx_video_filter_set_caps (GstOMXVideoFilter * self, GstCaps * incaps)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstVideoInfo *ininfo = NULL, *outinfo;
  GstCaps *outcaps = NULL;
  GstPad *otherpad;
  GList *outcaps_list = NULL, *outinfo_list = NULL;
  GList *srcpad, *srccaps;

  gboolean ret = FALSE;
  gboolean samecaps = FALSE;
  gchar *caps_str = NULL;

  GST_DEBUG_OBJECT (self, "have new caps %" GST_PTR_FORMAT " %s",
      incaps, caps_str = gst_caps_to_string (incaps));
  if (caps_str)
    g_free (caps_str);

  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next) {
    /* find best possible caps for the other pad */
    otherpad = srcpad->data;

    /* clear any pending reconfigure flag */
    gst_pad_check_reconfigure (otherpad);

    outcaps = gst_omx_video_filter_find_transform (self, incaps, otherpad);
    if (!outcaps || gst_caps_is_empty (outcaps))
      goto no_transform_possible;

    /* if we have the same caps, we can optimize and reuse the input caps */
    if (gst_caps_is_equal (incaps, outcaps)) {
      GST_INFO_OBJECT (self, "reuse caps");
      gst_caps_unref (outcaps);
      outcaps = gst_caps_ref (incaps);
    }
    outinfo = _new_video_info (outcaps);
    if (G_UNLIKELY (!outinfo))
      goto parse_fail;

    outcaps_list = g_list_append (outcaps_list, outcaps);
    outinfo_list = g_list_append (outinfo_list, outinfo);
    outcaps = NULL;
  }

  ininfo = _new_video_info (incaps);
  if (G_UNLIKELY (!ininfo))
    goto parse_fail;

  GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);
  //~ samecaps = gst_video_info_is_equal (ininfo, &(priv->input_info));
  samecaps = FALSE;
  if (!samecaps) {
    /* and subclass should be ready to configure format at any time around */
    ret =
        gst_omx_video_filter_set_format (self, incaps, ininfo, outcaps_list,
        outinfo_list);
    if (!ret) {
      GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
      goto done;
    }

    if (priv->output_info) {
      g_list_foreach (priv->output_info, (GFunc) _free_video_info, NULL);
      g_list_free (priv->output_info);
    }
    priv->output_info = outinfo_list;
    priv->input_info = *ininfo;

    /* let downstream know about our caps */
    srccaps = outcaps_list;
    for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next) {
      otherpad = srcpad->data;
      ret = gst_pad_set_caps (otherpad, srccaps->data);
      srccaps = srccaps->next;
    }

  } else {
    /* no need to stir things up */
    GST_DEBUG_OBJECT (self, "new video format identical to configured format");
    ret = TRUE;
  }

  GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);

done:
  if (ininfo)
    _free_video_info (ininfo);

  if (outcaps_list) {
    g_list_foreach (outcaps_list, (GFunc) gst_caps_unref, NULL);
    g_list_free (outcaps_list);
  }

  if (outcaps)
    gst_caps_unref (outcaps);

  if (!ret) {
    GST_WARNING_OBJECT (self, "rejected caps %" GST_PTR_FORMAT ": %s",
        incaps, caps_str = gst_caps_to_string (incaps));
    if (caps_str)
      g_free (caps_str);

    if (outinfo_list || priv->output_info) {
      g_list_foreach (outinfo_list, (GFunc) _free_video_info, NULL);
      g_list_free (outinfo_list);
    }
    priv->output_info = NULL;
  }
  return ret;

no_transform_possible:
  {
    GST_WARNING_OBJECT (self,
        "could not transform %" GST_PTR_FORMAT
        " in anything we support", incaps);
    ret = FALSE;
    goto done;
  }
parse_fail:
  {
    GST_WARNING_OBJECT (self, "Failed to parse caps");
    ret = FALSE;
    goto done;
  }
}

static gboolean
gst_omx_video_filter_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (parent);
  gboolean ret = FALSE;

  GST_DEBUG_OBJECT (self, "received event %d, %s", GST_EVENT_TYPE (event),
      GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      ret = gst_omx_video_filter_set_caps (self, caps);
      gst_event_unref (event);
      break;
    }
    default:
      ret = gst_pad_event_default (pad, parent, event);
      break;
  }

  return ret;
}

static void
gst_omx_video_filter_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (object);

  switch (prop_id) {
    case PROP_ALWAYS_COPY:
      self->always_copy = g_value_get_boolean (value);
      break;
    case PROP_OUTPUT_BUFFERS:
      self->output_buffers = g_value_get_uint (value);
      break;
    case PROP_INPUT_BUFFERS:
      self->input_buffers = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_video_filter_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (object);

  switch (prop_id) {
    case PROP_ALWAYS_COPY:
      g_value_set_boolean (value, self->always_copy);
      break;
    case PROP_OUTPUT_BUFFERS:
      g_value_set_uint (value, self->output_buffers);
      break;
    case PROP_INPUT_BUFFERS:
      g_value_set_uint (value, self->input_buffers);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_omx_video_filter_open (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstOMXVideoFilterClass *klass = GST_OMX_VIDEO_FILTER_GET_CLASS (self);
  GstBufferPool *pool;
  GstOMXPort *port;
  gint in_port_index, out_port_index;
  gint i;
  GList *srcpad;

  GST_LOG_OBJECT (self, "opening component %s", klass->cdata.component_name);
  self->comp =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);
  priv->started = FALSE;
  priv->sharing = FALSE;

  if (!self->comp)
    return FALSE;

  if (gst_omx_component_get_state (self->comp,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  in_port_index = klass->cdata.in_port_index;
  out_port_index = klass->cdata.out_port_index;

  if (in_port_index == -1 || out_port_index == -1) {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->comp, OMX_IndexParamVideoInit,
        &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u",
          (guint) param.nPorts, (guint) param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = OMX_VFPC_OUTPUT_PORT_START_INDEX;
    }
  }

  self->in_port = gst_omx_component_add_port (self->comp, in_port_index);
  if (!self->in_port)
    return FALSE;
  gst_pad_set_element_private (self->sinkpad, self->in_port);

  srcpad = self->srcpads;
  for (i = 0; i < klass->num_outputs; i++) {
    port = gst_omx_component_add_port (self->comp, out_port_index + i);
    if (!port)
      goto port_failed;
    self->out_port = g_list_append (self->out_port, port);
    gst_pad_set_element_private (srcpad->data, port);

    /* Allocate output buffer poool */
    pool = gst_omx_buffer_pool_new (GST_ELEMENT (self), self->comp, port);
    if (!pool)
      goto pool_failed;
    priv->output_pool = g_list_append (priv->output_pool, pool);

    srcpad = srcpad->next;
  }

  return TRUE;

port_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to add outpout port %d",
        out_port_index + i);
    goto cleanup;
  }
pool_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to create output pool");
    if (priv->output_pool)
      g_list_foreach (priv->output_pool, (GFunc) gst_object_unref, NULL);
    priv->output_pool = NULL;
    goto cleanup;
  }
cleanup:
  {
    if (self->out_port)
      g_list_free (self->out_port);
    self->out_port = NULL;
    return FALSE;
  }
}

static gboolean
gst_omx_video_filter_shutdown (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  OMX_STATETYPE state;
  GList *outport, *outpool;

  GST_DEBUG_OBJECT (self, "Shutting down element");

  state = gst_omx_component_get_state (self->comp, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->comp, OMX_StateIdle);
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->comp, OMX_StateLoaded);

    if (priv->output_pool) {
      for (outpool = priv->output_pool; outpool; outpool = outpool->next) {
        gst_buffer_pool_set_active (outpool->data, FALSE);
        GST_OMX_BUFFER_POOL (outpool->data)->deactivated = TRUE;
      }
    }

    gst_omx_port_deallocate_buffers (self->in_port);
    for (outport = self->out_port; outport; outport = outport->next)
      gst_omx_port_deallocate_buffers (outport->data);

    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_video_filter_close (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GST_DEBUG_OBJECT (self, "Closing element");

  if (!gst_omx_video_filter_shutdown (self))
    return FALSE;

  if (priv->output_pool)
    g_list_foreach (priv->output_pool, (GFunc) gst_object_unref, NULL);
  priv->output_pool = NULL;

  self->in_port = NULL;
  if (self->out_port)
    g_list_free (self->out_port);
  self->out_port = NULL;

  if (self->comp)
    gst_omx_component_free (self->comp);
  self->comp = NULL;

  return TRUE;
}

static gboolean
gst_omx_video_filter_start (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *srcpad;

  priv->downstream_flow_ret = GST_FLOW_OK;

  /* Create hash table to manage pending frames per src pad */
  priv->frames = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next)
    g_hash_table_insert (priv->frames, srcpad->data, g_ptr_array_new ());

  return TRUE;
}

static gboolean
gst_omx_video_filter_stop (GstOMXVideoFilter * self)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *srcpad;

  GST_DEBUG_OBJECT (self, "Stopping element");
  gst_omx_video_filter_reset (self);

  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next)
    gst_pad_stop_task (srcpad->data);

  if (gst_omx_component_get_state (self->comp, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->comp, OMX_StateIdle);

  priv->downstream_flow_ret = GST_FLOW_FLUSHING;
  priv->started = FALSE;
  priv->sharing = FALSE;

  gst_omx_component_get_state (self->comp, 5 * GST_SECOND);

  return TRUE;
}

static gboolean
gst_omx_video_filter_fill_buffer (GstOMXVideoFilter * self, GstBuffer * inbuf,
    GstOMXBuffer * outbuf)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstVideoInfo *info = &priv->input_info;
  OMX_PARAM_PORTDEFINITIONTYPE *port_def = &self->in_port->port_def;
  gboolean ret = FALSE;
  GstVideoFrame frame;

  if (info->width != port_def->format.video.nFrameWidth ||
      info->height != port_def->format.video.nFrameHeight) {
    GST_ERROR_OBJECT (self, "Width or height do not match");
    goto done;
  }

  /* Same strides and everything
   * Subtract 512 bytes padding used by the TI VIDENC component  */
  if (gst_buffer_get_size (inbuf) ==
      outbuf->omx_buf->nAllocLen - outbuf->omx_buf->nOffset - 512) {
    outbuf->omx_buf->nFilledLen = gst_buffer_get_size (inbuf);

    if (!priv->sharing) {
      gst_buffer_extract (inbuf, 0,
          outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset,
          outbuf->omx_buf->nFilledLen);
    }
    ret = TRUE;
    goto done;
  }

  /* Different strides */

  switch (info->finfo->format) {
    case GST_VIDEO_FORMAT_I420:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      outbuf->omx_buf->nFilledLen = 0;

      if (!gst_video_frame_map (&frame, info, inbuf, GST_MAP_READ)) {
        GST_ERROR_OBJECT (self, "Invalid input buffer size");
        ret = FALSE;
        break;
      }

      for (i = 0; i < 3; i++) {
        if (i == 0) {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 0);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        } else {
          dest_stride = port_def->format.video.nStride / 2;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 1);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        }

        dest = outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset;
        if (i > 0)
          dest +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;
        if (i == 2)
          dest +=
              (port_def->format.video.nSliceHeight / 2) *
              (port_def->format.video.nStride / 2);

        src = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i);

        if (dest + dest_stride * height >
            outbuf->omx_buf->pBuffer + outbuf->omx_buf->nAllocLen) {
          gst_video_frame_unmap (&frame);
          GST_ERROR_OBJECT (self, "Invalid output buffer size");
          ret = FALSE;
          break;
        }

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          outbuf->omx_buf->nFilledLen += dest_stride;
          src += src_stride;
          dest += dest_stride;
        }
      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    case GST_VIDEO_FORMAT_NV12:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      outbuf->omx_buf->nFilledLen = 0;

      if (!gst_video_frame_map (&frame, info, inbuf, GST_MAP_READ)) {
        GST_ERROR_OBJECT (self, "Invalid input buffer size");
        ret = FALSE;
        break;
      }

      for (i = 0; i < 2; i++) {
        if (i == 0) {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 0);
          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        } else {
          dest_stride = port_def->format.video.nStride;
          src_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, 1);

          /* XXX: Try this if no stride was set */
          if (dest_stride == 0)
            dest_stride = src_stride;
        }

        dest = outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset;
        if (i == 1)
          dest +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;

        src = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i) * (i == 0 ? 1 : 2);

        if (dest + dest_stride * height >
            outbuf->omx_buf->pBuffer + outbuf->omx_buf->nAllocLen) {
          gst_video_frame_unmap (&frame);
          GST_ERROR_OBJECT (self, "Invalid output buffer size");
          ret = FALSE;
          break;
        }

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          outbuf->omx_buf->nFilledLen += dest_stride;
          src += src_stride;
          dest += dest_stride;
        }

      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    default:
      GST_ERROR_OBJECT (self, "Unsupported format");
      goto done;
      break;
  }
done:
  return ret;
}


static gboolean
gst_omx_video_filter_component_init (GstOMXVideoFilter * self, GList * buffers)
{
  GstOMXVideoFilterPrivate *priv = self->priv;
  GList *outport, *srcpad, *outpool;

  GST_DEBUG_OBJECT (self, "Enabling buffers");
  for (outport = self->out_port; outport; outport = outport->next) {
    if (gst_omx_port_set_enabled (outport->data, TRUE) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_port_wait_enabled (outport->data,
            1 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
  }
  if (gst_omx_port_set_enabled (self->in_port, TRUE) != OMX_ErrorNone)
    return FALSE;
  if (gst_omx_port_wait_enabled (self->in_port,
          1 * GST_SECOND) != OMX_ErrorNone)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Changing state to Idle");
  if (gst_omx_component_set_state (self->comp, OMX_StateIdle) != OMX_ErrorNone)
    return FALSE;

  /* Need to allocate buffers to reach Idle state */
  if (!buffers) {
    if (gst_omx_port_allocate_buffers (self->in_port) != OMX_ErrorNone)
      return FALSE;
  } else {
    if (gst_omx_port_use_buffers (self->in_port, buffers) != OMX_ErrorNone) {
      return FALSE;
    }
  }

  for (outport = self->out_port; outport; outport = outport->next) {
    if (gst_omx_port_allocate_buffers (outport->data) != OMX_ErrorNone)
      return FALSE;
  }

  if (gst_omx_component_get_state (self->comp,
          GST_CLOCK_TIME_NONE) != OMX_StateIdle)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Changing state to Executing");
  if (gst_omx_component_set_state (self->comp,
          OMX_StateExecuting) != OMX_ErrorNone)
    return FALSE;

  if (gst_omx_component_get_state (self->comp,
          GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
    return FALSE;

  for (outport = self->out_port; outport; outport = outport->next) {
    if (gst_omx_port_populate (outport->data) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_mark_reconfigured (outport->data) != OMX_ErrorNone)
      return FALSE;
  }

  /* Allocate src buffer pool buffers */
  for (outpool = priv->output_pool; outpool; outpool = outpool->next) {
    GST_OMX_BUFFER_POOL (outpool->data)->allocating = TRUE;
    if (!gst_buffer_pool_set_active (outpool->data, TRUE))
      return FALSE;
    GST_OMX_BUFFER_POOL (outpool->data)->allocating = FALSE;
    GST_OMX_BUFFER_POOL (outpool->data)->deactivated = FALSE;
  }
  /* Start the srcpad loop */
  GST_DEBUG_OBJECT (self, "Starting out pad task");
  priv->downstream_flow_ret = GST_FLOW_OK;

  for (srcpad = self->srcpads; srcpad; srcpad = srcpad->next) {
    gst_pad_start_task (srcpad->data,
        (GstTaskFunction) gst_omx_video_filter_output_loop, srcpad->data, NULL);
  }

  return TRUE;
}

static gboolean
gst_omx_video_filter_use_buffers (GstOMXVideoFilter * self,
    GstOMXMemory * omxmem)
{
  GstOMXPort *mem_port;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  GList *buffers = NULL;
  gint i, n;
  gboolean ret;

  mem_port = omxmem->buf->port;
  n = mem_port->buffers->len;

  /* Set actual buffer count to the port */
  gst_omx_port_get_port_definition (self->in_port, &port_def);
  port_def.nBufferCountActual = n;

  GST_DEBUG_OBJECT (self, "Updating input port buffer count to %lu",
      port_def.nBufferCountActual);
  if (gst_omx_port_update_port_definition (self->in_port,
          &port_def) != OMX_ErrorNone)
    goto reconfigure_error;

  GST_DEBUG_OBJECT (self, "Configuring to use upstream buffers ...");

  for (i = 0; i < n; i++) {
    GstOMXBuffer *buf = g_ptr_array_index (mem_port->buffers, i);
    buffers = g_list_append (buffers, buf->omx_buf->pBuffer);
    GST_LOG_OBJECT (self, "Adding buffer %p to the use list",
        buf->omx_buf->pBuffer);
  }

  ret = gst_omx_video_filter_component_init (self, buffers);

  g_list_free (buffers);

  return ret;

reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure input port"));
    g_list_free (buffers);
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_omx_video_filter_handle_frame (GstOMXVideoFilter * self,
    GstVideoCodecFrame * frame)
{
  GstOMXVideoFilterPrivate *priv = self->priv;

  GstOMXPort *port;
  GstOMXBuffer *buf;
  OMX_ERRORTYPE err;
  GstOMXMemory *omxmem;
  GstOMXAcquireBufferReturn acq_ret = GST_OMX_ACQUIRE_BUFFER_ERROR;

  GST_LOG_OBJECT (self, "Handle frame");

  if (priv->downstream_flow_ret != GST_FLOW_OK) {
    gst_video_codec_frame_unref (frame);
    return priv->downstream_flow_ret;
  }

  if (!priv->started) {
    /* If this buffer has been allocated using the openmax memory management
     * we share the buffers in the pool instead of copy them */
    if (!priv->sharing && gst_buffer_n_memory (frame->input_buffer) == 1
        && (omxmem =
            (GstOMXMemory *) gst_buffer_peek_memory (frame->input_buffer, 0))
        && g_strcmp0 (omxmem->mem.allocator->mem_type, "openmax") == 0) {
      GST_LOG_OBJECT (self, "buffer from an omx pool, writing directly");
      if (gst_omx_video_filter_use_buffers (self, omxmem))
        priv->sharing = TRUE;
      else
        goto init_error;
    } else {
      if (!gst_omx_video_filter_component_init (self, NULL))
        goto init_error;
    }
  }

  port = self->in_port;

  while (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    BufferIdentification *id;

    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);
    acq_ret = gst_omx_port_acquire_buffer (port, &buf);

    if (acq_ret == GST_OMX_ACQUIRE_BUFFER_ERROR) {
      GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);
      goto component_error;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
      GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);
      goto flushing;
    }
    g_assert (acq_ret == GST_OMX_ACQUIRE_BUFFER_OK && buf != NULL);

    GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);

    if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <= 0) {
      gst_omx_port_release_buffer (port, buf);
      goto full_buffer;
    }

    if (priv->downstream_flow_ret != GST_FLOW_OK) {
      gst_omx_port_release_buffer (port, buf);
      goto flow_error;
    }

    /* Now handle the frame */
    GST_LOG_OBJECT (self, "Handling frame");

    if (priv->sharing) {
      omxmem = (GstOMXMemory *) gst_buffer_peek_memory (frame->input_buffer, 0);
      if (buf->omx_buf->pBuffer != omxmem->buf->omx_buf->pBuffer) {
        GST_WARNING_OBJECT (self,
            "OMX input buffer %p and self buffer %p doesn't match",
            omxmem->buf->omx_buf->pBuffer, buf->omx_buf->pBuffer);
      }
    }
    /* Copy the buffer content in chunks of size as requested
     * by the port */
    if (!gst_omx_video_filter_fill_buffer (self, frame->input_buffer, buf)) {
      gst_omx_port_release_buffer (port, buf);
      goto buffer_fill_error;
    }

    id = g_slice_new0 (BufferIdentification);
    id->timestamp = buf->omx_buf->nTimeStamp;
    gst_video_codec_frame_set_user_data (frame, id,
        (GDestroyNotify) buffer_identification_free);

    priv->started = TRUE;
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;

    GST_LOG_OBJECT (self, "Passed frame to component");
  }

  gst_video_codec_frame_unref (frame);

  return priv->downstream_flow_ret;

init_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to initialize OMX component"));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

full_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Got OpenMAX buffer with no free space (%p, %u/%u)", buf,
            (guint) buf->omx_buf->nOffset, (guint) buf->omx_buf->nAllocLen));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

flow_error:
  {
    gst_video_codec_frame_unref (frame);
    return priv->downstream_flow_ret;
  }

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->comp),
            gst_omx_component_get_last_error (self->comp)));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }

flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_FLUSHING;
  }
buffer_fill_error:
  {
    GST_ELEMENT_ERROR (self, RESOURCE, WRITE, (NULL),
        ("Failed to write input into the OpenMAX buffer"));
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_ERROR;
  }
release_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase input buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    return GST_FLOW_ERROR;
  }


}

static GstFlowReturn
gst_omx_video_filter_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (parent);
  GstOMXVideoFilterPrivate *priv = self->priv;
  GstVideoCodecFrame *frame;
  GstClockTime pts, duration;
  GstFlowReturn ret = GST_FLOW_OK;
  guint64 start, stop;

  GST_OMX_VIDEO_FILTER_STREAM_LOCK (self);

  pts = GST_BUFFER_PTS (buf);
  duration = GST_BUFFER_DURATION (buf);

  GST_LOG_OBJECT (self,
      "received buffer of size %" G_GSIZE_FORMAT " with PTS %" GST_TIME_FORMAT
      ", DTS %" GST_TIME_FORMAT ", duration %" GST_TIME_FORMAT,
      gst_buffer_get_size (buf), GST_TIME_ARGS (pts),
      GST_TIME_ARGS (GST_BUFFER_DTS (buf)), GST_TIME_ARGS (duration));

  start = pts;
  if (GST_CLOCK_TIME_IS_VALID (duration))
    stop = start + duration;
  else
    stop = GST_CLOCK_TIME_NONE;

  /* incoming DTS is not really relevant and does not make sense anyway,
   * so pass along _NONE and maybe come up with something better later on */
  frame = gst_omx_video_filter_new_frame (self, buf, start,
      GST_CLOCK_TIME_NONE, stop - start);

  /* Adding the video frame to each src pad pending frames list */
  g_hash_table_foreach (priv->frames, (GHFunc) add_frame_to_pad_array, frame);

  /* new data, more finish needed */
  priv->drained = FALSE;
  ret = gst_omx_video_filter_handle_frame (self, frame);

  GST_OMX_VIDEO_FILTER_STREAM_UNLOCK (self);

  return ret;
}

static GstStateChangeReturn
gst_omx_video_filter_change_state (GstElement * element,
    GstStateChange transition)
{
  GstOMXVideoFilter *self = GST_OMX_VIDEO_FILTER (element);
  GstOMXVideoFilterPrivate *priv = self->priv;

  GList *outport;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  g_return_val_if_fail (GST_IS_OMX_VIDEO_FILTER (element),
      GST_STATE_CHANGE_FAILURE);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      GST_INFO_OBJECT (self, "changing state from NULL to READY");
      if (!gst_omx_video_filter_open (self))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      GST_INFO_OBJECT (self, "changing state from READY to PAUSED");
      priv->downstream_flow_ret = GST_FLOW_OK;

      priv->started = FALSE;
      priv->sharing = FALSE;
      if (!gst_omx_video_filter_start (self))
        goto start_failed;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->in_port)
        gst_omx_port_set_flushing (self->in_port, 5 * GST_SECOND, TRUE);
      for (outport = self->out_port; outport; outport = outport->next)
        gst_omx_port_set_flushing (outport->data, 5 * GST_SECOND, TRUE);

      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_INFO_OBJECT (self, "changing state from PAUSED to READY");
      if (!gst_omx_video_filter_stop (self))
        goto stop_failed;

      if (!gst_omx_video_filter_shutdown (self))
        goto shutdown_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      GST_INFO_OBJECT (self, "changing state from READY to NULL");
      if (!gst_omx_video_filter_close (self))
        goto close_failed;
      break;
    default:
      break;
  }

  return ret;

open_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL), ("Failed to open filter"));
    return GST_STATE_CHANGE_FAILURE;
  }
start_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL), ("Failed to start filter"));
    return GST_STATE_CHANGE_FAILURE;
  }
stop_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL), ("Failed to stop filter"));
    return GST_STATE_CHANGE_FAILURE;
  }

shutdown_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL),
        ("Failed to shutdown OMX component"));
    return GST_STATE_CHANGE_FAILURE;
  }
close_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, INIT, (NULL), ("Failed to close filter"));
    return GST_STATE_CHANGE_FAILURE;
  }
}
