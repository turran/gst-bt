/* Gst-Bt - BitTorrent related GStreamer elements
 * Copyright (C) 2015 Jorge Luis Zapata
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GST_BT_DEMUX_H
#define GST_BT_DEMUX_H

#include <gst/gst.h>
#include <gst/base/gstadapter.h>

G_BEGIN_DECLS

#define GST_TYPE_BT_DEMUX            (gst_bt_demux_get_type())
#define GST_BT_DEMUX(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),\
                                         GST_TYPE_BT_DEMUX, GstBtDemux))
#define GST_BT_DEMUX_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),\
                                         GST_TYPE_BT_DEMUX, GstBtDemuxClass))
#define GST_BT_DEMUX_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),\
                                         GST_TYPE_BT_DEMUX, GstBtDemuxClass))
#define GST_IS_BT_DEMUX(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),\
                                         GST_TYPE_BT_DEMUX))
#define GST_IS_BT_DEMUX_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),\
                                         GST_TYPE_BT_DEMUX))

#define GST_TYPE_BT_DEMUX_STREAM            (gst_bt_demux_stream_get_type())
#define GST_BT_DEMUX_STREAM(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),\
                                                GST_TYPE_BT_DEMUX_STREAM, GstBtDemuxStream))
#define GST_BT_DEMUX_STREAM_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),\
                                                GST_TYPE_BT_DEMUX_STREAM, GstBtDemuxStreamClass))
#define GST_BT_DEMUX_STREAM_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),\
                                                GST_TYPE_BT_DEMUX_STREAM, GstBtDemuxStreamClass))
#define GST_IS_BT_DEMUX_STREAM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),\
                                               GST_TYPE_BT_DEMUX_STREAM))
#define GST_IS_BT_DEMUX_STREAM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),\
                                               GST_TYPE_BT_DEMUX_STREAM))

/* TODO add the FIRST policy */
typedef enum _GstBtDemuxSelectorPolicy {
  GST_BT_DEMUX_SELECTOR_POLICY_ALL,
  GST_BT_DEMUX_SELECTOR_POLICY_LARGER,
} GstBtDemuxSelectorPolicy;

typedef struct _GstBtDemuxStream
{
  GstPad pad;
  gint idx;

  gint current_piece;
  gint start_offset;
  gint start_piece;
  gint end_offset;
  gint end_piece;

  gint64 start_byte;
  gint64 end_byte;
  gboolean pending_segment;

  gboolean requested;
  gboolean finished;
  gboolean buffering;
  gint buffering_level;
  gint buffering_count;

  GMutex *lock;
} GstBtDemuxStream;

typedef struct _GstBtDemuxStreamClass {
  GstPadClass parent_class;
} GstBtDemuxStreamClass;

GType gst_bt_demux_stream_get_type (void);

typedef struct _GstBtDemux
{
  GstElement parent;
  GstAdapter *adapter;

  GstBtDemuxSelectorPolicy policy;
  GMutex *streams_lock;
  GSList *streams;
  gchar *requested_streams;

  gboolean finished;
  gboolean buffering;
  gint buffer_pieces;

  gpointer session;

  GstTask *task;
  GStaticRecMutex task_lock;

  GstTask *push_task;
  GStaticRecMutex push_task_lock;

  GAsyncQueue *ipc;
} GstBtDemux;

typedef struct _GstBtDemuxClass
{
  GstElementClass parent_class;
  /* inform that the streams metadata have changed */
  void (*streams_changed) (GstBtDemux * demux);
  /* get stream tags for a stream */
  GstTagList *(*get_stream_tags) (GstBtDemux * demux, gint stream);
} GstBtDemuxClass;

GType gst_bt_demux_get_type (void);

G_END_DECLS

#endif
