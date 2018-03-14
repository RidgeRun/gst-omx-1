/*
 * Copyright (C) 2018 RidgeRun LLC
 *
 * Author: Jose Lopez <jose.lopez@ridgerun.com>
 *
 * Based on gstomx_tvp.c made by:
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/base.h>
#include <gst/controller/controller.h>

#include <omx_vfcc.h>
#include <OMX_TI_Index.h>
#include <omx_ctrl.h>
#include "gstomxtvp.h"
#include "OMX_TI_Common.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_tvp_debug);
#define GST_CAT_DEFAULT gst_omx_tvp_debug

/* Properties enum */
enum
{
  PROP_0,
  PROP_STANDARD,
  PROP_SCAN_TYPE,
};

#define GST_OMX_TVP_STANDARD_720  720
#define GST_OMX_TVP_STANDARD_1080 1080
#define PROP_STANDARD_DEFAULT     GST_OMX_TVP_STANDARD_1080
#define PROP_SCAN_TYPE_DEFAULT    OMX_VIDEO_CaptureScanTypeProgressive

/* Properties enum types */
#define GST_OMX_TVP_STANDARD_TYPE (gst_omx_tvp_standard_type_get_type())
static GType
gst_omx_tvp_standard_type_get_type (void)
{
  static GType tvp_standard_type = 0;

  static const GEnumValue tvp_standard_types[] = {
    {720, "1280x720p resolution", "720p "},
    {1080, "1920x1080p resolution", "1080p"},
    {0, NULL, NULL}
  };

  if (!tvp_standard_type) {
    tvp_standard_type =
        g_enum_register_static ("GstOMXTvpStandard", tvp_standard_types);
  }
  return tvp_standard_type;
}

#define GST_OMX_TVP_SCAN_TYPE (gst_omx_tvp_scan_type_get_type())
static GType
gst_omx_tvp_scan_type_get_type (void)
{
  static GType scan_type_type = 0;

  static const GEnumValue scan_type_types[] = {
    {OMX_VIDEO_CaptureScanTypeProgressive, "Progressive", "progressive"},
    {OMX_VIDEO_CaptureScanTypeInterlaced, "Interlaced ", "interlaced"},
    {0, NULL, NULL}
  };

  if (!scan_type_type) {
    scan_type_type =
        g_enum_register_static ("GstOMXTvpScanType", scan_type_types);
  }
  return scan_type_type;
}

#define gst_omx_tvp_parent_class parent_class
G_DEFINE_TYPE (GstOMXTvp, gst_omx_tvp, GST_TYPE_BASE_TRANSFORM);

/* prototypes */
static void gst_omx_tvp_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_tvp_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static gboolean gst_omx_tvp_start (GstBaseTransform * base);
static gboolean gst_omx_tvp_stop (GstBaseTransform * base);
static gboolean gst_omx_tvp_configure (GstOMXTvp * self);

/* GObject vmethod implementations */

/* tvp class init */
static void
gst_omx_tvp_class_init (GstOMXTvpClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;

  gobject_class->set_property = gst_omx_tvp_set_property;
  gobject_class->get_property = gst_omx_tvp_get_property;

  g_object_class_install_property (gobject_class, PROP_STANDARD,
      g_param_spec_enum ("standard", "Video standard",
          "Video Standard to use: 1080 or 720",
          GST_OMX_TVP_STANDARD_TYPE, PROP_STANDARD_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SCAN_TYPE,
      g_param_spec_enum ("scan-type", "Scan Type",
          "Video scan type",
          GST_OMX_TVP_SCAN_TYPE, PROP_SCAN_TYPE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_details_simple (gstelement_class,
      "OpenMAX TVP Initializer", "Filter",
      "Initializes TVP hardware for video capture via component",
      "Jose Lopez <jose.lopez@ridgerun.com>");

  GST_BASE_TRANSFORM_CLASS (klass)->start =
      GST_DEBUG_FUNCPTR (gst_omx_tvp_start);

  GST_BASE_TRANSFORM_CLASS (klass)->stop = GST_DEBUG_FUNCPTR (gst_omx_tvp_stop);

  klass->cdata.type = GST_OMX_COMPONENT_TYPE_FILTER;
  klass->cdata.default_src_template_caps = "video/x-raw, "
      "width = " GST_VIDEO_SIZE_RANGE ", " "format = (string) {YUY2, NV12}, "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;
  klass->cdata.default_sink_template_caps = "video/x-raw, "
      "width = " GST_VIDEO_SIZE_RANGE ", " "format = (string) {YUY2, NV12}, "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;
  klass->cdata.component_name = "OMX.TI.VPSSM3.CTRL.TVP";
  klass->cdata.core_name = "/usr/lib/libOMX_Core.so";
  klass->cdata.hacks = GST_OMX_HACK_NO_COMPONENT_ROLE;

  GST_DEBUG_CATEGORY_INIT (gst_omx_tvp_debug, "omxtvp", 0,
      "debug category for gst-omx tvp initializer");
}

/* tvp object init */
static void
gst_omx_tvp_init (GstOMXTvp * self)
{
  self->standard = PROP_STANDARD_DEFAULT;
  self->scan_type = PROP_SCAN_TYPE_DEFAULT;
  self->mode_configured = FALSE;
}

static void
gst_omx_tvp_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOMXTvp *self = GST_OMX_TVP (object);

  switch (prop_id) {
    case PROP_STANDARD:
      self->standard = g_value_get_enum (value);
      break;
    case PROP_SCAN_TYPE:
      self->scan_type = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_tvp_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstOMXTvp *self = GST_OMX_TVP (object);

  switch (prop_id) {
    case PROP_STANDARD:
      g_value_set_enum (value, self->standard);
      break;
    case PROP_SCAN_TYPE:
      g_value_set_enum (value, self->scan_type);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* GstBaseTransform vmethod implementations */

static gboolean
gst_omx_tvp_start (GstBaseTransform * base)
{
  GstOMXTvp *self = GST_OMX_TVP (base);
  GstOMXTvpClass *klass = GST_OMX_TVP_GET_CLASS (base);

  self->comp =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);

  if (!self->comp) {
    GST_ERROR_OBJECT (self, "TVP component creation failed");
    return FALSE;
  }

  if (!gst_omx_tvp_configure (self)) {
    GST_ERROR_OBJECT (self, "TVP component configuration failed");
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_omx_tvp_configure (GstOMXTvp * self)
{
  OMX_PARAM_CTRL_VIDDECODER_INFO viddec_param;
  OMX_STATETYPE state;
  OMX_ERRORTYPE err;

  GST_OMX_INIT_STRUCT (&viddec_param);

  /* Set omx control param according to scan type and standard */
  if (self->scan_type == OMX_VIDEO_CaptureScanTypeProgressive) {
    if (self->standard == GST_OMX_TVP_STANDARD_1080)
      viddec_param.videoStandard = OMX_VIDEO_DECODER_STD_1080P_60;
    else
      viddec_param.videoStandard = OMX_VIDEO_DECODER_STD_720P_60;
  } else {
    viddec_param.videoStandard = OMX_VIDEO_DECODER_STD_1080I_60;
  }

  /* Set omx control param as TVP7002 with autodetect */
  viddec_param.videoDecoderId = OMX_VID_DEC_TVP7002_DRV;
  viddec_param.videoSystemId = OMX_VIDEO_DECODER_VIDEO_SYSTEM_AUTO_DETECT;

  /* FIXME: Even omx returns OMX_ErrorNone apparently the 'set' operation
     is not going through, leaving component with incorrect configuration */
  err = gst_omx_component_set_parameter (self->comp,
      (OMX_INDEXTYPE) OMX_TI_IndexParamCTRLVidDecInfo, &viddec_param);

  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "TVP parameter set failed 0x%X", err);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Changing state to Idle");
  if (gst_omx_component_set_state (self->comp, OMX_StateIdle) != OMX_ErrorNone)
    return FALSE;

  state = gst_omx_component_get_state (self->comp, GST_CLOCK_TIME_NONE);
  GST_DEBUG_OBJECT (self, "Component state 0x%X", state);

  GST_DEBUG_OBJECT (self, "Changing state to Executing");
  if (gst_omx_component_set_state (self->comp,
          OMX_StateExecuting) != OMX_ErrorNone)
    return FALSE;

  state = gst_omx_component_get_state (self->comp, GST_CLOCK_TIME_NONE);
  GST_DEBUG_OBJECT (self, "Component state 0x%X", state);

  if (state != OMX_StateExecuting)
    return FALSE;

  self->mode_configured = TRUE;
  return TRUE;
}

static gboolean
gst_omx_tvp_stop (GstBaseTransform * base)
{
  GstOMXTvp *self = GST_OMX_TVP (base);

  gst_omx_component_free (self->comp);
  self->comp = NULL;

  return TRUE;
}
