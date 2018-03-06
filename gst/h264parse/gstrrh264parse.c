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

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <string.h>

#include "gstrrh264parse.h"

GST_DEBUG_CATEGORY_STATIC (gst_rr_h264_parse_debug);
#define GST_CAT_DEFAULT gst_rr_h264_parse_debug

enum
{
  PROP_0,
  PROP_SINGLE_NALU
};


static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format = (string) byte-stream,"
        "width = " GST_VIDEO_SIZE_RANGE ","
        "height = " GST_VIDEO_SIZE_RANGE ";")
    );

static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format = (string) avc,"
        "width = " GST_VIDEO_SIZE_RANGE ","
        "height = " GST_VIDEO_SIZE_RANGE ";")
    );


#define NAL_LENGTH 4
#define GST_RR_H264_PARSE_SINGLE_NALU_DEFAULT  FALSE

enum
{
  GST_H264_NAL_UNKNOWN = 0,
  GST_H264_NAL_SLICE = 1,
  GST_H264_NAL_SLICE_IDR = 5,
  GST_H264_NAL_SEI = 6,
  GST_H264_NAL_SPS = 7,
  GST_H264_NAL_PPS = 8,
};

typedef struct
{
  gint type;
  gint index;
  gint size;
} nalUnit;


/* class initialization */
#define gst_rr_h264_parse_parent_class parent_class
G_DEFINE_TYPE (GstRrH264Parse, gst_rr_h264_parse, GST_TYPE_BASE_TRANSFORM);

static void gst_rr_h264_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_rr_h264_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstCaps *gst_rr_h264_parse_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_rr_h264_parse_set_caps (GstBaseTransform * btrans,
    GstCaps * incaps, GstCaps * outcaps);
static GstFlowReturn gst_rr_h264_parse_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);
static gboolean gst_rr_h264_parse_stop (GstBaseTransform * btrans);

static void
gst_rr_h264_parse_class_init (GstRrH264ParseClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;
  GstBaseTransformClass *base_transform_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);
  base_transform_class = GST_BASE_TRANSFORM_CLASS (klass);

  gobject_class->set_property = gst_rr_h264_parse_set_property;
  gobject_class->get_property = gst_rr_h264_parse_get_property;

  g_object_class_install_property (gobject_class, PROP_SINGLE_NALU,
      g_param_spec_boolean ("single-nalu", "Single NAL Unit ",
          "Buffers have a single NAL unit of data",
          GST_RR_H264_PARSE_SINGLE_NALU_DEFAULT, G_PARAM_READWRITE));

  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_rr_h264_parse_transform_caps);
  base_transform_class->set_caps =
      GST_DEBUG_FUNCPTR (gst_rr_h264_parse_set_caps);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_rr_h264_parse_transform_ip);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (gst_rr_h264_parse_stop);

  gst_element_class_set_static_metadata (element_class,
      "H.264 parse element", "Codec/Parse/Converter/Video",
      "Tranform H.264 video from bytestream to packetized",
      "Melissa Montero Bonilla <melissa.montero@ridgerun.com>");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
}

static void
gst_rr_h264_parse_init (GstRrH264Parse * self)
{
  self->header_size = 0;
  self->set_codec_data = FALSE;
  self->single_nalu = GST_RR_H264_PARSE_SINGLE_NALU_DEFAULT;
  self->caps = NULL;
}

static void
gst_rr_h264_parse_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstRrH264Parse *self = GST_RR_H264_PARSE (object);

  switch (prop_id) {
    case PROP_SINGLE_NALU:
      self->single_nalu = g_value_get_boolean (value);
      break;
    default:
      break;
  }
}

static void
gst_rr_h264_parse_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstRrH264Parse *self = GST_RR_H264_PARSE (object);

  switch (prop_id) {
    case PROP_SINGLE_NALU:
      g_value_set_boolean (value, self->single_nalu);
      break;
    default:
      break;
  }
}


/* copies the given caps */
static GstCaps *
gst_rr_h264_parse_caps_change_stream_format (GstCaps * caps,
    GstPadDirection direction)
{
  GstStructure *st;
  gint i, n;
  GstCaps *res;

  res = gst_caps_new_empty ();

  n = gst_caps_get_size (caps);
  for (i = 0; i < n; i++) {
    st = gst_caps_get_structure (caps, i);

    /* If this is already expressed by the existing caps
     * skip this structure */
    if (i > 0 && gst_caps_is_subset_structure (res, st))
      continue;

    st = gst_structure_copy (st);
    if (direction == GST_PAD_SINK)
      gst_structure_set (st, "stream-format", G_TYPE_STRING, "avc", NULL);
    else
      gst_structure_set (st, "stream-format", G_TYPE_STRING, "byte-stream",
          NULL);
    gst_caps_append_structure (res, st);
  }

  return res;
}

/* The caps can be transformed into any other caps with format info removed.
 * However, we should prefer passthrough, so if passthrough is possible,
 * put it first in the list. */
static GstCaps *
gst_rr_h264_parse_transform_caps (GstBaseTransform * btrans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *tmp, *tmp2;
  GstCaps *result;
  gchar *str1 = NULL, *str2 = NULL;
  /* Get all possible caps that we can transform to */
  tmp = gst_rr_h264_parse_caps_change_stream_format (caps, direction);

  if (filter) {
    tmp2 = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
    tmp = tmp2;
  }

  result = tmp;

  GST_INFO_OBJECT (btrans, "transformed (%" GST_PTR_FORMAT ") %s into (%"
      GST_PTR_FORMAT ") %s", caps, str1 = gst_caps_to_string (caps),
      result, str2 = gst_caps_to_string (result));

  if (str1)
    g_free (str1);

  if (str2)
    g_free (str2);

  return result;
}

static gboolean
gst_rr_h264_parse_set_caps (GstBaseTransform * btrans, GstCaps * incaps,
    GstCaps * outcaps)
{
  GstRrH264Parse *self = GST_RR_H264_PARSE (btrans);
  gchar *str1 = NULL, *str2 = NULL;

  GST_INFO_OBJECT (btrans,
      "setting input caps: (%" GST_PTR_FORMAT ") %s output caps: (%"
      GST_PTR_FORMAT ") %s", incaps, str1 =
      gst_caps_to_string (incaps), outcaps, str2 =
      gst_caps_to_string (outcaps));

  if (str1)
    g_free (str1);
  if (str2)
    g_free (str2);

  if (self->caps) {
    if (!gst_caps_is_equal (self->caps, outcaps)) {
      /* Codec data should be updated */
      self->set_codec_data = FALSE;
    }
  }

  return TRUE;
}


static gboolean
gst_rr_h264_parse_fetch_header (guint8 * data, gint buffer_size,
    nalUnit * sps, nalUnit * pps, guint * header_size)
{
  gint i;
  gint nal_type;
  nalUnit *nalu = NULL;
  gint32 state;

  const gint32 start_code = 0x00000001;

  GST_LOG ("fetching header PPS and SPS");
  /*Initialize to a pattern that does not match the start code */
  state = ~(start_code);
  for (i = 0; i < (buffer_size - NAL_LENGTH); i++) {
    state = ((state << 8) | data[i]);

    /* In bytestream format each NAL is preceded by
     * a four byte start code: 0x00 0x00 0x00 0x01.
     * The byte after this code indicates the NAL type,
     * we're looking for the SPS(0x07) and PPS(0x08) NAL*/
    if (state == start_code) {
      if (nalu) {
        nalu->size = i - nalu->index - NAL_LENGTH + 1;
        nalu = NULL;
      }

      nal_type = (data[i + 1]) & 0x1f;
      if (nal_type == GST_H264_NAL_SPS) {
        nalu = sps;
      } else if (nal_type == GST_H264_NAL_PPS) {
        nalu = pps;
      } else if ((nal_type == GST_H264_NAL_SLICE)
          || (nal_type == GST_H264_NAL_SLICE_IDR)) {
        *header_size = i - NAL_LENGTH + 1;
        break;
      } else
        continue;

      nalu->type = nal_type;
      nalu->index = i + 1;

      i++;
    }
  }

  if (i >= (buffer_size - 5)) {
    nalu->size = buffer_size - nalu->index;
    *header_size = buffer_size;
  }

  GST_MEMDUMP ("Header", data, *header_size);

  return TRUE;
}

static gboolean
gst_rr_h264_parse_get_codec_data (GstRrH264Parse * self, GstBuffer * buf,
    GstBuffer ** codec_data)
{
  GstMapInfo info;
  nalUnit sps, pps;
  guint8 *header, *buffer, *sps_ptr;
  gint codec_data_size;
  gint num_sps = 1;
  gint num_pps = 1;
  gint nal_idx;

  GST_DEBUG_OBJECT (self, "generating codec data..");
  /*Get pointer to the header data */
  if (!gst_buffer_map (buf, &info, GST_MAP_READ))
    return FALSE;

  header = info.data;

  /*Parse the PPS and SPS */
  gst_rr_h264_parse_fetch_header (header, info.size, &sps, &pps,
      &self->header_size);

  if (sps.type != 7 || pps.type != 8 || sps.size < 4 || pps.size < 1) {
    GST_WARNING_OBJECT (self, "unexpected H.264 header");
    return FALSE;
  }

  GST_MEMDUMP ("SPS", &header[sps.index], sps.size);
  GST_MEMDUMP ("PPS", &header[pps.index], pps.size);

  /*
   *      -: avc codec data:-
   *  -----------------------------------
   *  1 byte  - version
   *  1 byte  - h.264 stream profile
   *  1 byte  - h.264 compatible profiles
   *  1 byte  - h.264 stream level
   *  6 bits  - reserved set to 63
   *  2 bits  - NAL length
   *            ( 0 - 1 byte; 1 - 2 bytes; 3 - 4 bytes)
   *  1 byte  - number of SPS
   *  2 bytes - SPS length
   *  for (i=0; i < number of SPS; i++) {
   *      SPS length bytes - SPS NAL unit
   *  }
   *  1 byte  - number of PPS
   *  2 bytes - PPS length
   *  for (i=0; i < number of PPS; i++) {
   *      PPS length bytes - PPS NAL unit
   *  }
   * ------------------------------------------
   */
  codec_data_size = sps.size + pps.size + 11;
  buffer = g_malloc (codec_data_size);
  /* SPS pointer, skip NAL unit type */
  sps_ptr = &header[sps.index] + 1;

  buffer[0] = 1;
  buffer[1] = sps_ptr[0];
  buffer[2] = sps_ptr[1];
  buffer[3] = sps_ptr[2];
  buffer[4] = 0xfc | (4 - 1);   /*0xfc for the 6 bits reserved */
  buffer[5] = 0xe0 | num_sps;   /*0xe0 for the 3 bits reserved */

  nal_idx = 6;
  GST_WRITE_UINT16_BE (buffer + nal_idx, sps.size);
  nal_idx += 2;
  memcpy (buffer + nal_idx, &header[sps.index], sps.size);
  nal_idx += sps.size;

  buffer[nal_idx++] = num_pps;  /* number of PPSs */
  GST_WRITE_UINT16_BE (buffer + nal_idx, pps.size);
  nal_idx += 2;
  memcpy (buffer + nal_idx, &header[pps.index], pps.size);
  nal_idx += pps.size;

  GST_MEMDUMP ("Codec data", buffer, codec_data_size);
  gst_buffer_unmap (buf, &info);

  *codec_data = gst_buffer_new_wrapped (buffer, codec_data_size);

  return TRUE;
}

/* Function to convert the content of the buffer from bytestream to packetized convertion */
static gboolean
gst_rr_h264_parse_set_codec_data (GstRrH264Parse * self, GstBuffer * buf)
{

  GstBuffer *codec_data;
  GstCaps *src_caps, *caps;
  gchar *str = NULL;

  /* Generate the codec data with the SPS and the PPS */
  if (!gst_rr_h264_parse_get_codec_data (self, buf, &codec_data)) {
    GST_WARNING_OBJECT (self, "Failed to get codec data");
    return FALSE;
  }

  /* Update the caps with the codec data */
  src_caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM_SRC_PAD (self));
  caps = gst_caps_copy (src_caps);
  gst_caps_unref (src_caps);

  gst_caps_set_simple (caps, "codec_data", GST_TYPE_BUFFER, codec_data,
      (char *) NULL);
  gst_buffer_unref (codec_data);

  if (!gst_pad_set_caps (GST_BASE_TRANSFORM_SRC_PAD (self), caps))
    goto failed_codec_data;

  if (self->caps)
    gst_caps_unref (self->caps);
  self->caps = caps;

  GST_INFO_OBJECT (self,
      "updated caps with codec data (%" GST_PTR_FORMAT ") %s ", caps, str =
      gst_caps_to_string (caps));
  if (str)
    g_free (str);

  return TRUE;

failed_codec_data:
  GST_WARNING_OBJECT (self, "Src caps can't be updated");
  gst_caps_unref (caps);
  return FALSE;
}

/* Function that change the content of the buffer to packetizer */
static gboolean
gst_rr_h264_parse_to_packetized (GstRrH264Parse * self, GstBuffer * buffer)
{

  GstMapInfo info;
  guint8 *data;
  gint i, mark = 0;
  gint curr_nal_type = -1;
  gint prev_nal_type = -1;
  gint size;
  gint32 state;
  const gint32 start_code = 0x00000001;

  GST_DEBUG_OBJECT (self, "parsing byte-stream to avc");

  if (!gst_buffer_map (buffer, &info, GST_MAP_WRITE)) {
    GST_ERROR_OBJECT (self, "failed to map buffer");
    return FALSE;
  }

  data = info.data;
  size = info.size;
  /*Initialize to a pattern that does not match the start code */
  state = ~(start_code);
  for (i = 0; i < size - NAL_LENGTH; i++) {
    state = ((state << 8) | data[i]);
    if (state == start_code) {
      prev_nal_type = curr_nal_type;
      curr_nal_type = (data[i + 1]) & 0x1f;
      GST_DEBUG_OBJECT (self, "NAL unit %d", curr_nal_type);
      if (self->single_nalu) {
        if ((curr_nal_type == GST_H264_NAL_SPS)
            || (curr_nal_type == GST_H264_NAL_PPS)) {
          GST_DEBUG_OBJECT (self, "single NALU, found a I-frame");
          GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
          /* Caution: here we are asumming the output buffer only 
           * has one memory block*/
          info.memory->offset = self->header_size;
          gst_buffer_set_size (buffer, size - self->header_size);
          mark = i + self->header_size + 1;
        } else {
          GST_DEBUG_OBJECT (self, "single NALU, found a P-frame");
          GST_BUFFER_FLAG_SET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
          mark = i + 1;
        }
        i = size - NAL_LENGTH;
        break;
      } else {
        if ((prev_nal_type == GST_H264_NAL_SPS
                || prev_nal_type == GST_H264_NAL_PPS)) {
          /* Discard anything previous to the SPS and PPS */
          /* Caution: here we are asumming the output buffer  
           * has only one memory block*/
          info.memory->offset = i - NAL_LENGTH + 1;
          gst_buffer_set_size (buffer, size - (i - NAL_LENGTH + 1));
          GST_DEBUG_OBJECT (self, "SPS and PPS discard");
          GST_BUFFER_FLAG_UNSET (buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        } else if (prev_nal_type != -1) {
          /* Replace the NAL start code with the length */
          gint length = i - mark - NAL_LENGTH + 1;
          gint k;
          for (k = 1; k <= 4; k++) {
            data[mark - k] = length & 0xff;
            length >>= 8;
          }
        }
      }
      /* Mark where next NALU starts */
      mark = i + 1;
    }
  }

  if (i == (size - 4)) {
    /* We reach the end of the buffer */
    if (curr_nal_type != -1) {
      gint k;
      gint length = size - mark;
      GST_DEBUG_OBJECT (self, "Replace the NAL start code "
          "with the length %d buffer %d", length, size);
      for (k = 1; k <= 4; k++) {
        data[mark - k] = length & 0xff;
        length >>= 8;
      }
    }
  }

  gst_buffer_unmap (buffer, &info);

  return TRUE;
}

static GstFlowReturn
gst_rr_h264_parse_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRrH264Parse *self = GST_RR_H264_PARSE (trans);

  /* Obtain and set codec data */
  if (!self->set_codec_data) {
    if (gst_rr_h264_parse_set_codec_data (self, buf)) {
      self->set_codec_data = TRUE;
    }
  }

  /* Change the buffer content to packetized */
  if (!gst_rr_h264_parse_to_packetized (self, buf))
    return GST_FLOW_ERROR;

  return GST_FLOW_OK;
}

static gboolean
gst_rr_h264_parse_stop (GstBaseTransform * btrans)
{
  GstRrH264Parse *self = GST_RR_H264_PARSE (btrans);

  if (self->caps)
    gst_caps_unref (self->caps);

  return TRUE;
}

static gboolean
rr_h264_parse_init (GstPlugin * self)
{
  /* debug category for filtering log messages
   */
  GST_DEBUG_CATEGORY_INIT (gst_rr_h264_parse_debug, "rrh264parse",
      0, "RR H.264 parse");

  return gst_element_register (self, "rrh264parse", GST_RANK_NONE,
      GST_TYPE_RR_H264_PARSE);
}


GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    rrh264parse,
    "Transform H.264 stream from bytestream to packetized",
    rr_h264_parse_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
