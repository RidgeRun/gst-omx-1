/*
 * Copyright (C) 2013 RidgerRun LLC
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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "OMX_TI_Common.h"
#include <omx_vfpc.h>
#include <OMX_TI_Index.h>

#include "gstomxscaler.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_scaler_debug_category);
#define GST_CAT_DEFAULT gst_omx_scaler_debug_category

/* prototypes */
static void gst_omx_scaler_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_scaler_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static GstCaps *gst_omx_scaler_transform_caps (GstOMXVideoFilter * self,
    GstPadDirection direction, GstPad * srcpad, GstCaps * caps,
    GstCaps * filter);
static gboolean gst_omx_scaler_set_format (GstOMXVideoFilter * videofilter,
    GstCaps * incaps, GstVideoInfo * ininfo, GList * outcaps_list,
    GList * outinfo_list);

static GstCaps *gst_omx_scaler_fixate_caps (GstOMXVideoFilter * self,
    GstPad * srcpad, GstCaps * sinkcaps, GstCaps * srccaps);
enum
{
  PROP_0,
};

#define GST_OMX_SCALER_MAX_WIDTH    1920
#define GST_OMX_SCALER_MAX_HEIGHT   1080
#define GST_OMX_SCALER_MIN_WIDTH    16
#define GST_OMX_SCALER_MIN_HEIGHT   16

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_scaler_debug_category, "omxscaler", 0, \
      "debug category for gst-omx video scaler");

G_DEFINE_TYPE_WITH_CODE (GstOMXScaler, gst_omx_scaler,
    GST_TYPE_OMX_VIDEO_FILTER, DEBUG_INIT);

static void
gst_omx_scaler_class_init (GstOMXScalerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstOMXVideoFilterClass *videofilter_class =
      GST_OMX_VIDEO_FILTER_CLASS (klass);

  gobject_class->set_property = gst_omx_scaler_set_property;
  gobject_class->get_property = gst_omx_scaler_get_property;

  videofilter_class->num_outputs = 1;
  videofilter_class->cdata.default_src_template_caps = "video/x-raw,"
      "format=(string)YUY2, width=(int) [ 16, 1920 ], "
      "height=(int) [ 16, 1080 ], " "framerate = " GST_VIDEO_FPS_RANGE;
  videofilter_class->cdata.default_sink_template_caps =
      "video/x-raw, " "format=(string)NV12,  width=(int) [ 16, 1920 ], "
      "height = (int) [ 16, 1080 ], framerate = " GST_VIDEO_FPS_RANGE;
  videofilter_class->fixate_caps =
      GST_DEBUG_FUNCPTR (gst_omx_scaler_fixate_caps);
  videofilter_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_omx_scaler_transform_caps);
  videofilter_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_scaler_set_format);
  gst_element_class_set_static_metadata (element_class,
      "OpenMAX Video Scaler",
      "Filter/Encoder/Video",
      "Scale raw video streams",
      "Melissa Montero <melissa.montero@ridgerun.com>");
}

static void
gst_omx_scaler_init (GstOMXScaler * self)
{

}

static void
gst_omx_scaler_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_scaler_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_omx_scaler_set_format (GstOMXVideoFilter * videofilter, GstCaps * incaps,
    GstVideoInfo * ininfo, GList * outcaps_list, GList * outinfo_list)
{
  OMX_PARAM_VFPC_NUMCHANNELPERHANDLE num_channel;
  OMX_CONFIG_VIDCHANNEL_RESOLUTION channel_resolution;
  OMX_CONFIG_ALG_ENABLE alg_enable;
  OMX_ERRORTYPE err;

  GST_DEBUG_OBJECT (videofilter, "setting number of channels");
  GST_OMX_INIT_STRUCT (&num_channel);
  num_channel.nNumChannelsPerHandle = 1;
  err = gst_omx_component_set_parameter (videofilter->comp,
      (OMX_INDEXTYPE) OMX_TI_IndexParamVFPCNumChPerHandle, &num_channel);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (videofilter,
        "Failed to set num of channels: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  GST_DEBUG_OBJECT (videofilter, "setting input resolution");
  GST_OMX_INIT_STRUCT (&channel_resolution);
  channel_resolution.Frm0Width = GST_VIDEO_INFO_WIDTH (ininfo);
  channel_resolution.Frm0Height = GST_VIDEO_INFO_HEIGHT (ininfo);
  channel_resolution.Frm0Pitch = GST_VIDEO_INFO_PLANE_STRIDE (ininfo, 0);
  channel_resolution.Frm1Width = 0;
  channel_resolution.Frm1Height = 0;
  channel_resolution.Frm1Pitch = 0;
  channel_resolution.FrmStartX = 0;
  channel_resolution.FrmStartY = 0;
  channel_resolution.FrmCropWidth = 0;
  channel_resolution.FrmCropHeight = 0;
  channel_resolution.eDir = OMX_DirInput;
  channel_resolution.nChId = 0;
  err = gst_omx_component_set_config (videofilter->comp,
      OMX_TI_IndexConfigVidChResolution, &channel_resolution);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (videofilter,
        "failed to set input channel resolution: %s (0x%08x) ",
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  GST_DEBUG_OBJECT (videofilter, "setting output resolution");
  GST_OMX_INIT_STRUCT (&channel_resolution);
  channel_resolution.Frm0Width =
      GST_VIDEO_INFO_WIDTH ((GstVideoInfo *) outinfo_list->data);
  channel_resolution.Frm0Height =
      GST_VIDEO_INFO_HEIGHT ((GstVideoInfo *) outinfo_list->data);
  channel_resolution.Frm0Pitch =
      GST_VIDEO_INFO_PLANE_STRIDE ((GstVideoInfo *) outinfo_list->data, 0);
  channel_resolution.Frm1Width = 0;
  channel_resolution.Frm1Height = 0;
  channel_resolution.Frm1Pitch = 0;
  channel_resolution.FrmStartX = 0;
  channel_resolution.FrmStartY = 0;
  channel_resolution.FrmCropWidth = 0;
  channel_resolution.FrmCropHeight = 0;
  channel_resolution.eDir = OMX_DirOutput;
  channel_resolution.nChId = 0;
  err = gst_omx_component_set_config (videofilter->comp,
      OMX_TI_IndexConfigVidChResolution, &channel_resolution);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (videofilter,
        "failed to set output channel resolution: %s (0x%08x) ",
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  GST_DEBUG_OBJECT (videofilter, "disable algorithm bypass mode");
  GST_OMX_INIT_STRUCT (&alg_enable);
  alg_enable.nPortIndex = 0;
  alg_enable.nChId = 0;
  alg_enable.bAlgBypass = 0;
  err = gst_omx_component_set_config (videofilter->comp,
      OMX_TI_IndexConfigAlgEnable, &alg_enable);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (videofilter,
        "failed to set disable algorithm bypass mode: %s (0x%08x) ",
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  return TRUE;
}

static GstCaps *
gst_omx_scaler_fixate_caps (GstOMXVideoFilter * self,
    GstPad * srcpad, GstCaps * sinkcaps, GstCaps * srccaps)
{
  GstStructure *structure;
  GstCaps *intersection, *caps;
  gchar *caps_str = NULL;

  /* First try passthrough just change the format */
  caps = gst_caps_copy (sinkcaps);
  structure = gst_caps_get_structure (caps, 0);
  gst_structure_set (structure, "format", G_TYPE_STRING, "YUY2", NULL);
  intersection =
      gst_caps_intersect_full (caps, srccaps, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (caps);
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

static GstCaps *
gst_omx_scaler_transform_caps (GstOMXVideoFilter * videofilter,
    GstPadDirection direction, GstPad * srcpad, GstCaps * caps,
    GstCaps * filter)
{
  GstStructure *structure;
  const GValue *value;
  GstCaps *retcaps;
  guint width, height;
  guint min_width, min_height, max_width, max_height;
  gint n, i;

  retcaps = gst_caps_new_empty ();
  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    structure = gst_caps_get_structure (caps, i);
    /* make copy */
    structure = gst_structure_copy (structure);

    value = gst_structure_get_value (structure, "width");

    if (direction == GST_PAD_SINK) {
      if (G_VALUE_TYPE (value) == G_TYPE_INT)
        width = g_value_get_int (value);
      else if (G_VALUE_TYPE (value) == GST_TYPE_INT_RANGE)
        width = gst_value_get_int_range_min (value);

      if (width > 128)
        min_width = ((width >> 3) + 0x0f) & ~0x0f;
      else
        min_width = GST_OMX_SCALER_MIN_WIDTH;
      max_width = GST_OMX_SCALER_MAX_WIDTH;
    } else {
      if (G_VALUE_TYPE (value) == G_TYPE_INT)
        width = g_value_get_int (value);
      else if (G_VALUE_TYPE (value) == GST_TYPE_INT_RANGE)
        width = gst_value_get_int_range_min (value);

      max_width = ((width * 8) + 0x0f) & ~0x0f;
      if (max_width > GST_OMX_SCALER_MAX_WIDTH)
        max_width = GST_OMX_SCALER_MAX_WIDTH;
      min_width = GST_OMX_SCALER_MIN_WIDTH;
    }

    value = gst_structure_get_value (structure, "height");

    if (direction == GST_PAD_SINK) {
      if (G_VALUE_TYPE (value) == G_TYPE_INT)
        height = g_value_get_int (value);
      else if (G_VALUE_TYPE (value) == GST_TYPE_INT_RANGE)
        height = gst_value_get_int_range_min (value);

      min_height = ((height >> 3) + 0x0f) & ~0x0f;
      if (min_height < GST_OMX_SCALER_MIN_HEIGHT)
        min_height = GST_OMX_SCALER_MIN_HEIGHT;

      max_height = GST_OMX_SCALER_MAX_HEIGHT;
    } else {
      if (G_VALUE_TYPE (value) == G_TYPE_INT)
        height = g_value_get_int (value);
      else if (G_VALUE_TYPE (value) == GST_TYPE_INT_RANGE)
        height = gst_value_get_int_range_min (value);

      max_height = ((height * 8) + 0x0f) & ~0x0f;
      if (max_height > GST_OMX_SCALER_MAX_HEIGHT)
        max_height = GST_OMX_SCALER_MAX_HEIGHT;
      min_height = GST_OMX_SCALER_MIN_HEIGHT;
    }

    gst_structure_set (structure,
        "width", GST_TYPE_INT_RANGE, min_width, GST_OMX_SCALER_MAX_WIDTH,
        "height", GST_TYPE_INT_RANGE, min_height, GST_OMX_SCALER_MAX_HEIGHT,
        NULL);

    if (direction == GST_PAD_SINK)
      gst_structure_set (structure, "format", G_TYPE_STRING, "YUY2", NULL);
    else
      gst_structure_set (structure, "format", G_TYPE_STRING, "NV12", NULL);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (retcaps, structure))
      continue;

    gst_caps_append_structure (retcaps, structure);
  }

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, retcaps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (retcaps);
    retcaps = intersection;
  }

  GST_DEBUG_OBJECT (videofilter, "returning caps: %" GST_PTR_FORMAT, retcaps);

  return retcaps;
}
