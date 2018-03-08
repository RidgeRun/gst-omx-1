/*
 * Copyright (C) 2018 RidgerRun LLC
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

/**
 * SECTION:element-omxcamera
 *
 * omxcamera can be used to capture video from v4l2 devices throught the
 * OMX capture component
 *
 * <refsect2>
 * <title>Example launch lines</title>
 * |[
 * gst-launch-1.0 omxcamera ! fakesink
 * ]| This pipeline shows the video captured from /dev/video0
 * </refsect2>
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <string.h>

#include "OMX_TI_Common.h"
#include <omx_vfcc.h>
#include <OMX_TI_Index.h>

#include "gstomxbufferpool.h"
#include "gstomxcamera.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_camera_debug);
#define GST_CAT_DEFAULT gst_omx_camera_debug


enum
{
  PROP_0,
  PROP_ALWAYS_COPY,
  PROP_NUM_OUT_BUFFERS,
  PROP_INTERFACE,
  PROP_CAPT_MODE,
  PROP_VIP_MODE,
  PROP_SCAN_TYPE,
  PROP_SKIP_FRAMES
};

#define gst_omx_camera_parent_class parent_class
G_DEFINE_TYPE (GstOMXCamera, gst_omx_camera, GST_TYPE_PUSH_SRC);

#define MAX_SHIFTS	30
/* Properties defaults */
#define PROP_ALWAYS_COPY_DEFAULT          FALSE
#define PROP_NUM_OUT_BUFFERS_DEFAULT      5
#define PROP_INTERFACE_DEFAULT            OMX_VIDEO_CaptureHWPortVIP1_PORTA
#define PROP_CAPT_MODE_DEFAULT            OMX_VIDEO_CaptureModeSC_NON_MUX
#define PROP_VIP_MODE_DEFAULT             OMX_VIDEO_CaptureVifMode_16BIT
#define PROP_SCAN_TYPE_DEFAULT            OMX_VIDEO_CaptureScanTypeProgressive
#define PROP_SKIP_FRAMES_DEFAULT          0

/* Properties enumerates */
#define GST_OMX_CAMERA_INTERFACE_TYPE (gst_omx_camera_interface_get_type())
static GType
gst_omx_camera_interface_get_type (void)
{
  static GType interface_type = 0;

  static const GEnumValue interface_types[] = {
    {OMX_VIDEO_CaptureHWPortVIP1_PORTA, "VIP1 port", "vip1"},
    {OMX_VIDEO_CaptureHWPortVIP2_PORTA, "VIP2 port", "vip2"},
    {0, NULL, NULL}
  };

  if (!interface_type) {
    interface_type =
        g_enum_register_static ("GstOMXCameraInterface", interface_types);
  }
  return interface_type;
}

#define GST_OMX_CAMERA_CAPT_MODE_TYPE (gst_omx_camera_capt_mode_get_type())
static GType
gst_omx_camera_capt_mode_get_type (void)
{
  static GType capt_mode_type = 0;

  static const GEnumValue capt_mode_types[] = {
    {OMX_VIDEO_CaptureModeSC_NON_MUX, "Non multiplexed", "nmux"},
    {OMX_VIDEO_CaptureModeMC_LINE_MUX, "Line multiplexed ", "lmux"},
    {OMX_VIDEO_CaptureModeSC_DISCRETESYNC_ACTVID_VSYNC, "Discrete sync",
        "dsync"},
    {0, NULL, NULL}
  };

  if (!capt_mode_type) {
    capt_mode_type =
        g_enum_register_static ("GstOMXCameraCaptMode", capt_mode_types);
  }
  return capt_mode_type;
}

#define GST_OMX_CAMERA_VIP_MODE_TYPE (gst_omx_camera_vip_mode_get_type())
static GType
gst_omx_camera_vip_mode_get_type (void)
{
  static GType vip_mode_type = 0;

  static const GEnumValue vip_mode_types[] = {
    {OMX_VIDEO_CaptureVifMode_08BIT, "8 bits", "8"},
    {OMX_VIDEO_CaptureVifMode_16BIT, "16 bits ", "16"},
    {OMX_VIDEO_CaptureVifMode_24BIT, "24 bits", "24"},
    {0, NULL, NULL}
  };

  if (!vip_mode_type) {
    vip_mode_type =
        g_enum_register_static ("GstOMXCameraVipMode", vip_mode_types);
  }
  return vip_mode_type;
}

#define GST_OMX_CAMERA_SCAN_TYPE (gst_omx_camera_scan_type_get_type())
static GType
gst_omx_camera_scan_type_get_type (void)
{
  static GType scan_type_type = 0;

  static const GEnumValue scan_type_types[] = {
    {OMX_VIDEO_CaptureScanTypeProgressive, "Progressive", "progressive"},
    {OMX_VIDEO_CaptureScanTypeInterlaced, "Interlaced ", "interlaced"},
    {0, NULL, NULL}
  };

  if (!scan_type_type) {
    scan_type_type =
        g_enum_register_static ("GstOMXCameraScanType", scan_type_types);
  }
  return scan_type_type;
}

/* object methods */
static void gst_omx_camera_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_camera_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

/* element methods */
static GstStateChangeReturn gst_omx_camera_change_state (GstElement * element,
    GstStateChange transition);

/* basesrc methods */
static gboolean gst_omx_camera_start (GstBaseSrc * src);
static gboolean gst_omx_camera_stop (GstBaseSrc * src);
static gboolean gst_omx_camera_set_caps (GstBaseSrc * src, GstCaps * caps);

static GstFlowReturn gst_omx_camera_create (GstPushSrc * src, GstBuffer ** out);
static GstCaps *gst_omx_camera_fixate (GstBaseSrc * basesrc, GstCaps * caps);
static gboolean gst_omx_camera_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_omx_camera_negotiate (GstBaseSrc * basesrc);

static gboolean gst_omx_camera_shutdown (GstOMXCamera * self);

OMX_COLOR_FORMATTYPE gst_omx_camera_get_color_format (GstVideoFormat format);
gint gst_omx_camera_get_buffer_size (GstVideoFormat format, gint stride,
    gint height);

static void
gst_omx_camera_class_init (GstOMXCameraClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSrcClass *basesrc_class = GST_BASE_SRC_CLASS (klass);
  GstPushSrcClass *pushsrc_class = GST_PUSH_SRC_CLASS (klass);

  gobject_class->set_property = gst_omx_camera_set_property;
  gobject_class->get_property = gst_omx_camera_get_property;

  g_object_class_install_property (gobject_class, PROP_NUM_OUT_BUFFERS,
      g_param_spec_uint ("output-buffers", "Output buffers",
          "The number of OMX output buffers",
          5, 32, PROP_NUM_OUT_BUFFERS_DEFAULT, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_INTERFACE,
      g_param_spec_enum ("interface", "Interface",
          "The video input interface from where image/video is obtained",
          GST_OMX_CAMERA_INTERFACE_TYPE, PROP_INTERFACE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CAPT_MODE,
      g_param_spec_enum ("capt-mode", "Capture mode",
          "The video input multiplexed mode",
          GST_OMX_CAMERA_CAPT_MODE_TYPE, PROP_CAPT_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_VIP_MODE,
      g_param_spec_enum ("vip-mode", "VIP mode",
          "VIP port split configuration",
          GST_OMX_CAMERA_VIP_MODE_TYPE, PROP_VIP_MODE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SCAN_TYPE,
      g_param_spec_enum ("scan-type", "Scan Type",
          "Video scan type",
          GST_OMX_CAMERA_SCAN_TYPE, PROP_SCAN_TYPE_DEFAULT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ALWAYS_COPY,
      g_param_spec_boolean ("always-copy", "Always copy",
          "If the output buffer should be copied or should use the OpenMax buffer",
          PROP_ALWAYS_COPY_DEFAULT, G_PARAM_WRITABLE));

  g_object_class_install_property (gobject_class, PROP_SKIP_FRAMES,
      g_param_spec_uint ("skip-frames", "Skip Frames",
          "Skip this amount of frames after a vaild frame",
          0, 30, PROP_SKIP_FRAMES_DEFAULT, G_PARAM_READWRITE));

  element_class->change_state = gst_omx_camera_change_state;

  gst_element_class_set_static_metadata (element_class,
      "OpenMAX Video Source", "Source/Video",
      "Reads frames from a camera device",
      "Melissa Montero <melissa.montero@uridgerun.com>");

  basesrc_class->set_caps = GST_DEBUG_FUNCPTR (gst_omx_camera_set_caps);
  basesrc_class->start = GST_DEBUG_FUNCPTR (gst_omx_camera_start);
  basesrc_class->stop = GST_DEBUG_FUNCPTR (gst_omx_camera_stop);
  basesrc_class->fixate = GST_DEBUG_FUNCPTR (gst_omx_camera_fixate);
  basesrc_class->event = GST_DEBUG_FUNCPTR (gst_omx_camera_event);
  basesrc_class->negotiate = GST_DEBUG_FUNCPTR (gst_omx_camera_negotiate);

  pushsrc_class->create = GST_DEBUG_FUNCPTR (gst_omx_camera_create);

  klass->cdata.type = GST_OMX_COMPONENT_TYPE_SOURCE;
  klass->cdata.default_src_template_caps = "video/x-raw, "
      "width = " GST_VIDEO_SIZE_RANGE ", " "format = (string) {YUY2, NV12}, "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;
  klass->cdata.component_name = "OMX.TI.VPSSM3.VFCC";
  klass->cdata.core_name = "/usr/lib/libOMX_Core.so";
  klass->cdata.in_port_index = -1;
  klass->cdata.out_port_index = OMX_VFCC_OUTPUT_PORT_START_INDEX;
  klass->cdata.hacks = GST_OMX_HACK_NO_COMPONENT_ROLE;

  GST_DEBUG_CATEGORY_INIT (gst_omx_camera_debug, "omxcamera", 0,
      "OMX video source element");
}

static void
gst_omx_camera_init (GstOMXCamera * self)
{
  gst_base_src_set_format (GST_BASE_SRC (self), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (self), TRUE);

  self->started = FALSE;
  self->sharing = FALSE;
  self->outport = NULL;

  /* Initialize properties */
  self->interface = PROP_INTERFACE_DEFAULT;
  self->capt_mode = PROP_CAPT_MODE_DEFAULT;
  self->vip_mode = PROP_VIP_MODE_DEFAULT;
  self->scan_type = PROP_SCAN_TYPE_DEFAULT;
  self->always_copy = PROP_ALWAYS_COPY_DEFAULT;
  self->num_buffers = PROP_NUM_OUT_BUFFERS_DEFAULT;
  self->skip_frames = PROP_SKIP_FRAMES_DEFAULT;
}

static void
gst_omx_camera_set_skip_frames (GstOMXCamera * self)
{
  OMX_ERRORTYPE err;
  OMX_CONFIG_VFCC_FRAMESKIP_INFO skip_frames;
  guint32 shifts = 0, skip = 0, i = 0, count = 0;
  shifts = self->skip_frames;

  if (shifts) {
    while (count < MAX_SHIFTS) {
      if ((count + shifts) > MAX_SHIFTS)
        shifts = MAX_SHIFTS - count;

      for (i = 0; i < shifts; i++) {
        skip = (skip << 1) | 1;
        count++;
      }

      if (count < MAX_SHIFTS) {
        skip = skip << 1;
        count++;
      }
    }
  }

  GST_OMX_INIT_STRUCT (&skip_frames);
  /* OMX_TI_IndexConfigVFCCFrameSkip is for dropping frames in capture,
     it is a binary 30bit value where 1 means drop a frame and 0
     process the frame */
  skip_frames.frameSkipMask = skip;
  err =
      gst_omx_component_set_config (self->comp,
      OMX_TI_IndexConfigVFCCFrameSkip, &skip_frames);
  if (err != OMX_ErrorNone)
    GST_WARNING_OBJECT (self,
        "Failed to set capture skip frames to %d: %s (0x%08x)", shifts,
        gst_omx_error_to_string (err), err);

  return;
}

static void
gst_omx_camera_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstOMXCamera *self = GST_OMX_CAMERA (object);

  switch (prop_id) {
    case PROP_INTERFACE:
      self->interface = g_value_get_enum (value);
      break;
    case PROP_CAPT_MODE:
      self->capt_mode = g_value_get_enum (value);
      break;
    case PROP_VIP_MODE:
      self->vip_mode = g_value_get_enum (value);
      break;
    case PROP_SCAN_TYPE:
      self->scan_type = g_value_get_enum (value);
      break;
    case PROP_ALWAYS_COPY:
      self->always_copy = g_value_get_boolean (value);
      break;
    case PROP_NUM_OUT_BUFFERS:
      self->num_buffers = g_value_get_uint (value);
      break;
    case PROP_SKIP_FRAMES:
      self->skip_frames = g_value_get_uint (value);
      if (self->comp)
        gst_omx_camera_set_skip_frames (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_omx_camera_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstOMXCamera *self = GST_OMX_CAMERA (object);
  OMX_ERRORTYPE err;

  switch (prop_id) {
    case PROP_INTERFACE:
      g_value_set_enum (value, self->interface);
      break;
    case PROP_CAPT_MODE:
      g_value_set_enum (value, self->capt_mode);
      break;
    case PROP_VIP_MODE:
      g_value_set_enum (value, self->vip_mode);
      break;
    case PROP_SCAN_TYPE:
      g_value_set_enum (value, self->scan_type);
      break;
    case PROP_ALWAYS_COPY:
      g_value_set_boolean (value, self->always_copy);
      break;
    case PROP_NUM_OUT_BUFFERS:
      g_value_set_uint (value, self->num_buffers);
      break;
    case PROP_SKIP_FRAMES:
    {
      if (self->comp) {
        OMX_CONFIG_VFCC_FRAMESKIP_INFO skip_frames;

        err =
            gst_omx_component_set_config (self->comp,
            OMX_TI_IndexConfigVFCCFrameSkip, &skip_frames);
        if (err != OMX_ErrorNone)
          GST_ERROR_OBJECT (self,
              "Failed to get capture skip frames: %s (0x%08x)",
              gst_omx_error_to_string (err), err);

        g_value_set_uint (value, skip_frames.frameSkipMask);
      } else {
        g_value_set_uint (value, self->skip_frames);
      }
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Following caps negotiation related functions were taken from the
 * omx_camera element code */

/* this function is a bit of a last resort */
static GstCaps *
gst_omx_camera_fixate (GstBaseSrc * basesrc, GstCaps * caps)
{
  GstStructure *structure;
  gint i;

  GST_DEBUG_OBJECT (basesrc, "fixating caps %" GST_PTR_FORMAT, caps);

  caps = gst_caps_make_writable (caps);

  for (i = 0; i < gst_caps_get_size (caps); ++i) {
    structure = gst_caps_get_structure (caps, i);

    /* We are fixating to a resonable 320x200 resolution
       and the maximum framerate resolution for that size */
    gst_structure_fixate_field_nearest_int (structure, "width", 320);
    gst_structure_fixate_field_nearest_int (structure, "height", 240);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate",
        G_MAXINT, 1);
    gst_structure_fixate_field (structure, "format");
  }

  GST_DEBUG_OBJECT (basesrc, "fixated caps %" GST_PTR_FORMAT, caps);

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (basesrc, caps);

  return caps;
}

static gboolean
gst_omx_camera_event (GstBaseSrc * src, GstEvent * event)
{
  GST_DEBUG_OBJECT (src, "handle event %" GST_PTR_FORMAT, event);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_RECONFIGURE:
      gst_pad_check_reconfigure (GST_BASE_SRC_PAD (src));
      break;
    default:
      break;
  }
  return GST_BASE_SRC_CLASS (parent_class)->event (src, event);
}


static gboolean
gst_omx_camera_negotiate (GstBaseSrc * basesrc)
{
  GstCaps *thiscaps = NULL;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;

  GST_DEBUG_OBJECT (basesrc, "negotiate caps: %" GST_PTR_FORMAT, thiscaps);
  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);

  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL || gst_caps_is_any (thiscaps))
    goto no_nego_needed;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps && !gst_caps_is_any (peercaps)) {
    GstCaps *icaps = NULL;
    int i;

    /* Prefer the first caps we are compatible with that the peer proposed */
    for (i = 0; i < gst_caps_get_size (peercaps); i++) {
      /* get intersection */
      GstCaps *ipcaps = gst_caps_copy_nth (peercaps, i);

      GST_DEBUG_OBJECT (basesrc, "peer: %" GST_PTR_FORMAT, ipcaps);

      icaps = gst_caps_intersect (thiscaps, ipcaps);
      gst_caps_unref (ipcaps);

      if (!gst_caps_is_empty (icaps))
        break;

      gst_caps_unref (icaps);
      icaps = NULL;
    }

    GST_DEBUG_OBJECT (basesrc, "intersect: %" GST_PTR_FORMAT, icaps);
    if (icaps) {
      /* If there are multiple intersections pick the one with the smallest
       * resolution strictly bigger than the first peer caps */
      if (gst_caps_get_size (icaps) > 1) {
        GstStructure *s = gst_caps_get_structure (peercaps, 0);
        int best = 0;
        int twidth, theight;
        int width = G_MAXINT, height = G_MAXINT;

        if (gst_structure_get_int (s, "width", &twidth)
            && gst_structure_get_int (s, "height", &theight)) {

          /* Walk the structure backwards to get the first entry of the
           * smallest resolution bigger (or equal to) the preferred resolution)
           */
          for (i = gst_caps_get_size (icaps) - 1; i >= 0; i--) {
            GstStructure *is = gst_caps_get_structure (icaps, i);
            int w, h;

            if (gst_structure_get_int (is, "width", &w)
                && gst_structure_get_int (is, "height", &h)) {
              if (w >= twidth && w <= width && h >= theight && h <= height) {
                width = w;
                height = h;
                best = i;
              }
            }
          }
        }

        caps = gst_caps_copy_nth (icaps, best);
        gst_caps_unref (icaps);
      } else {
        caps = icaps;
      }
    }
    gst_caps_unref (thiscaps);
  } else {
    /* no peer or peer have ANY caps, work with our own caps then */
    caps = thiscaps;
  }
  if (peercaps)
    gst_caps_unref (peercaps);
  if (caps) {
    caps = gst_caps_truncate (caps);

    /* now fixate */
    if (!gst_caps_is_empty (caps)) {
      caps = gst_omx_camera_fixate (basesrc, caps);
      GST_DEBUG_OBJECT (basesrc, "fixated to: %" GST_PTR_FORMAT, caps);

      if (gst_caps_is_any (caps)) {
        /* hmm, still anything, so element can do anything and
         * nego is not needed */
        result = TRUE;
      } else if (gst_caps_is_fixed (caps)) {
        /* yay, fixed caps, use those then */
        result = gst_base_src_set_caps (basesrc, caps);
      }
    }
    gst_caps_unref (caps);
  }
  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
}

OMX_COLOR_FORMATTYPE
gst_omx_camera_get_color_format (GstVideoFormat format)
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
gst_omx_camera_get_buffer_size (GstVideoFormat format, gint stride, gint height)
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
gst_omx_camera_drain (GstOMXCamera * self)
{
  if (self->outport)
    gst_omx_port_set_flushing (self->outport, 5 * GST_SECOND, TRUE);

  if (!gst_omx_camera_stop (GST_BASE_SRC (self)))
    return FALSE;

  if (!gst_omx_camera_shutdown (self))
    return FALSE;
  GST_DEBUG_OBJECT (self, "OMX camera drained and disabled");

  return TRUE;
}

static gboolean
gst_omx_camera_set_format (GstOMXCamera * self, GstCaps * caps,
    GstVideoInfo * info)
{

  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
  OMX_PARAM_BUFFER_MEMORYTYPE mem_type;
  OMX_PARAM_VFCC_HWPORT_PROPERTIES hw_port_param;
  OMX_PARAM_VFCC_HWPORT_ID hw_port;
  OMX_ERRORTYPE err;
  GstStructure *config;

  GST_DEBUG_OBJECT (self, "Setting new format");

  needs_disable =
      gst_omx_component_get_state (self->comp,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;

  /* If the component is not in Loaded state and a real format change happens
   * we have to teardown the OMX comoponent.
   */
  if (needs_disable) {
    GST_DEBUG_OBJECT (self, "Need to disable and drain element");
    if (!gst_omx_camera_drain (self))
      goto drain_failed;
  }

  gst_omx_port_get_port_definition (self->outport, &port_def);
  port_def.format.video.nFrameWidth = GST_VIDEO_INFO_WIDTH (info);
  port_def.format.video.nFrameHeight = GST_VIDEO_INFO_HEIGHT (info);
  port_def.format.video.nStride = GST_VIDEO_INFO_PLANE_STRIDE (info, 0);
  port_def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
  port_def.format.video.eColorFormat =
      gst_omx_camera_get_color_format (info->finfo->format);
  if (port_def.format.video.eColorFormat == OMX_COLOR_FormatUnused) {
    GST_ERROR_OBJECT (self, "Unsupported format %s",
        gst_video_format_to_string (info->finfo->format));
    return FALSE;
  }
  port_def.nBufferSize = self->imagesize =
      gst_omx_camera_get_buffer_size (info->finfo->format,
      GST_VIDEO_INFO_PLANE_STRIDE (info, 0),
      port_def.format.video.nFrameHeight);
  port_def.nBufferCountActual = self->num_buffers;

  GST_DEBUG_OBJECT (self, "Updating outport port definition");
  if (gst_omx_port_update_port_definition (self->outport,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  GST_DEBUG_OBJECT (self,
      "width= %li, height=%li, stride=%li, format %d, buffersize %li",
      port_def.format.video.nFrameWidth, port_def.format.video.nFrameHeight,
      port_def.format.video.nStride, port_def.format.video.eColorFormat,
      port_def.nBufferSize);

  /* Set memory type on ports to raw memory */
  GST_OMX_INIT_STRUCT (&mem_type);
  mem_type.nPortIndex = self->outport->index;
  mem_type.eBufMemoryType = OMX_BUFFER_MEMORY_DEFAULT;
  err = gst_omx_component_set_parameter (self->comp,
      OMX_TI_IndexParamBuffMemType, &mem_type);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Failed to set memory type on port %lu: %s (0x%08x)",
        mem_type.nPortIndex, gst_omx_error_to_string (err), err);
    return FALSE;
  }
  GST_DEBUG_OBJECT (self, "Memory type: %d", mem_type.eBufMemoryType);

  GST_OMX_INIT_STRUCT (&hw_port);
  /* Set capture interface */
  hw_port.eHwPortId = self->interface;
  err = gst_omx_component_set_parameter (self->comp,
      OMX_TI_IndexParamVFCCHwPortID, &hw_port);
  GST_DEBUG_OBJECT (self, "Hardware port id: %d", hw_port.eHwPortId);

  GST_OMX_INIT_STRUCT (&hw_port_param);
  hw_port_param.eCaptMode = self->capt_mode;
  hw_port_param.eVifMode = self->vip_mode;
  hw_port_param.eInColorFormat = OMX_COLOR_FormatYCbYCr;
  hw_port_param.eScanType = self->scan_type;
  hw_port_param.nMaxHeight = GST_VIDEO_INFO_HEIGHT (info);
  hw_port_param.nMaxWidth = GST_VIDEO_INFO_WIDTH (info);
  hw_port_param.nMaxChnlsPerHwPort = 1;
  if (self->scan_type == OMX_VIDEO_CaptureScanTypeInterlaced)
    hw_port_param.nMaxHeight = hw_port_param.nMaxHeight >> 1;

  err = gst_omx_component_set_parameter (self->comp,
      OMX_TI_IndexParamVFCCHwPortProperties, &hw_port_param);

  GST_DEBUG_OBJECT (self,
      "Hw port properties: capture mode %d, vif mode %d, max height %li, max width %li, max channel %li, scan type %d, format %d",
      hw_port_param.eCaptMode, hw_port_param.eVifMode, hw_port_param.nMaxHeight,
      hw_port_param.nMaxWidth, hw_port_param.nMaxChnlsPerHwPort,
      hw_port_param.eScanType, hw_port_param.eInColorFormat);

  gst_omx_camera_set_skip_frames (self);

  /* Configure output pool */
  config = gst_buffer_pool_get_config (self->outpool);
  gst_buffer_pool_config_set_params (config, caps,
      port_def.nBufferSize, port_def.nBufferCountActual,
      port_def.nBufferCountActual);
  if (!gst_buffer_pool_set_config (self->outpool, config))
    goto config_pool_failed;

  /* Calculate duration based on the framerate to use it in case
   * nTickCount is not provided by the OMX component */
  self->duration = gst_util_uint64_scale_int (GST_SECOND,
      GST_VIDEO_INFO_FPS_D (info), GST_VIDEO_INFO_FPS_N (info));

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

static gboolean
gst_omx_camera_set_caps (GstBaseSrc * src, GstCaps * caps)
{
  GstOMXCamera *self = GST_OMX_CAMERA (src);
  GstVideoInfo info;
  gchar *caps_str = NULL;

  GST_INFO_OBJECT (self, "set caps (%" GST_PTR_FORMAT "): %s", caps, caps_str =
      gst_caps_to_string (caps));
  if (caps_str)
    g_free (caps_str);

  gst_video_info_init (&info);
  if (G_UNLIKELY (!gst_video_info_from_caps (&info, caps)))
    return FALSE;

  if (!gst_omx_camera_set_format (self, caps, &info))
    return FALSE;

  return TRUE;
}


static gboolean
gst_omx_camera_start (GstBaseSrc * bsrc)
{
  return TRUE;
}

static gboolean
gst_omx_camera_stop (GstBaseSrc * bsrc)
{
  GstOMXCamera *self = GST_OMX_CAMERA (bsrc);

  GST_DEBUG_OBJECT (self, "Stopping element");

  if (gst_omx_component_get_state (self->comp, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->comp, OMX_StateIdle);

  self->started = FALSE;
  self->sharing = FALSE;

  gst_omx_component_get_state (self->comp, 5 * GST_SECOND);

  return TRUE;
}

static gboolean
gst_omx_camera_open (GstOMXCamera * self)
{
  GstOMXCameraClass *klass = GST_OMX_CAMERA_GET_CLASS (self);
  GstBufferPool *pool;
  gint port_index;

  self->started = FALSE;
  self->sharing = FALSE;

  GST_DEBUG_OBJECT (self, "Opening component %s", klass->cdata.component_name);
  self->comp =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);

  if (!self->comp)
    return FALSE;

  if (gst_omx_component_get_state (self->comp,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  port_index = klass->cdata.out_port_index;
  self->outport = gst_omx_component_add_port (self->comp, port_index);
  if (!self->outport)
    goto port_failed;

  gst_pad_set_element_private (GST_BASE_SRC_PAD (self), self->outport);

  /* Allocate output buffer pool */
  pool =
      gst_omx_buffer_pool_new (GST_ELEMENT (self), self->comp, self->outport);
  if (!pool)
    goto pool_failed;
  self->outpool = pool;

  GST_INFO_OBJECT (self, "Opened component %s", klass->cdata.component_name);
  return TRUE;

port_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to add output port %d", port_index);
    goto cleanup;
  }
pool_failed:
  {
    GST_ERROR_OBJECT (self, "Failed to create output pool");
    goto cleanup;
  }
cleanup:
  {
    if (self->comp)
      gst_omx_component_free (self->comp);
    self->comp = NULL;
    return FALSE;
  }
}

static gboolean
gst_omx_camera_shutdown (GstOMXCamera * self)
{
  OMX_STATETYPE state;

  GST_DEBUG_OBJECT (self, "Shutting down OMX camera");

  state = gst_omx_component_get_state (self->comp, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    /* Changing states to Loaded */
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->comp, OMX_StateIdle);
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->comp, OMX_StateLoaded);

    /* Deallocating buffers to allow state change */
    if (self->outpool) {
      gst_buffer_pool_set_active (self->outpool, FALSE);
      GST_OMX_BUFFER_POOL (self->outpool)->deactivated = TRUE;
    }
    gst_omx_port_deallocate_buffers (self->outport);

    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->comp, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_camera_close (GstOMXCamera * self)
{
  GstOMXCameraClass *klass = GST_OMX_CAMERA_GET_CLASS (self);

  GST_DEBUG_OBJECT (self, "Closing OMX camera");
  if (!gst_omx_camera_shutdown (self))
    return FALSE;

  /* Free resources and OMX component */
  if (self->outpool) {
    gst_object_unref (self->outpool);
    self->outpool = NULL;
  }

  if (self->comp) {
    gst_omx_component_free (self->comp);
    self->comp = NULL;
    self->outport = NULL;
  }

  GST_INFO_OBJECT (self, "Closed component %s", klass->cdata.component_name);
  return TRUE;
}


static GstStateChangeReturn
gst_omx_camera_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
  GstOMXCamera *self = GST_OMX_CAMERA (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      /* open the component */
      if (!gst_omx_camera_open (self))
        return GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_NULL:
      /* close the component */
      if (!gst_omx_camera_close (self))
        return GST_STATE_CHANGE_FAILURE;

      break;
    default:
      break;
  }

  return ret;
}

static GstFlowReturn
gst_omx_camera_get_buffer (GstOMXCamera * self, GstBuffer ** outbuf)
{
  GstOMXPort *port = self->outport;
  GstOMXBuffer *buf = NULL;
  GstOMXAcquireBufferReturn acq_return;
  OMX_ERRORTYPE err;
  GstBufferPool *pool = self->outpool;
  GstFlowReturn flow_ret;

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK) {
    return GST_FLOW_ERROR;
  }

  if (gst_omx_port_is_flushing (port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (port, buf);
    goto flushing;
  }

  GST_LOG_OBJECT (self, "Handling buffer: 0x%08x %" G_GUINT64_FORMAT,
      (guint) buf->omx_buf->nFlags, (guint64) buf->omx_buf->nTimeStamp);

  buf->omx_buf->nFilledLen = self->imagesize;
  if (buf->omx_buf->nFilledLen > 0) {
    GstMapInfo map = GST_MAP_INFO_INIT;

    GST_LOG_OBJECT (self, "Handling output data");

    if (self->always_copy) {
      *outbuf = gst_buffer_new_and_alloc (buf->omx_buf->nFilledLen);

      gst_buffer_map (*outbuf, &map, GST_MAP_WRITE);
      memcpy (map.data,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          buf->omx_buf->nFilledLen);
      gst_buffer_unmap (*outbuf, &map);

      err = gst_omx_port_release_buffer (port, buf);
      if (err != OMX_ErrorNone)
        goto release_error;
    } else {
      GstBufferPoolAcquireParams params = { 0, };
      gint n = port->buffers->len;
      GstMemory *mem;
      gint i;

      for (i = 0; i < n; i++) {
        GstOMXBuffer *tmp = g_ptr_array_index (port->buffers, i);

        if (tmp == buf)
          break;
      }
      g_assert (i != n);

      GST_OMX_BUFFER_POOL (pool)->current_buffer_index = i;
      flow_ret = gst_buffer_pool_acquire_buffer (pool, outbuf, &params);
      if (flow_ret != GST_FLOW_OK) {
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
      mem = gst_buffer_get_memory (*outbuf, 0);
      gst_memory_unref (mem);
    }
  } else {
    *outbuf = gst_buffer_new ();
  }

  GST_BUFFER_TIMESTAMP (*outbuf) =
      gst_util_uint64_scale (buf->omx_buf->nTimeStamp, GST_SECOND,
      OMX_TICKS_PER_SECOND) * 1000;

  if (buf->omx_buf->nTickCount != 0)
    GST_BUFFER_DURATION (*outbuf) =
        gst_util_uint64_scale (buf->omx_buf->nTickCount, GST_SECOND,
        OMX_TICKS_PER_SECOND);
  else
    GST_BUFFER_DURATION (*outbuf) = self->duration;

  GST_DEBUG_OBJECT (self,
      "Got buffer from component: %p with timestamp %" GST_TIME_FORMAT
      " duration %" GST_TIME_FORMAT, outbuf,
      GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (*outbuf)),
      GST_TIME_ARGS (GST_BUFFER_DURATION (*outbuf)));
  return GST_FLOW_OK;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->comp),
            gst_omx_component_get_last_error (self->comp)));
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    self->started = FALSE;
    return GST_FLOW_ERROR;
  }
flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing");
    self->started = FALSE;
    return GST_FLOW_FLUSHING;
  }
invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Cannot acquire output buffer from pool"));
    return flow_ret;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (GST_BASE_SRC_PAD (self), gst_event_new_eos ());
    self->started = FALSE;
    return GST_FLOW_ERROR;
  }
}


static gboolean
gst_omx_camera_component_init (GstOMXCamera * self, GList * buffers)
{
  GST_DEBUG_OBJECT (self, "Enabling buffers");
  if (gst_omx_port_set_enabled (self->outport, TRUE) != OMX_ErrorNone)
    return FALSE;

  if (gst_omx_port_wait_enabled (self->outport,
          1 * GST_SECOND) != OMX_ErrorNone)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Changing state to Idle");
  if (gst_omx_component_set_state (self->comp, OMX_StateIdle) != OMX_ErrorNone)
    return FALSE;

  /* Need to allocate buffers to reach Idle state */
  if (!buffers) {
    if (gst_omx_port_allocate_buffers (self->outport) != OMX_ErrorNone)
      return FALSE;
  } else {
    if (gst_omx_port_use_buffers (self->outport, buffers) != OMX_ErrorNone) {
      return FALSE;
    }
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

  gst_omx_port_set_flushing (self->outport, 5 * GST_SECOND, FALSE);

  if (gst_omx_port_populate (self->outport) != OMX_ErrorNone)
    return FALSE;
  if (gst_omx_port_mark_reconfigured (self->outport) != OMX_ErrorNone)
    return FALSE;

  /* Allocate src buffer pool buffers */
  GST_OMX_BUFFER_POOL (self->outpool)->allocating = TRUE;
  if (!gst_buffer_pool_set_active (self->outpool, TRUE))
    return FALSE;
  GST_OMX_BUFFER_POOL (self->outpool)->allocating = FALSE;
  GST_OMX_BUFFER_POOL (self->outpool)->deactivated = FALSE;


  return TRUE;
}


static GstFlowReturn
gst_omx_camera_create (GstPushSrc * src, GstBuffer ** buf)
{
  GstOMXCamera *self = GST_OMX_CAMERA (src);
  GstFlowReturn ret;
  GstClock *clock;
  GstClockTime abs_time, base_time, timestamp;

  /* timestamps, LOCK to get clock and base time. */
  GST_OBJECT_LOCK (self);
  if ((clock = GST_ELEMENT_CLOCK (self))) {
    /* we have a clock, get base time and ref clock */
    base_time = GST_ELEMENT (self)->base_time;
    abs_time = gst_clock_get_time (clock);
  } else {
    /* no clock, can't set timestamps */
    base_time = GST_CLOCK_TIME_NONE;
    abs_time = GST_CLOCK_TIME_NONE;
  }
  GST_OBJECT_UNLOCK (self);

  if (!self->started) {
    if (!gst_omx_camera_component_init (self, NULL)) {
      ret = GST_FLOW_ERROR;
      goto error;
    }
  }

  ret = gst_omx_camera_get_buffer (self, buf);
  if (G_UNLIKELY (ret != GST_FLOW_OK))
    goto error;

  timestamp = GST_BUFFER_TIMESTAMP (*buf);

  if (!self->started) {
    self->running_time = abs_time - base_time;
    if (!self->running_time)
      self->running_time = timestamp;
    self->omx_delay = timestamp - self->running_time;

    GST_DEBUG_OBJECT (self, "OMX delay %" G_GINT64_FORMAT, self->omx_delay);
    self->started = TRUE;
  }

  /* set buffer metadata */
  GST_BUFFER_OFFSET (*buf) = self->offset++;
  GST_BUFFER_OFFSET_END (*buf) = self->offset;

  /* the time now is the time of the clock minus the base time */
  timestamp = timestamp - self->omx_delay;

  GST_DEBUG_OBJECT (self, "Adjusted timestamp %" GST_TIME_FORMAT,
      GST_TIME_ARGS (timestamp));

  GST_BUFFER_TIMESTAMP (*buf) = timestamp;

  return ret;

  /* ERROR */
error:
  {
    GST_ERROR_OBJECT (self, "error processing buffer %d (%s)", ret,
        gst_flow_get_name (ret));
    return ret;
  }
}
