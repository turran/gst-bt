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

/*
 * TODO:
 * + Implement queries:
 *   buffering level
 *   position
 * + Implenent the element to work in pull mode
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt.h"
#include "gst_bt_demux.hpp"
#include <gst/base/gsttypefindhelper.h>

#include <glib/gstdio.h>

#include <iterator>
#include <deque>

#include "libtorrent/session.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/time.hpp"

#define GST_BT_DEMUX_STREAMS_LOCK(o) (g_mutex_lock(&o->streams_lock))
#define GST_BT_DEMUX_STREAMS_UNLOCK(o) (g_mutex_unlock(&o->streams_lock))

#define GST_BT_DEMUX_STREAM_MUTEX_INIT(o) (g_rec_mutex_init(&o->lock))
#define GST_BT_DEMUX_STREAM_MUTEX_CLEAR(o) (g_rec_mutex_clear(&o->lock))
#define GST_BT_DEMUX_STREAM_LOCK(o) (g_rec_mutex_lock(&o->lock))
#define GST_BT_DEMUX_STREAM_UNLOCK(o) (g_rec_mutex_unlock(&o->lock))

#define DEFAULT_TYPEFIND TRUE
#define DEFAULT_BUFFER_PIECES 3
#define DEFAULT_DIR "btdemux"
#define DEFAULT_TEMP_REMOVE TRUE

GST_DEBUG_CATEGORY_EXTERN (gst_bt_demux_debug);
#define GST_CAT_DEFAULT gst_bt_demux_debug

/* Forward declarations */
static void
gst_bt_demux_send_buffering (GstBtDemux * btdemux, libtorrent::torrent_handle h);
static void
gst_bt_demux_check_no_more_pads (GstBtDemux * btdemux);

typedef struct _GstBtDemuxBufferData
{
  boost::shared_array <char> buffer;
  int piece;
  int size;
} GstBtDemuxBufferData;

/*----------------------------------------------------------------------------*
 *                            The buffer helper                               *
 *----------------------------------------------------------------------------*/
static void gst_bt_demux_buffer_data_free (gpointer data)
{
  g_free (data);
}

GstBuffer * gst_bt_demux_buffer_new (boost::shared_array <char> buffer,
    gint piece, gint size, GstBtDemuxStream * s)
{
  GstBuffer *buf;
  GstBtDemuxBufferData *buf_data;
  guint8 *data;

  buf_data = g_new0 (GstBtDemuxBufferData, 1);
  buf_data->buffer = buffer;

  data = (guint8 *)buffer.get ();
  /* handle the offsets */
  if (piece == s->start_piece) {
    data += s->start_offset;
    size -= s->start_offset;
  }

  if (piece == s->end_piece) {
    size -= size - s->end_offset;
  }

  /* create the buffer */
  buf = gst_buffer_new_wrapped_full ((GstMemoryFlags)0, data, size, 0, size,
      buf_data, gst_bt_demux_buffer_data_free);

  return buf;
}

/*----------------------------------------------------------------------------*
 *                           The selector policy                              *
 *----------------------------------------------------------------------------*/
static GType
gst_bt_demux_selector_policy_get_type (void)
{
  static GType gst_bt_demux_selector_policy_type = 0;
  static const GEnumValue selector_policy_types[] = {
    {GST_BT_DEMUX_SELECTOR_POLICY_ALL, "All streams", "all" },
    {GST_BT_DEMUX_SELECTOR_POLICY_LARGER, "Larger stream", "larger" },
    {0, NULL, NULL}
  };

  if (!gst_bt_demux_selector_policy_type) {
    gst_bt_demux_selector_policy_type =
        g_enum_register_static ("GstBtDemuxSelectorPolicy",
        selector_policy_types);
  }
  return gst_bt_demux_selector_policy_type;
}

/*----------------------------------------------------------------------------*
 *                             The stream class                               *
 *----------------------------------------------------------------------------*/

G_DEFINE_TYPE (GstBtDemuxStream, gst_bt_demux_stream, GST_TYPE_PAD);

static gboolean
gst_bt_demux_stream_start_buffering (GstBtDemuxStream * btstream,
    libtorrent::torrent_handle h, int max_pieces)
{
  using namespace libtorrent;
  int i;
  int start = btstream->current_piece + 1;
  int end = btstream->current_piece + max_pieces;

  /* do not overflow */
  if (end > btstream->end_piece)
    end = btstream->end_piece;

  /* count how many consecutive pieces need to be downloaded */
  btstream->buffering_count = 0;
  for (i = start; i <= end; i++) {

    /* already downloaded */
    if (h.have_piece (i))
      continue;

    btstream->buffering_count++;
  }

  if (btstream->buffering_count) {
    btstream->buffering = TRUE;
    btstream->buffering_level = 0;
    return TRUE;
  }
  
  return FALSE;
}

static void
gst_bt_demux_stream_push_loop (gpointer user_data)
{
  using namespace libtorrent;
  GstBtDemux *btdemux;
  GstBtDemuxStream *btstream;
  GstBtDemuxBufferData *ipc_data;
  GstBuffer *buf;
  GstFlowReturn ret;
  GstBtDemuxBufferData *buf_data;
  GSList *walk;
  guint8 *data;
  session *s;
  torrent_handle h;
  gboolean update_buffering;
  gboolean send_eos;
  
  update_buffering = FALSE;
  send_eos = FALSE;

  btstream = GST_BT_DEMUX_STREAM (user_data);
  btdemux = GST_BT_DEMUX (gst_pad_get_parent (GST_PAD (btstream)));

  // GST_BT_DEMUX_STREAM_LOCK (btdemux);

  if (btdemux->finished) {
    gst_pad_pause_task (GST_PAD (btstream));
    return;
  }

  ipc_data = (GstBtDemuxBufferData *)g_async_queue_pop (btstream->ipc);
  if (!ipc_data) {
    gst_pad_pause_task (GST_PAD (btstream));
    return;
  }

  if (!ipc_data->size) {
    gst_bt_demux_buffer_data_free (ipc_data);
    return;
  }

  s = (session *)btdemux->session;
  h = s->get_torrents ()[0];

  GST_BT_DEMUX_STREAM_LOCK(btstream);

  if (ipc_data->piece < btstream->start_piece && 
      ipc_data->piece > btstream->end_piece) {
    gst_bt_demux_buffer_data_free (ipc_data);

    GST_BT_DEMUX_STREAM_UNLOCK (btstream);
    return;
  }

  if (!btstream->requested) {
    gst_bt_demux_buffer_data_free (ipc_data);
    GST_BT_DEMUX_STREAM_UNLOCK (btstream);
    return;
  }

  /* in case we are not expecting this buffer */
  if (ipc_data->piece != btstream->current_piece + 1) {
    GST_DEBUG_OBJECT (btstream, "Dropping piece %d, waiting for %d on "
        "file %d", ipc_data->piece, btstream->current_piece + 1, btstream->idx);
    gst_bt_demux_buffer_data_free (ipc_data);
    GST_BT_DEMUX_STREAM_UNLOCK(btstream);
    return;
  }

  buf = gst_bt_demux_buffer_new (ipc_data->buffer, ipc_data->piece,
    ipc_data->size, btstream);

  GST_DEBUG_OBJECT (btstream, "Received piece %d of size %d on file %d",
      ipc_data->piece, ipc_data->size, btstream->idx);

  /* read the next piece */
  if (ipc_data->piece + 1 <= btstream->end_piece) {
    if (h.have_piece (ipc_data->piece + 1)) {
      GST_DEBUG_OBJECT (btstream, "Reading next piece %d, current: %d",
          ipc_data->piece + 1, btstream->current_piece);
      h.read_piece (ipc_data->piece + 1);
    } else {
      int i;

      GST_DEBUG_OBJECT (btstream, "Start buffering next piece %d",
          ipc_data->piece + 1);
      /* start buffering now that the piece is not available */
      gst_bt_demux_stream_start_buffering (btstream, h,
          btdemux->buffer_pieces);
      update_buffering = TRUE;
    }
  }

  if (btstream->pending_segment) {
    GstEvent *event;
    GstSegment *segment;
    gboolean update;

    segment = gst_segment_new ();
    gst_segment_init (segment, GST_FORMAT_BYTES);
    gst_segment_do_seek (segment, 1.0, GST_FORMAT_BYTES,
        GST_SEEK_FLAG_NONE, GST_SEEK_TYPE_SET, btstream->start_byte,
        GST_SEEK_TYPE_SET, btstream->end_byte, &update);
    event = gst_event_new_segment (segment);
    gst_pad_push_event (GST_PAD (btstream), event);
    btstream->pending_segment = FALSE;
  }

  GST_DEBUG_OBJECT (btstream, "Pushing buffer, size: %d, file: %d, piece: %d",
      ipc_data->size, btstream->idx, ipc_data->piece);

  /* keep track of the current piece */
  btstream->current_piece = ipc_data->piece;

  ret = gst_pad_push (GST_PAD (btstream), buf);
  if (ret != GST_FLOW_OK) {
    send_eos = TRUE;
    if (ret == GST_FLOW_NOT_LINKED || ret <= GST_FLOW_UNEXPECTED) {
      GST_ELEMENT_ERROR (btdemux, STREAM, FAILED,
          ("Internal data flow error."),
          ("streaming task paused, reason %s (%d)", gst_flow_get_name (ret),
          ret));
    }
  }

  // FIXME: ?
#if 0
  /* send the end of segment in case we need to */
  if (ipc_data->piece == thiz->end_piece) {

  }
#endif

  /* send the EOS downstream, check that last push didnt trigger a new seek */
  if (ipc_data->piece == btstream->last_piece && !btstream->pending_segment)
    send_eos = TRUE;

  if (send_eos) {
    GstEvent *eos;

    eos = gst_event_new_eos ();
    GST_DEBUG_OBJECT (btstream, "Sending EOS on file %d", btstream->idx);
    gst_pad_push_event (GST_PAD (btstream), eos);
    gst_pad_pause_task (GST_PAD (btstream));
    btstream->finished = TRUE;
  }
  gst_bt_demux_buffer_data_free (ipc_data);

  GST_BT_DEMUX_STREAM_UNLOCK (btstream);

  /* send information about the whole element */
  // GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
  if (update_buffering)
    gst_bt_demux_send_buffering (btdemux, h);

  // FIXME: why is this unlock here?
  // GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
}


static void
gst_bt_demux_stream_update_buffering_unlocked (GstBtDemuxStream * btstream,
    libtorrent::torrent_handle h, int max_pieces)
{
  using namespace libtorrent;

  int i;
  int start = btstream->current_piece + 1;
  int end = btstream->current_piece + max_pieces;
  int buffered_pieces = 0;

  /* do not overflow */
  if (end > btstream->end_piece)
    end = btstream->end_piece;

  /* count how many consecutive pieces have been downloaded */
  for (i = start; i <= end; i++) {
    if (h.have_piece (i))
      buffered_pieces++;
  }

  if (buffered_pieces > btstream->buffering_count)
    buffered_pieces = btstream->buffering_count;

  btstream->buffering = TRUE;
  btstream->buffering_level = (buffered_pieces * 100) / btstream->buffering_count;
  GST_DEBUG_OBJECT (btstream, "Buffering level %d (%d/%d)", btstream->buffering_level,
      buffered_pieces, btstream->buffering_count);
}

static void
gst_bt_demux_stream_add_piece (GstBtDemuxStream * btstream,
    libtorrent::torrent_handle h, int piece, int max_pieces)
{
  using namespace libtorrent;

  GST_DEBUG_OBJECT (btstream, "Adding more pieces at %d, current: %d, "
      "max: %d", piece, btstream->current_piece, max_pieces);
  for (piece; piece <= btstream->end_piece; piece++) {
    int priority;

    if (h.have_piece (piece))
      continue;

    /* if already scheduled, do nothing */
    priority = h.piece_priority (piece);
    if (priority == 7)
      continue;

    /* max priority */
    priority = 7;

    h.piece_priority (piece, priority);
    GST_DEBUG_OBJECT (btstream, "Requesting piece %d, prio: %d, current: %d, ",
        piece, priority, btstream->current_piece);
    break;
  }
}

static gboolean
gst_bt_demux_stream_activate_unlocked (GstBtDemuxStream * btstream,
    libtorrent::torrent_handle h, int max_pieces)
{
  gboolean ret = FALSE;

  btstream->requested = TRUE;
  btstream->current_piece = btstream->start_piece - 1;
  btstream->pending_segment = TRUE;

  GST_DEBUG_OBJECT (btstream, "Activating stream '%s', start: %d, "
      "start_offset: %d, end: %d, end_offset: %d, current: %d",
      GST_PAD_NAME (btstream), btstream->start_piece, btstream->start_offset,
      btstream->end_piece, btstream->end_offset, btstream->current_piece);

  if (h.have_piece (btstream->start_piece)) {
    /* request the first non-downloaded piece */
    if (btstream->start_piece != btstream->end_piece) {
      int i;

      for (i = 1; i < max_pieces; i++) {
        gst_bt_demux_stream_add_piece (btstream, h, btstream->start_piece + i,
            max_pieces);
      }
    }

  } else {
    int i;

    for (i = 0; i < max_pieces; i++) {
      gst_bt_demux_stream_add_piece (btstream, h, btstream->start_piece + i,
          max_pieces);
    }
    /* start the buffering */
    gst_bt_demux_stream_start_buffering (btstream, h, max_pieces);
    ret = TRUE;
  }

  return ret;
}

static void
gst_bt_demux_stream_info (GstBtDemuxStream * btstream,
    libtorrent::torrent_handle h, gint * start_offset,
    gint * start_piece, gint * end_offset, gint * end_piece,
    gint64 * size)
{
  using namespace libtorrent;
  // file_slice fe;
  int piece_length;
  auto ti = h.torrent_file();
  auto & files = ti->files();

  gint file_offset;
  gint file_size;
  
  file_offset = (gint) files.file_offset(btstream->idx);
  file_size = (gint)files.file_size(btstream->idx);
  piece_length = ti->piece_length ();

  // fe = ti->file_at (btstream->idx);

  // TODO: verify !

  if (start_piece)
    *start_piece = file_offset / piece_length;
  if (start_offset)
    *start_offset = file_offset % piece_length;
  if (end_piece)
    *end_piece = (file_offset + file_size) / piece_length;
  if (end_offset)
    *end_offset = (file_offset + file_size) % piece_length;
  if (size)
    *size = file_size;
}

static gboolean
gst_bt_demux_stream_seek (GstBtDemuxStream * btstream, GstEvent * event)
{
  using namespace libtorrent;
  GstBtDemux *btdemux;
  GstFormat format;
  GstSeekFlags flags;
  GstSeekType start_type, stop_type;
  gint64 start, stop;
  gdouble rate;
  gint start_piece, start_offset, end_piece, end_offset;
  torrent_handle h;
  session *s;
  int piece_length;
  int tmp;
  gboolean update_buffering;
  gboolean ret = FALSE;

  btdemux = GST_BT_DEMUX (gst_pad_get_parent (GST_PAD (btstream)));
  s = (session *)btdemux->session;
  h = s->get_torrents ()[0];
  gst_object_unref (btdemux);

  /* get the piece length */
  auto ti = h.torrent_file();
  piece_length = ti->piece_length ();

  gst_event_parse_seek (event, &rate, &format, &flags, &start_type,
      &start, &stop_type, &stop);

  /* sanitize stuff */
  if (format != GST_FORMAT_BYTES)
    goto beach;

  if (rate < 0.0)
    goto beach;

  gst_bt_demux_stream_info (btstream, h, &start_offset,
      &start_piece, &end_offset, &end_piece, NULL);

  if (start < 0)
    start = 0;

  if (stop < 0) {
    int num_pieces;

    num_pieces = end_piece - start_piece + 1;
    if (num_pieces == 1) {
      /* all the bytes on a single piece */
      stop = end_offset - start_offset;
    } else {
      /* count the full pieces */
      stop = (num_pieces - 2) * piece_length;
      /* add the start bytes */
      stop += piece_length - start_offset;
      /* add the end bytes */
      stop += end_offset;
    }
  }

  if (flags & GST_SEEK_FLAG_FLUSH) {
    GstEvent *flush_stop;

    gst_pad_push_event (GST_PAD (btstream), gst_event_new_flush_start ());
    flush_stop = gst_event_new_flush_stop (TRUE);
    gst_pad_push_event (GST_PAD (btstream), flush_stop);
  } else {
    /* TODO we need to close the segment */
  }

  if (flags & GST_SEEK_FLAG_SEGMENT) {
    GST_ERROR ("Segment seek");
  }

  GST_BT_DEMUX_STREAM_LOCK (btstream);

  /* update the stream segment */
  btstream->start_byte = start;
  btstream->end_byte = stop,

  btstream->end_piece = start_piece + ((stop + start_offset) / piece_length);
  btstream->end_offset = start_piece + ((stop + start_offset) % piece_length); 

  tmp = start_piece;
  btstream->start_piece = start_piece + ((start + start_offset) / piece_length);
  btstream->start_offset = tmp + ((start + start_offset) % piece_length);

  GST_DEBUG_OBJECT (btstream, "Seeking to, start: %d, start_offset: %d, end: %d, "
      "end_offset: %d", btstream->start_piece, btstream->start_offset,
      btstream->end_piece, btstream->end_offset);

  /* activate again this stream */
  update_buffering = gst_bt_demux_stream_activate_unlocked (btstream, h,
      btdemux->buffer_pieces);

  if (!update_buffering) {
    /* FIXME what if the demuxer is already buffering ? */
    /* start directly */
    GST_DEBUG_OBJECT (btstream, "Starting stream '%s', reading piece %d, "
        "current: %d", GST_PAD_NAME (btstream), btstream->start_piece,
        btstream->current_piece);
    h.read_piece (btstream->start_piece);
  }

  ret = TRUE;

  /* send the buffering if we need to */
  if (update_buffering)
    gst_bt_demux_send_buffering (btdemux, h);

  GST_BT_DEMUX_STREAM_UNLOCK(btstream);

beach:
  return ret;
}

static gboolean
gst_bt_demux_stream_event (GstPad * pad, GstObject * object, GstEvent * event)
{
  GstBtDemuxStream *btstream;
  gboolean ret = FALSE;

  btstream = GST_BT_DEMUX_STREAM (pad);

  GST_DEBUG_OBJECT (btstream, "Event %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      {
        ret = gst_bt_demux_stream_seek (btstream, event);
        break;
      }
  }

  return ret;
}

static gboolean
gst_bt_demux_stream_query (GstPad * pad, GstObject * object, GstQuery * query)
{
  using namespace libtorrent;
  GstBtDemux *btdemux;
  GstBtDemuxStream *btstream;
  gboolean ret = FALSE;

  btstream = GST_BT_DEMUX_STREAM (pad);
  btdemux = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (btstream, "Quering %s", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_SEEKING:
      {
        GstFormat format;
        gst_query_parse_seeking (query, &format, NULL, NULL, NULL);
        if (format == GST_FORMAT_BYTES) {
          gst_query_set_seeking (query, GST_FORMAT_BYTES, TRUE, 0, -1);
          ret = TRUE;
        }
      }
      break;

    case GST_QUERY_DURATION:
      {
        session *s;
        torrent_handle h;
        GstFormat fmt;
        gint64 bytes;

        s = (session *)btdemux->session;
        h = s->get_torrents ()[0];

        gst_query_parse_duration (query, &fmt, NULL);
        if (fmt == GST_FORMAT_BYTES) {
          gst_bt_demux_stream_info (btstream, h, NULL, NULL, NULL, NULL, &bytes);
          gst_query_set_duration (query, GST_FORMAT_BYTES, bytes);
          ret = TRUE;
        }
      }
      break;

    /* TODO add suport for buffering ranges
     * The API get_download_queue() only returns the current
     * downloading pieces, not the status of every piece, so we need
     * to track every piece status
     */
    case GST_QUERY_BUFFERING:
    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_stream_dispose (GObject * object)
{
  GstBtDemuxStream *btstream;

  btstream = GST_BT_DEMUX_STREAM (object);

  GST_DEBUG_OBJECT (btstream, "Disposing");

  if (btstream->path)
    g_free (btstream->path);

  if (btstream->ipc)
    g_async_queue_unref (btstream->ipc);

  btstream->ipc = NULL;

  GST_BT_DEMUX_STREAM_MUTEX_CLEAR(btstream);

  G_OBJECT_CLASS (gst_bt_demux_stream_parent_class)->dispose (object);
}

static void
gst_bt_demux_stream_class_init (GstBtDemuxStreamClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gst_bt_demux_stream_parent_class = g_type_class_peek_parent (klass);

  /* initialize the object class */
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_bt_demux_stream_dispose);
}

static void
gst_bt_demux_stream_init (GstBtDemuxStream * btstream)
{
  GST_BT_DEMUX_STREAM_MUTEX_INIT(btstream);

  gst_pad_set_event_function (GST_PAD (btstream),
      GST_DEBUG_FUNCPTR (gst_bt_demux_stream_event));
  gst_pad_set_query_function (GST_PAD (btstream),
      GST_DEBUG_FUNCPTR (gst_bt_demux_stream_query));

  /* our ipc */
  btstream->ipc = g_async_queue_new_full (
      (GDestroyNotify)gst_bt_demux_buffer_data_free);
}

/*----------------------------------------------------------------------------*
 *                            The demuxer class                               *
 *----------------------------------------------------------------------------*/
enum {
  PROP_0,
  PROP_SELECTOR_POLICY,
  PROP_TYPEFIND,
  PROP_N_STREAMS,
  PROP_CURRENT_STREAM,
  PROP_TEMP_LOCATION,
  PROP_TEMP_REMOVE,
};

enum
{
  SIGNAL_GET_STREAM_TAGS,
  SIGNAL_STREAMS_CHANGED,
  LAST_SIGNAL
};

static guint gst_bt_demux_signals[LAST_SIGNAL] = { 0 };
 
G_DEFINE_TYPE (GstBtDemux, gst_bt_demux, GST_TYPE_ELEMENT);

static GstStaticPadTemplate sink_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-bittorrent"));

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE ("src_%02d",
    GST_PAD_SRC,
    GST_PAD_SOMETIMES,
    GST_STATIC_CAPS_ANY);

/* Accumulate every buffer for processing later */
static GstFlowReturn
gst_bt_demux_sink_chain (GstPad * pad, GstObject * object,
    GstBuffer * buffer)
{
  GstBtDemux *btdemux;

  btdemux = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (btdemux, "Received buffer");
  gst_adapter_push (btdemux->adapter, gst_buffer_ref (buffer));

  return GST_FLOW_OK;
}

/* On EOS, start processing the file */
static gboolean
gst_bt_demux_sink_event (GstPad * pad, GstObject * object,
    GstEvent * event)
{
  GstBtDemux *btdemux;
  // GstPad *peer;
  GstBuffer *buf;
  GstMessage *message;
  gint len;
  gboolean res;
  libtorrent::torrent_info *torrent_info;
  guint8 *data;
  GstMapInfo mi;

  res = TRUE;

  if (GST_EVENT_TYPE (event) != GST_EVENT_EOS)
    goto beach;

  btdemux = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (btdemux, "Received EOS");
  len = gst_adapter_available (btdemux->adapter);
  buf = gst_adapter_take_buffer (btdemux->adapter, len);

  gst_buffer_map (buf, &mi, GST_MAP_READ);
  data = mi.data;

  /* Time to process */
  torrent_info = new libtorrent::torrent_info (
    (const char *)data, len);
  gst_buffer_unmap (buf, &mi);

  gst_buffer_unref (buf);

  if (torrent_info) {
    libtorrent::session *session;
    libtorrent::add_torrent_params tp;

    tp.ti = boost::shared_ptr<libtorrent::torrent_info>(torrent_info);
    tp.save_path = btdemux->temp_location;

    session = (libtorrent::session *)btdemux->session;
    session->async_add_torrent (tp);
  } else {
    /* TODO Send an error message */
  }

beach:
  return res;
}


/* Code taken from playbin2 to marshal the action prototype */
#define g_marshal_value_peek_int(v)      g_value_get_int (v)
void
gst_bt_demux_cclosure_marshal_BOXED__INT (GClosure     * closure,
                             GValue       * return_value G_GNUC_UNUSED,
                             guint         n_param_values,
                             const GValue * param_values,
                             gpointer      invocation_hint G_GNUC_UNUSED,
                             gpointer      marshal_data)
{
  typedef gpointer (*GMarshalFunc_BOXED__INT) (gpointer     data1,
                                               gint         arg_1,
                                               gpointer     data2);
  register GMarshalFunc_BOXED__INT callback;
  register GCClosure * cc = (GCClosure *) closure;
  register gpointer data1, data2;
  gpointer v_return;

  g_return_if_fail (return_value != NULL);
  g_return_if_fail (n_param_values == 2);

  if (G_CCLOSURE_SWAP_DATA (closure)) {
    data1 = closure->data;
    data2 = g_value_peek_pointer (param_values + 0);
  }
  else {
    data1 = g_value_peek_pointer (param_values + 0);
    data2 = closure->data;
  }
  callback =
      (GMarshalFunc_BOXED__INT) (marshal_data ? marshal_data : cc->callback);

  v_return =
      callback (data1, g_marshal_value_peek_int (param_values + 1), data2);

  g_value_take_boxed (return_value, v_return);
}


static GstTagList *
gst_bt_demux_get_stream_tags (GstBtDemux * btdemux, gint stream)
{
  using namespace libtorrent;
  session *s;
  // file_entry fe;
  std::vector<torrent_handle> torrents;
  int i;

  if (!btdemux->streams)
    return NULL;

  /* get the torrent_info */
  s = (session *)btdemux->session;
  torrents = s->get_torrents ();

  auto ti = torrents[0].torrent_file();
  if (!ti)
      return NULL;

  for (i = 0; i < ti->num_files (); i++) {
    // file_entry fe = ti->file_at (i);

    /* TODO get the stream tags (i.e metadata) */
    /* set the file name */
    /* set the file size */
  }
  
  return NULL;
}

static GSList *
gst_bt_demux_get_policy_streams (GstBtDemux * btdemux)
{
  using namespace libtorrent;
  GSList *ret = NULL;

  switch (btdemux->policy) {
    case GST_BT_DEMUX_SELECTOR_POLICY_ALL:
      {
        GSList *walk;

        /* copy the streams list */
        for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
          ret = g_slist_append (ret, gst_object_ref (stream));
        }
        break;
      }

    case GST_BT_DEMUX_SELECTOR_POLICY_LARGER:
      {
        gint max_file_size;
        session *s;
        std::vector<torrent_handle> torrents;
        int i;
        int index = 0;

        s = (session *)btdemux->session;
        torrents = s->get_torrents ();
        if (torrents.size () < 1)
          break;

        auto ti = torrents[0].torrent_file();
        if (!ti)
            break;
        if (ti->num_files () < 1)
          break;

        auto& files = ti->files();

        max_file_size = files.file_size(0);

        for (i = 1; i < ti->num_files (); i++) {
            gint file_size = files.file_size(i);

          /* get the larger file */
          if (file_size > max_file_size) {
              max_file_size = file_size;
            index = i;
          }
        }

        ret = g_slist_append (ret, gst_object_ref (g_slist_nth_data (
            btdemux->streams, index)));
      }

    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_check_no_more_pads (GstBtDemux * btdemux)
{
  GSList *walk;
  gboolean send = TRUE;

  /* whenever every requested stream has an active pad inform about the
   * no more pads
   */
  for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    if (stream->requested && !gst_pad_is_active (GST_PAD (stream)))
      send = FALSE;
    if (!stream->requested && gst_pad_is_active (GST_PAD (stream)))
      send = FALSE;
  }

  if (send) {
    GST_DEBUG_OBJECT (btdemux, "Sending no more pads");
    gst_element_no_more_pads (GST_ELEMENT (btdemux));
  }
}

static void
gst_bt_demux_send_buffering (GstBtDemux * btdemux, libtorrent::torrent_handle h)
{
  using namespace libtorrent;
  GSList *walk;
  int num_buffering = 0;
  int buffering = 0;
  gboolean start_pushing = FALSE;

  /* generate the real buffering level */
  for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    GST_BT_DEMUX_STREAM_LOCK(stream);

    if (!stream->requested) {
      GST_BT_DEMUX_STREAM_UNLOCK(stream);
      continue;
    }

    if (!stream->buffering) {
      GST_BT_DEMUX_STREAM_UNLOCK(stream);
      continue;
    }

    buffering += stream->buffering_level;
    /* unset the stream buffering */
    if (stream->buffering_level == 100) {
      stream->buffering = FALSE;
      stream->buffering_level = 0;
    }
    num_buffering++;
    GST_BT_DEMUX_STREAM_UNLOCK(stream);
  }

  if (num_buffering) {
    gdouble level = ((gdouble) buffering) / num_buffering;
    if (btdemux->buffering) {
      gst_element_post_message (GST_ELEMENT_CAST (btdemux),
          gst_message_new_buffering (GST_OBJECT_CAST (btdemux), level));
      if (level >= 100.0) {
        btdemux->buffering = FALSE;
        start_pushing = TRUE;
      }
    } else if (level < 100.0) {
      gst_element_post_message (GST_ELEMENT_CAST (btdemux),
          gst_message_new_buffering (GST_OBJECT_CAST (btdemux), level));
      btdemux->buffering = TRUE;
    }
  }

  if (!start_pushing)
      return;

  /* start pushing buffers on every stream */
    for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
      GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

      GST_BT_DEMUX_STREAM_LOCK(stream);

      if (stream->requested) {
          GST_DEBUG_OBJECT (btdemux, "Buffering finished, reading piece %d"
                  ", current: %d", stream->current_piece + 1,
                  stream->current_piece);
          h.read_piece (stream->current_piece + 1);
      }

      GST_BT_DEMUX_STREAM_UNLOCK(stream);
    }
}

static void
gst_bt_demux_activate_streams (GstBtDemux * btdemux)
{
  using namespace libtorrent;
  GSList *streams = NULL;
  GSList *walk;
  session *s;
  torrent_handle h;
  gboolean update_buffering = FALSE;
  
  GST_BT_DEMUX_STREAMS_LOCK(btdemux);
  if (!btdemux->streams) {
    GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
    return;
  }

  if (!btdemux->requested_streams) {
    /* use the policy */
    streams = gst_bt_demux_get_policy_streams (btdemux);
  } else {
    /* TODO use the value */
  }

  s = (session *)btdemux->session;
  h = s->get_torrents ()[0];

  /* mark every stream as not requested */
  for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    /* TODO set the priority to 0 on every piece */
    /* Actually inactivate it? */
    GST_BT_DEMUX_STREAM_LOCK(stream);
    stream->requested = FALSE;
    GST_BT_DEMUX_STREAM_UNLOCK(stream);
  }

  /* prioritize the first piece of every requested stream */
  for (walk = streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    GST_BT_DEMUX_STREAM_LOCK(stream);

    GST_DEBUG_OBJECT (btdemux, "Requesting stream %s", GST_PAD_NAME (stream));
    update_buffering |= gst_bt_demux_stream_activate_unlocked (stream, h,
        btdemux->buffer_pieces);
    GST_BT_DEMUX_STREAM_UNLOCK(stream);
  }

  /* wait for the buffering before reading pieces */
  if (update_buffering) {
    gst_bt_demux_send_buffering (btdemux, h);
  } else {
    /* start directly */
    for (walk = streams; walk; walk = g_slist_next (walk)) {
      GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

      GST_BT_DEMUX_STREAM_LOCK(stream);

      GST_DEBUG_OBJECT (btdemux, "Starting stream '%s', reading piece %d, "
          "current: %d", GST_PAD_NAME (stream), stream->start_piece,
          stream->current_piece);
      h.read_piece (stream->start_piece);

      GST_BT_DEMUX_STREAM_UNLOCK(stream);
    }
  }

  g_slist_free_full (streams, gst_object_unref);
  GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
}


/* thread reading messages from libtorrent */
static gboolean
gst_bt_demux_handle_alert (GstBtDemux * btdemux, const libtorrent::alert * a)
{
  using namespace libtorrent;
  gboolean ret = FALSE;

  GST_LOG_OBJECT (btdemux, "Received alert '%s'", a->what());

  switch (a->type()) {
    case add_torrent_alert::alert_type:
      {
        const add_torrent_alert *p = alert_cast<add_torrent_alert>(a);

        if (p->error) {
          GST_ELEMENT_ERROR (btdemux, STREAM, FAILED,
              ("Error while adding the torrent."),
              ("libtorrent says %s", p->error.message ().c_str ()));
          ret = TRUE;
        } else {
          GSList *walk;
          torrent_handle h = p->handle;
          int i;

          GST_INFO_OBJECT (btdemux, "Start downloading");
          GST_DEBUG_OBJECT (btdemux, "num files: %d, num pieces: %d, "
              "piece length: %d", p->params.ti->num_files (),
              p->params.ti->num_pieces (), p->params.ti->piece_length ());

          /* create the streams */
          for (i = 0; i < p->params.ti->num_files (); i++) {
            GstBtDemuxStream *stream;
            gchar *name;

            // file_entry fe;

            /* create the pads */
            name = g_strdup_printf ("src_%02d", i);

            /* initialize the streams */
            stream = (GstBtDemuxStream *) g_object_new (
                GST_TYPE_BT_DEMUX_STREAM, "name", name, "direction",
                GST_PAD_SRC, "template", gst_static_pad_template_get (&src_factory), NULL);
            g_free (name);

            /* set the idx */
            stream->idx = i;

            /* set the path */
            // TODO: verify this is correct!

            // fe =  p->params.ti->file_at (i);
            // stream->path = g_strdup (fe.path.c_str ());
            stream->path = g_strdup (p->params.ti->files().file_path(i).c_str());

            /* get the pieces and offsets related to the file */
            gst_bt_demux_stream_info (stream, h, &stream->start_offset,
                &stream->start_piece, &stream->end_offset, &stream->end_piece,
                &stream->end_byte);
            stream->start_byte = 0;
            stream->last_piece = stream->end_piece;

            GST_INFO_OBJECT (btdemux, "Adding stream %s for file '%s', "
                " start_piece: %d, start_offset: %d, end_piece: %d, "
                "end_offset: %d", GST_PAD_NAME (stream), stream->path,
                stream->start_piece, stream->start_offset, stream->end_piece,
                stream->end_offset);

            /* add it to our list of streams */
            btdemux->streams = g_slist_append (btdemux->streams, stream);
          }

          /* mark every piece to none-priority */
          for (i = 0; i < p->params.ti->num_pieces (); i++) {
            h.piece_priority (i, 0);
          }

          /* inform that we do know the available streams now */
          g_signal_emit (btdemux, gst_bt_demux_signals[SIGNAL_STREAMS_CHANGED], 0);

          /* make sure to download sequentially */
          h.set_sequential_download (true);
        }
        break;
      }

    case torrent_checked_alert::alert_type:
      {
        /* time to activate the streams */
        gst_bt_demux_activate_streams (btdemux);
        break;
      }

    case piece_finished_alert::alert_type:
      {
        GSList *walk;
        const piece_finished_alert *p = alert_cast<piece_finished_alert>(a);
        torrent_handle h = p->handle;
        torrent_status s = h.status();
        gboolean update_buffering = FALSE;

        GST_DEBUG_OBJECT (btdemux, "Piece %d completed (down: %d kb/s, "
            "up: %d kb/s, peers: %d)", p->piece_index, s.download_rate / 1000,
            s.upload_rate  / 1000, s.num_peers);

        GST_BT_DEMUX_STREAMS_LOCK(btdemux);

        /* read the piece once it is finished and send downstream in order */
        for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          GST_BT_DEMUX_STREAM_LOCK(stream);

          if (p->piece_index < stream->start_piece ||
              p->piece_index > stream->end_piece) {
              GST_BT_DEMUX_STREAM_UNLOCK(stream);
            continue;
          }

          if (!stream->requested) {
              GST_BT_DEMUX_STREAM_UNLOCK(stream);
            continue;
          }

          /* low the priority again */
          h.piece_priority (p->piece_index, 0);

          /* update the buffering */
          if (stream->buffering) {
            gst_bt_demux_stream_update_buffering_unlocked (stream, h, btdemux->buffer_pieces);
            update_buffering |= TRUE;
          }

          /* download the next piece */
          gst_bt_demux_stream_add_piece (stream, h, p->piece_index + 1,
              btdemux->buffer_pieces);

          GST_BT_DEMUX_STREAM_UNLOCK(stream);
        }

        if (update_buffering)
          gst_bt_demux_send_buffering (btdemux, h);

        GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
        break;
      }
    case read_piece_alert::alert_type:
      {
        GSList *walk;
        const read_piece_alert *p = alert_cast<read_piece_alert>(a);
        gboolean topology_changed = FALSE;

        GST_BT_DEMUX_STREAMS_LOCK(btdemux);

        /* read the piece once it is finished and send downstream in order */
        for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxBufferData *ipc_data;
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          GST_BT_DEMUX_STREAM_LOCK(stream);

          if (p->piece < stream->start_piece ||
              p->piece > stream->end_piece) {

            GST_BT_DEMUX_STREAM_UNLOCK(stream);
            continue;
          }

          /* in case the pad is active but not requested, disable it */
          if (gst_pad_is_active (GST_PAD (stream)) && !stream->requested) {
            topology_changed = TRUE;
            gst_pad_set_active (GST_PAD (stream), FALSE);
            gst_element_remove_pad (GST_ELEMENT (btdemux), GST_PAD (stream));
            gst_pad_stop_task (GST_PAD (stream));

            GST_BT_DEMUX_STREAM_UNLOCK(stream);
            continue;
          }

          if (!stream->requested) {
            GST_BT_DEMUX_STREAM_UNLOCK(stream);
            continue;
          }

          /* create the pad if needed */
          if (!gst_pad_is_active (GST_PAD (stream))) {
            gst_pad_set_active (GST_PAD (stream), TRUE);
            gst_element_add_pad (GST_ELEMENT (btdemux), GST_PAD (
                gst_object_ref (stream)));
            topology_changed = TRUE;

            if (btdemux->typefind) {
              GstTypeFindProbability prob;
              GstCaps *caps;
              GstBuffer *buf;

              buf = gst_bt_demux_buffer_new (p->buffer, p->piece, p->size,
                  stream);

              caps = gst_type_find_helper_for_buffer (GST_OBJECT (btdemux), buf, &prob);
              gst_buffer_unref (buf);

              if (caps) {
                gst_pad_set_caps (GST_PAD (stream), caps);
                gst_caps_unref (caps);
              }
            }
          }

          /* send the data to the stream thread */
          ipc_data = g_new0 (GstBtDemuxBufferData, 1);
          ipc_data->buffer = p->buffer;
          ipc_data->piece = p->piece;
          ipc_data->size = p->size;
          g_async_queue_push (stream->ipc, ipc_data);

          /* start the task */
          gst_pad_start_task (GST_PAD (stream), gst_bt_demux_stream_push_loop,
              stream, NULL);

          GST_BT_DEMUX_STREAM_UNLOCK(stream);
        }

        if (topology_changed)
          gst_bt_demux_check_no_more_pads (btdemux);

        GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);
      }
      break;

    case torrent_removed_alert::alert_type:
      /* a safe cleanup, the torrent has been removed */
      ret = TRUE;
      break;

    case file_completed_alert::alert_type:
      /* TODO send the EOS downstream */
      /* TODO mark ourselves as done */
      /* TODO quit the main btdemux loop */
      break;

    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_loop (gpointer user_data)
{
  using namespace libtorrent;

  GstBtDemux *btdemux;
  session *s;
  
  btdemux = GST_BT_DEMUX (user_data);
  s = (session *)btdemux->session;

  while (!btdemux->finished) {
    if (s->wait_for_alert (libtorrent::seconds(10)) != NULL) {
      std::vector<alert*> alerts;
      s->pop_alerts(&alerts);

      /* handle every alert */
      for (const alert * a : alerts) {
          if (!btdemux->finished)
              btdemux->finished = gst_bt_demux_handle_alert (btdemux, a);
      }
    }
  }

  gst_task_stop (btdemux->task);
}

static void
gst_bt_demux_task_setup (GstBtDemux * btdemux)
{
  /* to pop from the libtorrent async system */
  btdemux->task = gst_task_new (gst_bt_demux_loop, btdemux, NULL);
  gst_task_set_lock (btdemux->task, &btdemux->task_lock);
  gst_task_start (btdemux->task);
}

static void
gst_bt_demux_task_cleanup (GstBtDemux * btdemux)
{
  using namespace libtorrent;
  GSList *walk;
  session *s;
  std::vector<torrent_handle> torrents;

  /* pause every task */
  GST_BT_DEMUX_STREAMS_LOCK(btdemux);
  for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
    GstBtDemuxBufferData *ipc_data;

    /* send a cleanup buffer */
    ipc_data = g_new0 (GstBtDemuxBufferData, 1);
    g_async_queue_push (stream->ipc, ipc_data);
    gst_pad_stop_task (GST_PAD (stream));
  }
  GST_BT_DEMUX_STREAMS_UNLOCK(btdemux);

  s = (session *)btdemux->session;
  torrents = s->get_torrents ();

  if (torrents.size () < 1) {
    /* nothing added, stop the task directly */
    btdemux->finished = TRUE;
  } else {
    torrent_handle h;
    h = torrents[0];
    s->remove_torrent (h);
  }
  
  /* given that the pads are removed on the parent class at the paused
   * to ready state, we need to exit the task and wait for it
   */
  if (btdemux->task) { 
    gst_task_stop (btdemux->task);
    gst_task_join (btdemux->task);
    gst_object_unref (btdemux->task);
    btdemux->task = NULL;
  }
}

static void
gst_bt_demux_cleanup (GstBtDemux * btdemux)
{
  /* remove every pad reference */
  if (btdemux->streams) {
    /* finally remove the files if we need to */
    if (btdemux->temp_remove) {
      GSList *walk;

      for (walk = btdemux->streams; walk; walk = g_slist_next (walk)) {
        GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
        gchar *to_remove;

        to_remove = g_build_path (G_DIR_SEPARATOR_S, btdemux->temp_location,
            stream->path, NULL);
        g_remove (to_remove);
        g_free (to_remove);
      }
    }

    g_slist_free_full (btdemux->streams, gst_object_unref);
    btdemux->streams = NULL;
  }
}

static GstStateChangeReturn
gst_bt_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstBtDemux * btdemux;
  GstStateChangeReturn ret;

  btdemux = GST_BT_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_bt_demux_task_setup (btdemux);
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_bt_demux_task_cleanup (btdemux);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_bt_demux_parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_bt_demux_cleanup (btdemux);
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      break;

    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_dispose (GObject * object)
{
  GstBtDemux *btdemux;

  btdemux = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (btdemux, "Disposing");

  gst_bt_demux_task_cleanup (btdemux);
  gst_bt_demux_cleanup (btdemux);

  if (btdemux->session) {
    libtorrent::session *session;

    session = (libtorrent::session *)btdemux->session;
    delete (session);
    btdemux->session = NULL;
  }

  if (btdemux->adapter) {
    g_object_unref (btdemux->adapter);
    btdemux->adapter = NULL;
  }

  g_free (btdemux->temp_location);

  G_OBJECT_CLASS (gst_bt_demux_parent_class)->dispose (object);
}

static void
gst_bt_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBtDemux *btdemux;

  g_return_if_fail (GST_IS_BT_DEMUX (object));

  btdemux = GST_BT_DEMUX (object);

  switch (prop_id) {
    case PROP_SELECTOR_POLICY:
      btdemux->policy = (GstBtDemuxSelectorPolicy)g_value_get_enum (value);
      break;

    case PROP_TYPEFIND:
      btdemux->typefind = g_value_get_boolean (value);
      break;

    case PROP_TEMP_REMOVE:
      btdemux->temp_remove = g_value_get_boolean (value);
      break;

    case PROP_TEMP_LOCATION:
      g_free (btdemux->temp_location);
      btdemux->temp_location = g_strdup (g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_bt_demux_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstBtDemux *btdemux;

  g_return_if_fail (GST_IS_BT_DEMUX (object));

  btdemux = GST_BT_DEMUX (object);

  switch (prop_id) {
    case PROP_N_STREAMS:
      g_value_set_int (value, g_slist_length (btdemux->streams));
      break;

    case PROP_SELECTOR_POLICY:
      g_value_set_enum (value, btdemux->policy);
      break;

    case PROP_TYPEFIND:
      g_value_set_boolean (value, btdemux->typefind);
      break;

    case PROP_TEMP_REMOVE:
      g_value_set_boolean (value, btdemux->temp_remove);
      break;

    case PROP_TEMP_LOCATION:
      g_value_set_string (value, btdemux->temp_location);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_bt_demux_class_init (GstBtDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gst_bt_demux_parent_class = g_type_class_peek_parent (klass);

  /* initialize the object class */
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_bt_demux_dispose);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_bt_demux_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_bt_demux_get_property);
  g_object_class_install_property (gobject_class, PROP_N_STREAMS,
      g_param_spec_int ("n-streams", "Number of streams",
          "Get the total number of available streams",
          0, G_MAXINT, 0, G_PARAM_READABLE));
  g_object_class_install_property (gobject_class, PROP_SELECTOR_POLICY,
      g_param_spec_enum ("selector-policy", "Stream selector policy",
          "Specifies the automatic stream selector policy when no stream is "
          "selected", gst_bt_demux_selector_policy_get_type(),
          0, (GParamFlags)G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_TYPEFIND,
      g_param_spec_boolean ("typefind", "Typefind",
          "Run typefind before negotiating", DEFAULT_TYPEFIND,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_TEMP_LOCATION,
      g_param_spec_string ("temp-location", "Temporary File Location",
          "Location to store temporary files in", NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
  g_object_class_install_property (gobject_class, PROP_TEMP_REMOVE,
      g_param_spec_boolean ("temp-remove", "Remove temporary files",
          "Remove temporary files", DEFAULT_TEMP_REMOVE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  gst_bt_demux_signals[SIGNAL_STREAMS_CHANGED] =
      g_signal_new ("streams-changed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GstBtDemuxClass,
          streams_changed), NULL, NULL, g_cclosure_marshal_VOID__VOID,
      G_TYPE_NONE, 0);
  gst_bt_demux_signals[SIGNAL_GET_STREAM_TAGS] =
      g_signal_new ("get-stream-tags", G_TYPE_FROM_CLASS (klass),
      (GSignalFlags) (G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION),
      G_STRUCT_OFFSET (GstBtDemuxClass, get_stream_tags), NULL, NULL,
      gst_bt_demux_cclosure_marshal_BOXED__INT, GST_TYPE_TAG_LIST, 1,
      G_TYPE_INT);

  /* initialize the element class */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_factory));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_bt_demux_change_state);

  gst_element_class_set_details_simple (element_class,
      "BitTorrent Demuxer", "Codec/Demuxer",
      "Streams a BitTorrent file",
      "Jorge Luis Zapata <jorgeluis.zapata@gmail.com>");

  /* initialize the demuxer class */
  klass->get_stream_tags = gst_bt_demux_get_stream_tags;
}
 
static void
gst_bt_demux_init (GstBtDemux * btdemux)
{
  using namespace libtorrent;

  GstPad *pad;

  session *s;
  settings_pack session_settings;
  
  pad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (pad, gst_bt_demux_sink_chain);
  gst_pad_set_event_function (pad, gst_bt_demux_sink_event);

  gst_element_add_pad (GST_ELEMENT (btdemux), pad);

  /* to store the buffers from upstream until we have a full torrent file */
  btdemux->adapter = gst_adapter_new ();

  /* set the error alerts and the progress alerts */
  session_settings.set_int(settings_pack::alert_mask,
          alert::error_notification | alert::progress_notification | alert::status_notification);
  /* create a new session */
  s = new session (session_settings);
 btdemux->session = s;

  g_rec_mutex_init (&btdemux->task_lock);

  /* default properties */
  btdemux->policy = GST_BT_DEMUX_SELECTOR_POLICY_LARGER;
  btdemux->buffer_pieces = DEFAULT_BUFFER_PIECES;
  btdemux->typefind = DEFAULT_TYPEFIND;
  btdemux->temp_location = g_build_path (G_DIR_SEPARATOR_S, g_get_tmp_dir (), DEFAULT_DIR,
      NULL);
  btdemux->temp_remove = DEFAULT_TEMP_REMOVE;
}
