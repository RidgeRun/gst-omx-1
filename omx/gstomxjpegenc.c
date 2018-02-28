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

#include <OMX_Component.h>
#include <OMX_TI_Index.h>
#include <OMX_TI_Video.h>

#include "gstomxjpegenc.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_jpeg_enc_debug_category);
#define GST_CAT_DEFAULT gst_omx_jpeg_enc_debug_category

/* prototypes */
static void gst_omx_jpeg_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_jpeg_enc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_omx_jpeg_enc_set_format (GstOMXVideoEnc * enc,
    GstOMXPort * port, GstVideoCodecState * state);
static GstCaps *gst_omx_jpeg_enc_get_caps (GstOMXVideoEnc * enc,
    GstOMXPort * port, GstVideoCodecState * state);

enum
{
  PROP_0,
  PROP_QUALITY
};

#define GST_OMX_JPEG_ENC_QUALITY_DEFAULT 90

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_jpeg_enc_debug_category, "omxjpegenc", 0, \
      "OMX JPEG encoder element");

G_DEFINE_TYPE_WITH_CODE (GstOMXJpegEnc, gst_omx_jpeg_enc,
    GST_TYPE_OMX_VIDEO_ENC, DEBUG_INIT);

static void
gst_omx_jpeg_enc_class_init (GstOMXJpegEncClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstOMXVideoEncClass *videoenc_class = GST_OMX_VIDEO_ENC_CLASS (klass);

  gobject_class->set_property = gst_omx_jpeg_enc_set_property;
  gobject_class->get_property = gst_omx_jpeg_enc_get_property;

  g_object_class_install_property (gobject_class, PROP_QUALITY,
      g_param_spec_int ("quality", "MJPEG/JPEG quality",
          "MJPEG/JPEG quality (integer 0:min 100:max)",
          0, 100, GST_OMX_JPEG_ENC_QUALITY_DEFAULT, G_PARAM_READWRITE));

  videoenc_class->set_format = GST_DEBUG_FUNCPTR (gst_omx_jpeg_enc_set_format);
  videoenc_class->get_caps = GST_DEBUG_FUNCPTR (gst_omx_jpeg_enc_get_caps);
  videoenc_class->cdata.default_src_template_caps = "image/jpeg, "
      "width=(int) [ 16, 4096 ], " "height=(int) [ 16, 4096 ]";

  gst_element_class_set_static_metadata (element_class,
      "OpenMAX IL MJPEG Video Encoder",
      "Codec/Encoder/Video",
      "Encode MJPEG video streams with OpenMax IL",
      "Melissa Montero <melissa.montero@ridgerun.com>");

  gst_omx_set_default_role (&videoenc_class->cdata, "video_encoder.avc");
}

static void
gst_omx_jpeg_enc_init (GstOMXJpegEnc * self)
{
  self->quality = GST_OMX_JPEG_ENC_QUALITY_DEFAULT;
}

static gboolean
gst_omx_jpeg_enc_set_format (GstOMXVideoEnc * enc, GstOMXPort * port,
    GstVideoCodecState * state)
{
  GstOMXJpegEnc *self = GST_OMX_JPEG_ENC (enc);
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_IMAGE_PARAM_QFACTORTYPE quality_factor;
  OMX_ERRORTYPE err;

  gst_omx_port_get_port_definition (enc->enc_out_port, &port_def);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingMJPEG;
  err =
      gst_omx_port_update_port_definition (GST_OMX_VIDEO_ENC
      (self)->enc_out_port, &port_def);
  if (err != OMX_ErrorNone)
    return FALSE;

  GST_OMX_INIT_STRUCT (&quality_factor);
  quality_factor.nPortIndex = enc->enc_out_port->index;

  err =
      gst_omx_component_get_parameter (GST_OMX_VIDEO_ENC (self)->enc,
      OMX_IndexParamQFactor, &quality_factor);
  if (err != OMX_ErrorNone) {
    GST_WARNING_OBJECT (self,
        "Setting quality factor not supported by component");
    return TRUE;
  }
  quality_factor.nQFactor = self->quality;

  err =
      gst_omx_component_set_parameter (GST_OMX_VIDEO_ENC (self)->enc,
      OMX_IndexParamQFactor, &quality_factor);
  if (err == OMX_ErrorUnsupportedIndex) {
    GST_WARNING_OBJECT (self,
        "Setting quality factor not supported by component");
  } else if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Error setting quality factor %lu: %s (0x%08x)",
        quality_factor.nQFactor, gst_omx_error_to_string (err), err);
    return FALSE;
  }

  GST_INFO_OBJECT (self, "Succesfully setup quality factor %lu",
      quality_factor.nQFactor);

  return TRUE;
}


static GstCaps *
gst_omx_jpeg_enc_get_caps (GstOMXVideoEnc * enc, GstOMXPort * port,
    GstVideoCodecState * state)
{
  GstCaps *caps;

  caps = gst_caps_new_empty_simple ("image/jpeg");

  return caps;
}

static void
gst_omx_jpeg_enc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOMXJpegEnc *self = GST_OMX_JPEG_ENC (object);

  switch (prop_id) {
    case PROP_QUALITY:
      self->quality = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_jpeg_enc_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstOMXJpegEnc *self = GST_OMX_JPEG_ENC (object);

  switch (prop_id) {
    case PROP_QUALITY:
      g_value_set_int (value, self->quality);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
