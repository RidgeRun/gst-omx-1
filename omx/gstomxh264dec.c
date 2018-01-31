/*
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gstomxh264dec.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_h264_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_h264_dec_debug_category

/* prototypes */
static gboolean gst_omx_h264_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);
static gboolean gst_omx_h264_dec_set_format (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state);

enum
{
  PROP_0
};

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_h264_dec_debug_category, "omxh264dec", 0, \
      "debug category for gst-omx video decoder base class");

G_DEFINE_TYPE_WITH_CODE (GstOMXH264Dec, gst_omx_h264_dec,
    GST_TYPE_OMX_VIDEO_DEC, DEBUG_INIT);

static void
gst_omx_h264_dec_class_init (GstOMXH264DecClass * klass)
{
  GstOMXVideoDecClass *videodec_class = GST_OMX_VIDEO_DEC_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  videodec_class->is_format_change =
      GST_DEBUG_FUNCPTR (gst_omx_h264_dec_is_format_change);
  videodec_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_h264_dec_set_format);

  videodec_class->cdata.default_sink_template_caps = "video/x-h264, "
      "parsed=(boolean) true, "
      "alignment=(string) au, "
      "stream-format=(string) byte-stream, "
      "width=(int) [1,MAX], " "height=(int) [1,MAX]";

  gst_element_class_set_static_metadata (element_class,
      "OpenMAX H.264 Video Decoder",
      "Codec/Decoder/Video",
      "Decode H.264 video streams",
      "Sebastian Dröge <sebastian.droege@collabora.co.uk>");

  gst_omx_set_default_role (&videodec_class->cdata, "video_decoder.avc");
}

static void
gst_omx_h264_dec_init (GstOMXH264Dec * self)
{
}

static gboolean
gst_omx_h264_dec_is_format_change (GstOMXVideoDec * dec,
    GstOMXPort * port, GstVideoCodecState * state)
{
  GstCaps *old_caps = NULL;
  GstCaps *new_caps = state->caps;
  GstStructure *old_structure, *new_structure;
  const gchar *old_profile, *old_level, *new_profile, *new_level;

  if (dec->input_state) {
    old_caps = dec->input_state->caps;
  }

  if (!old_caps) {
    return FALSE;
  }

  old_structure = gst_caps_get_structure (old_caps, 0);
  new_structure = gst_caps_get_structure (new_caps, 0);
  old_profile = gst_structure_get_string (old_structure, "profile");
  old_level = gst_structure_get_string (old_structure, "level");
  new_profile = gst_structure_get_string (new_structure, "profile");
  new_level = gst_structure_get_string (new_structure, "level");

  if (g_strcmp0 (old_profile, new_profile) != 0
      || g_strcmp0 (old_level, new_level) != 0) {
    return TRUE;
  }

  return FALSE;
}

#if 0
static gboolean
gst_omx_h264_dec_set_format (GstOMXVideoDec * dec, GstOMXPort * port,
    GstVideoCodecState * state)
{
  gboolean ret;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;

  gst_omx_port_get_port_definition (port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
  ret = gst_omx_port_update_port_definition (port, &port_def) == OMX_ErrorNone;

  return ret;
}
#else
static gboolean
gst_omx_h264_dec_set_format (GstOMXVideoDec * dec, GstOMXPort * port,
    GstVideoCodecState * state)
{
  GstOMXH264Dec *self = GST_OMX_H264_DEC (dec);
  GstCaps *peercaps;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_VIDEO_PARAM_PROFILELEVELTYPE param;
  OMX_ERRORTYPE err;
  gboolean ret;
  const gchar *profile_string, *level_string;

  gst_omx_port_get_port_definition (port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
  ret = gst_omx_port_update_port_definition (port, &port_def) == OMX_ErrorNone;

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = GST_OMX_VIDEO_DEC (self)->dec_in_port->index;

  err =
      gst_omx_component_get_parameter (GST_OMX_VIDEO_DEC (self)->dec,
      OMX_IndexParamVideoProfileLevelCurrent, &param);
  if (err != OMX_ErrorNone) {
    GST_WARNING_OBJECT (self,
        "Getting profile/level not supported by component");
    return TRUE;
  }

  peercaps = gst_pad_peer_query_caps (GST_VIDEO_DECODER_SINK_PAD (dec),
      gst_pad_get_pad_template_caps (GST_VIDEO_DECODER_SINK_PAD (dec)));
  if (peercaps) {
    GstStructure *s;

    if (gst_caps_is_empty (peercaps)) {
      gst_caps_unref (peercaps);
      GST_ERROR_OBJECT (self, "Empty caps");
      return FALSE;
    }

    s = gst_caps_get_structure (peercaps, 0);
    profile_string = gst_structure_get_string (s, "profile");
    if (profile_string) {
      if (g_str_equal (profile_string, "baseline")) {
        param.eProfile = OMX_VIDEO_AVCProfileBaseline;
      } else if (g_str_equal (profile_string, "main")) {
        param.eProfile = OMX_VIDEO_AVCProfileMain;
      } else if (g_str_equal (profile_string, "high")) {
        param.eProfile = OMX_VIDEO_AVCProfileHigh;
      } else {
        goto unsupported_profile;
      }
    }
    level_string = gst_structure_get_string (s, "level");
    if (level_string) {
      if (g_str_equal (level_string, "1")) {
        param.eLevel = OMX_VIDEO_AVCLevel1;
      } else if (g_str_equal (level_string, "1b")) {
        param.eLevel = OMX_VIDEO_AVCLevel1b;
      } else if (g_str_equal (level_string, "1.1")) {
        param.eLevel = OMX_VIDEO_AVCLevel11;
      } else if (g_str_equal (level_string, "1.2")) {
        param.eLevel = OMX_VIDEO_AVCLevel12;
      } else if (g_str_equal (level_string, "1.3")) {
        param.eLevel = OMX_VIDEO_AVCLevel13;
      } else if (g_str_equal (level_string, "2")) {
        param.eLevel = OMX_VIDEO_AVCLevel2;
      } else if (g_str_equal (level_string, "2.1")) {
        param.eLevel = OMX_VIDEO_AVCLevel21;
      } else if (g_str_equal (level_string, "2.2")) {
        param.eLevel = OMX_VIDEO_AVCLevel22;
      } else if (g_str_equal (level_string, "3")) {
        param.eLevel = OMX_VIDEO_AVCLevel3;
      } else if (g_str_equal (level_string, "3.1")) {
        param.eLevel = OMX_VIDEO_AVCLevel31;
      } else if (g_str_equal (level_string, "3.2")) {
        param.eLevel = OMX_VIDEO_AVCLevel32;
      } else if (g_str_equal (level_string, "4")) {
        param.eLevel = OMX_VIDEO_AVCLevel4;
      } else if (g_str_equal (level_string, "4.1")) {
        param.eLevel = OMX_VIDEO_AVCLevel41;
      } else if (g_str_equal (level_string, "4.2")) {
        param.eLevel = OMX_VIDEO_AVCLevel42;
      } else if (g_str_equal (level_string, "5")) {
        param.eLevel = OMX_VIDEO_AVCLevel5;
      } else if (g_str_equal (level_string, "5.1")) {
        param.eLevel = OMX_VIDEO_AVCLevel51;
      } else {
        goto unsupported_level;
      }
    }
    gst_caps_unref (peercaps);
  }

  err =
      gst_omx_component_set_parameter (GST_OMX_VIDEO_DEC (self)->dec,
      OMX_IndexParamVideoProfileLevelCurrent, &param);
  if (err == OMX_ErrorUnsupportedIndex) {
    GST_WARNING_OBJECT (self,
        "Setting profile/level not supported by component");
  } else if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Error setting profile %u and level %u: %s (0x%08x)",
        (guint) param.eProfile, (guint) param.eLevel,
        gst_omx_error_to_string (err), err);
    return FALSE;
  }

  return ret;

unsupported_profile:
  GST_ERROR_OBJECT (self, "Unsupported profile %s", profile_string);
  gst_caps_unref (peercaps);
  return FALSE;

unsupported_level:
  GST_ERROR_OBJECT (self, "Unsupported level %s", level_string);
  gst_caps_unref (peercaps);
  return FALSE;
}
#endif
