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

#ifndef __GST_RR_H264_PARSE_H__
#define __GST_RR_H264_PARSE_H__

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS
#define GST_TYPE_RR_H264_PARSE \
  (gst_rr_h264_parse_get_type())
#define GST_RR_H264_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_RR_H264_PARSE,GstRrH264Parse))
#define GST_RR_H264_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_RR_H264_PARSE,GstRrH264ParseClass))
#define GST_IS_RR_H264_PARSE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_RR_H264_PARSE))
#define GST_IS_RR_H264_PARSE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_RR_H264_PARSE))
typedef struct _GstRrH264Parse GstRrH264Parse;
typedef struct _GstRrH264ParseClass GstRrH264ParseClass;

struct _GstRrH264Parse
{
  GstBaseTransform element;
  
  GstCaps *caps;

  guint header_size;
  gboolean single_nalu;
  gboolean set_codec_data;
};

struct _GstRrH264ParseClass
{
  GstBaseTransformClass parent_class;
};

GType gst_rr_h264_parse_get_type (void);

G_END_DECLS
#endif /* __GST_RR_H264_PARSE_H__ */
