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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt_demux.hpp"
#include <gst/base/gsttypefindhelper.h>

#include <glib/gstdio.h>

#include <iterator>
#include <deque>

#include "libtorrent/session.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/time.hpp"

#define DEFAULT_TYPEFIND TRUE
#define DEFAULT_BUFFER_PIECES 3
#define DEFAULT_DIR "btdemux"
#define DEFAULT_TEMP_REMOVE TRUE

GST_DEBUG_CATEGORY_EXTERN (gst_bt_demux_debug);
#define GST_CAT_DEFAULT gst_bt_demux_debug

/* Forward declarations */
static void
gst_bt_demux_send_buffering (GstBtDemux * thiz, libtorrent::torrent_handle h);

typedef struct _GstBtDemuxBufferData
{
  boost::shared_array <char> buffer;
  int piece;
  int size;
} GstBtDemuxBufferData;

static void gst_bt_demux_buffer_data_free (gpointer data)
{
  g_free (data);
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
gst_bt_demux_stream_start_buffering (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int max_pieces)
{
  using namespace libtorrent;
  int i;
  int start = thiz->current_piece + 1;
  int end = thiz->current_piece + max_pieces;

  /* do not overflow */
  if (end > thiz->end_piece)
    end = thiz->end_piece;

  /* count how many consecutive pieces need to be downloaded */
  thiz->buffering_count = 0;
  for (i = start; i <= end; i++) {

    /* already downloaded */
    if (h.have_piece (i))
      continue;

    thiz->buffering_count++;
  }

  if (thiz->buffering_count) {
    thiz->buffering = TRUE;
    thiz->buffering_level = 0;
    return TRUE;
  } else {
    return FALSE;
  }
}

static void
gst_bt_demux_stream_update_buffering (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int max_pieces)
{
  using namespace libtorrent;
  int i;
  int start = thiz->current_piece + 1;
  int end = thiz->current_piece + max_pieces;
  int buffered_pieces = 0;

  /* do not overflow */
  if (end > thiz->end_piece)
    end = thiz->end_piece;

  /* count how many consecutive pieces have been downloaded */
  for (i = start; i <= end; i++) {
    if (h.have_piece (i))
      buffered_pieces++;
  }

  if (buffered_pieces > thiz->buffering_count)
    buffered_pieces = thiz->buffering_count;

  thiz->buffering = TRUE;
  thiz->buffering_level = (buffered_pieces * 100) / thiz->buffering_count;
  GST_DEBUG_OBJECT (thiz, "Buffering level %d (%d/%d)", thiz->buffering_level,
      buffered_pieces, thiz->buffering_count);
}

static void
gst_bt_demux_stream_add_piece (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int piece, int max_pieces)
{
  using namespace libtorrent;

  GST_DEBUG_OBJECT (thiz, "Adding more pieces at %d, current: %d, "
      "max: %d", piece, thiz->current_piece, max_pieces);
  for (piece; piece <= thiz->end_piece; piece++) {
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
    GST_DEBUG_OBJECT (thiz, "Requesting piece %d, prio: %d, current: %d, ",
        piece, priority, thiz->current_piece);
    break;
  }
}

static gboolean
gst_bt_demux_stream_activate (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int max_pieces)
{
  gboolean ret = FALSE;

  thiz->requested = TRUE;
  thiz->current_piece = thiz->start_piece - 1;
  thiz->pending_segment = TRUE;

  GST_DEBUG_OBJECT (thiz, "Activating stream '%s', start: %d, "
      "start_offset: %d, end: %d, end_offset: %d, current: %d",
      GST_PAD_NAME (thiz), thiz->start_piece, thiz->start_offset,
      thiz->end_piece, thiz->end_offset, thiz->current_piece);

  if (h.have_piece (thiz->start_piece)) {
    /* request the first non-downloaded piece */
    if (thiz->start_piece != thiz->end_piece) {
      int i;

      for (i = 1; i < max_pieces; i++) {
        gst_bt_demux_stream_add_piece (thiz, h, thiz->start_piece + i,
            max_pieces);
      }
    }

  } else {
    int i;

    for (i = 0; i < max_pieces; i++) {
      gst_bt_demux_stream_add_piece (thiz, h, thiz->start_piece + i,
          max_pieces);
    }
    /* start the buffering */
    gst_bt_demux_stream_start_buffering (thiz, h, max_pieces);
    ret = TRUE;
  }
  return ret;
}

static void
gst_bt_demux_stream_info (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, gint * start_offset,
    gint * start_piece, gint * end_offset, gint * end_piece,
    gint64 * size)
{
  using namespace libtorrent;
  file_entry fe;
  int piece_length;
  torrent_info ti = h.get_torrent_info ();

  piece_length = ti.piece_length ();
  fe = ti.file_at (thiz->idx);
  if (start_piece)
    *start_piece = fe.offset / piece_length;
  if (start_offset)
    *start_offset = fe.offset % piece_length;
  if (end_piece)
    *end_piece = (fe.offset + fe.size) / piece_length;
  if (end_offset)
    *end_offset = (fe.offset + fe.size) % piece_length;
  if (size)
    *size = fe.size;
}

static gboolean
gst_bt_demux_stream_seek (GstBtDemuxStream * thiz, GstEvent * event)
{
  using namespace libtorrent;
  GstBtDemux *demux;
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

  demux = GST_BT_DEMUX (gst_pad_get_parent (GST_PAD (thiz)));
  s = (session *)demux->session;
  h = s->get_torrents ()[0];
  gst_object_unref (demux);

  /* get the piece length */
  torrent_info ti = h.get_torrent_info ();
  piece_length = ti.piece_length ();

  gst_event_parse_seek (event, &rate, &format, &flags, &start_type,
      &start, &stop_type, &stop);

  /* sanitize stuff */
  if (format != GST_FORMAT_BYTES)
    goto beach;

  if (rate < 0.0)
    goto beach;

  gst_bt_demux_stream_info (thiz, h, &start_offset,
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

  g_mutex_lock (thiz->lock);

  if (flags & GST_SEEK_FLAG_FLUSH) {
    gst_pad_push_event (GST_PAD (thiz), gst_event_new_flush_start ());
    gst_pad_push_event (GST_PAD (thiz), gst_event_new_flush_stop ());
  } else {
    /* TODO we need to close the segment */
  }

  /* update the stream segment */
  thiz->start_byte = start;
  thiz->end_byte = stop,

  thiz->end_piece = start_piece + ((stop + start_offset) / piece_length);
  thiz->end_offset = start_piece + ((stop + start_offset) % piece_length); 

  tmp = start_piece;
  thiz->start_piece = start_piece + ((start + start_offset) / piece_length);
  thiz->start_offset = tmp + ((start + start_offset) % piece_length);

  GST_DEBUG_OBJECT (thiz, "Seeking to, start: %d, start_offset: %d, end: %d, "
      "end_offset: %d", thiz->start_piece, thiz->start_offset,
      thiz->end_piece, thiz->end_offset);

  /* activate again this stream */
  update_buffering = gst_bt_demux_stream_activate (thiz, h,
      demux->buffer_pieces);
  if (!update_buffering) {
    /* FIXME what if the demuxer is already buffering ? */
    /* start directly */
    GST_DEBUG_OBJECT (thiz, "Starting stream '%s', reading piece %d, "
        "current: %d", GST_PAD_NAME (thiz), thiz->start_piece,
        thiz->current_piece);
    h.read_piece (thiz->start_piece);
    g_mutex_unlock (thiz->lock);
  }

  ret = TRUE;

  g_mutex_unlock (thiz->lock);

  /* send the buffering if we need to */
  if (update_buffering)
    gst_bt_demux_send_buffering (demux, h);

beach:
  return ret;
}

static gboolean
gst_bt_demux_stream_event (GstPad * pad, GstEvent * event)
{
  GstBtDemuxStream *thiz;
  gboolean ret = FALSE;

  thiz = GST_BT_DEMUX_STREAM (pad);

  GST_DEBUG_OBJECT (thiz, "Event %s", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEEK:
      {
        ret = gst_bt_demux_stream_seek (thiz, event);
        break;
      }
  }

  return ret;
}

static gboolean
gst_bt_demux_stream_query (GstPad * pad, GstQuery * query)
{
  using namespace libtorrent;
  GstBtDemuxStream *thiz;
  gboolean ret = FALSE;

  thiz = GST_BT_DEMUX_STREAM (pad);

  GST_DEBUG_OBJECT (thiz, "Quering %s", GST_QUERY_TYPE_NAME (query));

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
        GstBtDemux *demux;
        session *s;
        torrent_handle h;
        gint64 bytes;

        demux = GST_BT_DEMUX (gst_pad_get_parent (GST_PAD (thiz)));
        s = (session *)demux->session;
        h = s->get_torrents ()[0];
        gst_object_unref (demux);

        gst_bt_demux_stream_info (thiz, h, NULL, NULL, NULL, NULL, &bytes);
        gst_query_set_duration (query, GST_FORMAT_BYTES, bytes);

        ret = TRUE;
      }
      break;

    case GST_QUERY_BUFFERING:
      {
        GstFormat format;

        gst_query_parse_buffering_range (query, &format, NULL, NULL, NULL);
        if (format == GST_FORMAT_BYTES) {
          GstBtDemux *demux;
          session *s;
          torrent_handle h;
          std::vector<partial_piece_info> pi;

          demux = GST_BT_DEMUX (gst_pad_get_parent (GST_PAD (thiz)));
          s = (session *)demux->session;
          h = s->get_torrents ()[0];
          gst_object_unref (demux);

          h.get_download_queue (pi);
          for (std::vector<partial_piece_info>::iterator it = pi.begin();
              it != pi.end(); ++it) {

            /* all the piece is finished */
            if (it->finished == it->blocks_in_piece) {
              // gst_query_add_buffering_range (query, pi...);
            }
          }
          ret = TRUE;
        }
        break;
      }

    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_stream_dispose (GObject * object)
{
  GstBtDemuxStream *thiz;

  thiz = GST_BT_DEMUX_STREAM (object);

  if (thiz->path) {
    g_free (thiz->path);
  }
  g_mutex_free (thiz->lock);

  GST_DEBUG_OBJECT (thiz, "Disposing");

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
gst_bt_demux_stream_init (GstBtDemuxStream * thiz)
{
  thiz->lock = g_mutex_new ();

  gst_pad_set_event_function (GST_PAD (thiz),
      GST_DEBUG_FUNCPTR (gst_bt_demux_stream_event));
  gst_pad_set_query_function (GST_PAD (thiz),
      GST_DEBUG_FUNCPTR (gst_bt_demux_stream_query));
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
gst_bt_demux_sink_chain (GstPad * pad, GstBuffer * buffer)
{
  GstBtDemux *thiz;
  GstFlowReturn ret;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Received buffer");
  gst_adapter_push (thiz->adapter, gst_buffer_ref (buffer));
  gst_object_unref (thiz);

  return GST_FLOW_OK;
}

/* On EOS, start processing the file */
static gboolean
gst_bt_demux_sink_event (GstPad * pad, GstEvent * event)
{
  GstBtDemux *thiz;
  GstPad *peer;
  GstBuffer *buf;
  GstMessage *message;
  gint len;
  gboolean res = TRUE;
  libtorrent::torrent_info *torrent_info;

  if (GST_EVENT_TYPE (event) != GST_EVENT_EOS)
    goto beach;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Received EOS");
  len = gst_adapter_available (thiz->adapter);
  buf = gst_adapter_take_buffer (thiz->adapter, len);

  /* Time to process */
  torrent_info = new libtorrent::torrent_info (
    (const char *)GST_BUFFER_DATA (buf), GST_BUFFER_SIZE (buf));
  gst_buffer_unref (buf);

  if (torrent_info) {
    libtorrent::session *session;
    libtorrent::add_torrent_params tp;

    tp.ti = torrent_info;
    tp.save_path = thiz->temp_location;

    session = (libtorrent::session *)thiz->session;
    session->async_add_torrent (tp);
  } else {
    /* TODO Send an error message */
  }

  gst_object_unref (thiz);
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
gst_bt_demux_get_stream_tags (GstBtDemux * thiz, gint stream)
{
  using namespace libtorrent;
  session *s;
  file_entry fe;
  std::vector<torrent_handle> torrents;
  int i;

  if (!thiz->streams)
    return NULL;

  /* get the torrent_info */
  s = (session *)thiz->session;
  torrents = s->get_torrents ();

  torrent_info ti = torrents[0].get_torrent_info ();

  for (i = 0; i < ti.num_files (); i++) {
    file_entry fe = ti.file_at (i);

    /* TODO get the stream tags (i.e metadata) */
    /* set the file name */
    /* set the file size */
  }
  
  return NULL;
}

static GSList *
gst_bt_demux_get_policy_streams (GstBtDemux * thiz)
{
  using namespace libtorrent;
  GSList *ret = NULL;

  switch (thiz->policy) {
    case GST_BT_DEMUX_SELECTOR_POLICY_ALL:
      {
        GSList *walk;

        /* copy the streams list */
        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
          ret = g_slist_append (ret, gst_object_ref (stream));
        }
        break;
      }

    case GST_BT_DEMUX_SELECTOR_POLICY_LARGER:
      {
        file_entry fe;
        session *s;
        std::vector<torrent_handle> torrents;
        int i;
        int index = 0;

        s = (session *)thiz->session;
        torrents = s->get_torrents ();
        if (torrents.size () < 1)
          break;

        torrent_info ti = torrents[0].get_torrent_info ();
        if (ti.num_files () < 1)
          break;

        fe = ti.file_at (0);
        for (i = 0; i < ti.num_files (); i++) {
          file_entry fee = ti.file_at (i);

          /* get the larger file */
          if (fee.size > fe.size) {
            fe = fee;
            index = i;
          }
        }

        ret = g_slist_append (ret, gst_object_ref (g_slist_nth_data (
            thiz->streams, index)));
      }

    default:
      break;
  }

  return ret;
}

static void
gst_bt_demux_check_no_more_pads (GstBtDemux * thiz)
{
  GSList *walk;
  gboolean send = TRUE;

  /* whenever every requested stream has an active pad inform about the
   * no more pads
   */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    if (stream->requested && !gst_pad_is_active (GST_PAD (stream)))
      send = FALSE;
    if (!stream->requested && gst_pad_is_active (GST_PAD (stream)))
      send = FALSE;
  }

  if (send) {
    GST_DEBUG_OBJECT (thiz, "Sending no more pads");
    gst_element_no_more_pads (GST_ELEMENT (thiz));
  }
}

static void
gst_bt_demux_send_buffering (GstBtDemux * thiz, libtorrent::torrent_handle h)
{
  using namespace libtorrent;
  GSList *walk;
  int num_buffering = 0;
  int buffering = 0;
  gboolean start_pushing = FALSE;

  /* generate the real buffering level */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    g_mutex_lock (stream->lock);

    if (!stream->requested) {
      g_mutex_unlock (stream->lock);
      continue;
    }

    if (!stream->buffering) {
      g_mutex_unlock (stream->lock);
      continue;
    }

    buffering += stream->buffering_level;
    /* unset the stream buffering */
    if (stream->buffering_level == 100) {
      stream->buffering = FALSE;
      stream->buffering_level = 0;
    }
    num_buffering++;
    g_mutex_unlock (stream->lock);
  }

  if (num_buffering) {
    gdouble level = ((gdouble) buffering) / num_buffering;
    if (thiz->buffering) {
      gst_element_post_message (GST_ELEMENT_CAST (thiz),
          gst_message_new_buffering (GST_OBJECT_CAST (thiz), level));
      if (level >= 100.0) {
        thiz->buffering = FALSE;
        start_pushing = TRUE;
      }
    } else if (level < 100.0) {
      gst_element_post_message (GST_ELEMENT_CAST (thiz),
          gst_message_new_buffering (GST_OBJECT_CAST (thiz), level));
      thiz->buffering = TRUE;
    }
  }

  /* start pushing buffers on every stream */
  if (start_pushing) {
    for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
      GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

      g_mutex_lock (stream->lock);
      if (!stream->requested) {
        g_mutex_unlock (stream->lock);
        continue;
      }

      GST_DEBUG_OBJECT (thiz, "Buffering finished, reading piece %d"
          ", current: %d", stream->current_piece + 1,
          stream->current_piece);
      h.read_piece (stream->current_piece + 1);
      g_mutex_unlock (stream->lock);
    }
  }
}

static void
gst_bt_demux_activate_streams (GstBtDemux * thiz)
{
  using namespace libtorrent;
  GSList *streams = NULL;
  GSList *walk;
  session *s;
  torrent_handle h;
  gboolean update_buffering = FALSE;


  g_mutex_lock (thiz->streams_lock);
  if (!thiz->streams) {
    g_mutex_unlock (thiz->streams_lock);
    return;
  }

  if (!thiz->requested_streams) {
    /* use the policy */
    streams = gst_bt_demux_get_policy_streams (thiz);
  } else {
    /* TODO use the value */
  }

  s = (session *)thiz->session;
  h = s->get_torrents ()[0];

  /* mark every stream as not requested */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    /* TODO set the priority to 0 on every piece */
    /* Actually inactivate it? */
    g_mutex_lock (stream->lock);
    stream->requested = FALSE;
    g_mutex_unlock (stream->lock);
  }

  /* prioritize the first piece of every requested stream */
  for (walk = streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    g_mutex_lock (stream->lock);
    GST_DEBUG_OBJECT (thiz, "Requesting stream %s", GST_PAD_NAME (stream));
    update_buffering |= gst_bt_demux_stream_activate (stream, h,
        thiz->buffer_pieces);
    g_mutex_unlock (stream->lock);
  }

  /* wait for the buffering before reading pieces */
  if (update_buffering) {
    gst_bt_demux_send_buffering (thiz, h);
  } else {
    /* start directly */
    for (walk = streams; walk; walk = g_slist_next (walk)) {
      GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

      g_mutex_lock (stream->lock);
      GST_DEBUG_OBJECT (thiz, "Starting stream '%s', reading piece %d, "
          "current: %d", GST_PAD_NAME (stream), stream->start_piece,
          stream->current_piece);
      h.read_piece (stream->start_piece);
      g_mutex_unlock (stream->lock);
    }
  }

  g_slist_free_full (streams, gst_object_unref);
  g_mutex_unlock (thiz->streams_lock);
}


/* thread reading messages from libtorrent */
static gboolean
gst_bt_demux_handle_alert (GstBtDemux * thiz, libtorrent::alert * a)
{
  using namespace libtorrent;
  gboolean ret = FALSE;

  GST_LOG_OBJECT (thiz, "Received alert '%s'", a->what());

  switch (a->type()) {
    case add_torrent_alert::alert_type:
      {
        add_torrent_alert *p = alert_cast<add_torrent_alert>(a);

        if (p->error) {
          /* TODO send an error */
          GST_ERROR_OBJECT (thiz, "Error '%s' while adding the torrent",
              p->error.message ().c_str ());
        } else {
          GSList *walk;
          torrent_handle h = p->handle;
          int i;

          GST_INFO_OBJECT (thiz, "Start downloading");
          GST_DEBUG_OBJECT (thiz, "num files: %d, num pieces: %d, "
              "piece length: %d", p->params.ti->num_files (),
              p->params.ti->num_pieces (), p->params.ti->piece_length ());

          /* create the streams */
          for (i = 0; i < p->params.ti->num_files (); i++) {
            GstBtDemuxStream *stream;
            gchar *name;
            file_entry fe;

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
            fe =  p->params.ti->file_at (i);
            stream->path = g_strdup (fe.path.c_str ());

            /* get the pieces and offsets related to the file */
            stream->start_byte = 0;
            gst_bt_demux_stream_info (stream, h, &stream->start_offset,
                &stream->start_piece, &stream->end_offset, &stream->end_piece,
                &stream->end_byte);

            GST_INFO_OBJECT (thiz, "Adding stream %s for file '%s', "
                " start_piece: %d, start_offset: %d, end_piece: %d, "
                "end_offset: %d", GST_PAD_NAME (stream), stream->path,
                stream->start_piece, stream->start_offset, stream->end_piece,
                stream->end_offset);

            /* add it to our list of streams */
            thiz->streams = g_slist_append (thiz->streams, stream);
          }

          /* mark every piece to none-priority */
          for (i = 0; i < p->params.ti->num_pieces (); i++) {
            h.piece_priority (i, 0);
          }

          /* inform that we do know the available streams now */
          g_signal_emit (thiz, gst_bt_demux_signals[SIGNAL_STREAMS_CHANGED], 0);

          /* make sure to download sequentially */
          h.set_sequential_download (true);
        }
        break;
      }

    case torrent_checked_alert::alert_type:
      {
        /* time to activate the streams */
        gst_bt_demux_activate_streams (thiz);
        break;
      }

    case piece_finished_alert::alert_type:
      {
        GSList *walk;
        piece_finished_alert *p = alert_cast<piece_finished_alert>(a);
        torrent_handle h = p->handle;
        torrent_status s = h.status();
        gboolean update_buffering = FALSE;

        GST_DEBUG_OBJECT (thiz, "Piece %d completed (down: %d kb/s, "
            "up: %d kb/s, peers: %d)", p->piece_index, s.download_rate / 1000,
            s.upload_rate  / 1000, s.num_peers);

        g_mutex_lock (thiz->streams_lock);
        /* read the piece once it is finished and send downstream in order */
        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          g_mutex_lock (stream->lock);
          if (p->piece_index < stream->start_piece ||
              p->piece_index > stream->end_piece) {
            g_mutex_unlock (stream->lock);
            continue;
          }

          if (!stream->requested) {
            g_mutex_unlock (stream->lock);
            continue;
          }

          /* low the priority again */
          h.piece_priority (p->piece_index, 0);

          /* update the buffering */
          if (stream->buffering) {
            gst_bt_demux_stream_update_buffering (stream, h, thiz->buffer_pieces);
            update_buffering |= TRUE;
          }

          /* download the next piece */
          gst_bt_demux_stream_add_piece (stream, h, p->piece_index + 1,
              thiz->buffer_pieces);
          g_mutex_unlock (stream->lock);
        }

        if (update_buffering)
          gst_bt_demux_send_buffering (thiz, h);

        g_mutex_unlock (thiz->streams_lock);
        break;
      }
    case read_piece_alert::alert_type:
      {
        GstBtDemuxBufferData *ipc_data;
        read_piece_alert *p = alert_cast<read_piece_alert>(a);

        /* send the data to the other thread */
        ipc_data = g_new0 (GstBtDemuxBufferData, 1);
        ipc_data->buffer = p->buffer;
        ipc_data->piece = p->piece;
        ipc_data->size = p->size;

        g_async_queue_push (thiz->ipc, ipc_data);
      }
      break;

    case torrent_removed_alert::alert_type:
      /* a safe cleanup, the torrent has been removed */
      ret = TRUE;
      break;

    case file_completed_alert::alert_type:
      /* TODO send the EOS downstream */
      /* TODO mark ourselves as done */
      /* TODO quit the main demux loop */
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
  GstBtDemux *thiz;
  
  thiz = GST_BT_DEMUX (user_data);
  while (!thiz->finished) {
    session *s;
    s = (session *)thiz->session;

    if (s->wait_for_alert (libtorrent::seconds(10)) != NULL) {
      std::deque<alert*> alerts;
      s->pop_alerts(&alerts);

      /* handle every alert */
      for (std::deque<libtorrent::alert*>::iterator i = alerts.begin(),
          end(alerts.end()); i != end; ++i) {

        if (!thiz->finished)
          thiz->finished = gst_bt_demux_handle_alert (thiz, *i);
        delete *i;
      }
      alerts.clear();
    }
  }
  gst_task_stop (thiz->task);
}

static void
gst_bt_demux_push_loop (gpointer user_data)
{
  using namespace libtorrent;
  GstBtDemux *thiz;
  GstBtDemuxBufferData *ipc_data;
  GSList *walk;
  session *s;
  torrent_handle h;
  gboolean topology_changed = FALSE;
  gboolean update_buffering = FALSE;
  
  thiz = GST_BT_DEMUX (user_data);
  if (thiz->finished)
    return;

  ipc_data = (GstBtDemuxBufferData *)g_async_queue_pop (thiz->ipc);
  if (!ipc_data)
    return;

  if (!ipc_data->size) {
    gst_bt_demux_buffer_data_free (ipc_data);
    return;
  }

  s = (session *)thiz->session;
  h = s->get_torrents ()[0];

  /* get the stream this buffer belongs to */
  g_mutex_lock (thiz->streams_lock);
  /* send the data downstream */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
    GstBuffer *buf;
    GstFlowReturn ret;
    GstBtDemuxBufferData *buf_data;

    g_mutex_lock (stream->lock);
    if (ipc_data->piece < stream->start_piece && ipc_data->piece > stream->end_piece) {
      g_mutex_unlock (stream->lock);
      continue;
    }

    /* in case the pad is active but not requested, disable it */
    if (gst_pad_is_active (GST_PAD (stream)) && !stream->requested) {
      topology_changed = TRUE;
      gst_pad_set_active (GST_PAD (stream), FALSE);
      gst_element_remove_pad (GST_ELEMENT (thiz), GST_PAD (stream));
      g_mutex_unlock (stream->lock);
      continue;
    }

    if (!stream->requested) {
      g_mutex_unlock (stream->lock);
      continue;
    }

    /* in case we are not expecting this buffer */
    if (ipc_data->piece != stream->current_piece + 1) {
      GST_DEBUG_OBJECT (thiz, "Dropping piece %d, waiting for %d on "
          "file %d", ipc_data->piece, stream->current_piece + 1, stream->idx);
      g_mutex_unlock (stream->lock);
      continue;
    }

    /* create the buffer */
    buf = gst_buffer_new ();
    GST_BUFFER_DATA (buf) = (guint8 *)ipc_data->buffer.get ();

    buf_data = g_new0 (GstBtDemuxBufferData, 1);
    buf_data->buffer = ipc_data->buffer;
    GST_BUFFER_MALLOCDATA (buf) = (guint8 *)buf_data;
    GST_BUFFER_FREE_FUNC (buf) = gst_bt_demux_buffer_data_free;

    GST_BUFFER_SIZE (buf) = ipc_data->size;
    /* handle the offsets */
    if (ipc_data->piece == stream->start_piece) {
      GST_BUFFER_DATA (buf) = GST_BUFFER_DATA (buf) + stream->start_offset;
      GST_BUFFER_SIZE (buf) -= stream->start_offset;
    }

    if (ipc_data->piece == stream->end_piece) {
      GST_BUFFER_SIZE (buf) -= GST_BUFFER_SIZE (buf) - stream->end_offset;
    }

    /* create the pad if needed */
    if (!gst_pad_is_active (GST_PAD (stream))) {
      if (thiz->typefind) {
        GstTypeFindProbability prob;
        GstCaps *caps;

        caps = gst_type_find_helper_for_buffer (GST_OBJECT (thiz), buf, &prob);
        if (caps) {
          gst_pad_set_caps (GST_PAD (stream), caps);
        }
      }
      gst_pad_set_active (GST_PAD (stream), TRUE);
      gst_element_add_pad (GST_ELEMENT (thiz), GST_PAD (
          gst_object_ref (stream)));
      topology_changed = TRUE;
    }

    GST_DEBUG_OBJECT (thiz, "Received piece %d of size %d on file %d",
        ipc_data->piece, ipc_data->size, stream->idx);

    /* read the next piece */
    if (ipc_data->piece + 1 <= stream->end_piece) {
      if (h.have_piece (ipc_data->piece + 1)) {
        GST_DEBUG_OBJECT (thiz, "Reading next piece %d, current: %d",
            ipc_data->piece + 1, stream->current_piece);
        h.read_piece (ipc_data->piece + 1);
      } else {
        int i;

        GST_DEBUG_OBJECT (thiz, "Start buffering next piece %d",
            ipc_data->piece + 1);
        /* start buffering now that the piece is not available */
        gst_bt_demux_stream_start_buffering (stream, h,
            thiz->buffer_pieces);
        update_buffering = TRUE;
      }
    }

    if (stream->pending_segment) {
      GstEvent *event;

      event = gst_event_new_new_segment (FALSE, 1.0, GST_FORMAT_BYTES,
          stream->start_byte, stream->end_byte, stream->start_byte);
      gst_pad_push_event (GST_PAD (stream), event);
      stream->pending_segment = FALSE;
    }

    GST_DEBUG_OBJECT (thiz, "Pushing buffer, size: %d, file: %d, piece: %d",
        GST_BUFFER_SIZE (buf), stream->idx, ipc_data->piece);
    /* TODO handle the return value */
    ret = gst_pad_push (GST_PAD (stream), buf);

    /* send the EOS downstream */
    if (ipc_data->piece == stream->end_piece) {
      GstEvent *eos;

      eos = gst_event_new_eos ();
      GST_DEBUG_OBJECT (thiz, "Sending EOS on file %d", stream->idx);
      gst_pad_push_event (GST_PAD (stream), eos);
      stream->finished = TRUE;
    }

    /* keep track of the current piece */
    stream->current_piece = ipc_data->piece;
    g_mutex_unlock (stream->lock);
  }

  if (topology_changed)
    gst_bt_demux_check_no_more_pads (thiz);
  if (update_buffering)
    gst_bt_demux_send_buffering (thiz, h);

  g_mutex_unlock (thiz->streams_lock);
  gst_bt_demux_buffer_data_free (ipc_data);
}

static void
gst_bt_demux_task_setup (GstBtDemux * thiz)
{
  /* to pop from the libtorrent async system */
  thiz->task = gst_task_create (gst_bt_demux_loop, thiz);
  gst_task_set_lock (thiz->task, &thiz->task_lock);
  gst_task_start (thiz->task);

  /* to push downstream */
  thiz->push_task = gst_task_create (gst_bt_demux_push_loop, thiz);
  gst_task_set_lock (thiz->push_task, &thiz->push_task_lock);
  gst_task_start (thiz->push_task);

  /* our ipc between tasks */
  thiz->ipc = g_async_queue_new_full (
      (GDestroyNotify)gst_bt_demux_buffer_data_free);
}

static void
gst_bt_demux_task_cleanup (GstBtDemux * thiz)
{
  using namespace libtorrent;
  session *s;
  std::vector<torrent_handle> torrents;

  s = (session *)thiz->session;
  torrents = s->get_torrents ();

  if (thiz->push_task) {
    GstBtDemuxBufferData *ipc_data;

    /* send a cleanup buffer */
    ipc_data = g_new0 (GstBtDemuxBufferData, 1);
    g_async_queue_push (thiz->ipc, ipc_data);

    gst_task_stop (thiz->push_task); 
    gst_task_join (thiz->push_task);
    gst_object_unref (thiz->push_task);
    thiz->push_task = NULL;
  }

  if (torrents.size () < 1) {
    /* nothing added, stop the task directly */
    thiz->finished = TRUE;
  } else {
    torrent_handle h;
    h = torrents[0];
    s->remove_torrent (h);
  }
  
  /* given that the pads are removed on the parent class at the paused
   * to ready state, we need to exit the task and wait for it
   */
  if (thiz->task) { 
    gst_task_stop (thiz->task);
    gst_task_join (thiz->task);
    gst_object_unref (thiz->task);
    thiz->task = NULL;
  }

  if (thiz->ipc) {
    g_async_queue_unref (thiz->ipc);
    thiz->ipc = NULL;
  }
}

static void
gst_bt_demux_cleanup (GstBtDemux * thiz)
{
  /* remove every pad reference */
  if (thiz->streams) {
    /* finally remove the files if we need to */
    if (thiz->temp_remove) {
      GSList *walk;

      for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
        GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
        gchar *to_remove;

        to_remove = g_build_path (G_DIR_SEPARATOR_S, thiz->temp_location,
            stream->path, NULL);
        g_remove (to_remove);
        g_free (to_remove);
      }
    }

    g_slist_free_full (thiz->streams, gst_object_unref);
    thiz->streams = NULL;
  }
}

static GstStateChangeReturn
gst_bt_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstBtDemux * thiz;
  GstStateChangeReturn ret;

  thiz = GST_BT_DEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_bt_demux_task_setup (thiz);
      break;

    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_bt_demux_task_cleanup (thiz);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_bt_demux_parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_bt_demux_cleanup (thiz);
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
  GstBtDemux *thiz;

  thiz = GST_BT_DEMUX (object);

  GST_DEBUG_OBJECT (thiz, "Disposing");

  gst_bt_demux_task_cleanup (thiz);
  gst_bt_demux_cleanup (thiz);

  if (thiz->session) {
    libtorrent::session *session;

    session = (libtorrent::session *)thiz->session;
    delete (session);
    thiz->session = NULL;
  }

  if (thiz->adapter) {
    g_object_unref (thiz->adapter);
    thiz->adapter = NULL;
  }

  g_mutex_free (thiz->streams_lock);

  g_free (thiz->temp_location);

  G_OBJECT_CLASS (gst_bt_demux_parent_class)->dispose (object);
}

static void
gst_bt_demux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBtDemux *thiz = NULL;

  g_return_if_fail (GST_IS_BT_DEMUX (object));

  thiz = GST_BT_DEMUX (object);

  switch (prop_id) {
    case PROP_SELECTOR_POLICY:
      thiz->policy = (GstBtDemuxSelectorPolicy)g_value_get_enum (value);
      break;

    case PROP_TYPEFIND:
      thiz->typefind = g_value_get_boolean (value);
      break;

    case PROP_TEMP_REMOVE:
      thiz->temp_remove = g_value_get_boolean (value);
      break;

    case PROP_TEMP_LOCATION:
      g_free (thiz->temp_location);
      thiz->temp_location = g_strdup (g_value_get_string (value));
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
  GstBtDemux *thiz = NULL;

  g_return_if_fail (GST_IS_BT_DEMUX (object));

  thiz = GST_BT_DEMUX (object);
  switch (prop_id) {
    case PROP_N_STREAMS:
      g_value_set_int (value, g_slist_length (thiz->streams));
      break;

    case PROP_SELECTOR_POLICY:
      g_value_set_enum (value, thiz->policy);
      break;

    case PROP_TYPEFIND:
      g_value_set_boolean (value, thiz->typefind);
      break;

    case PROP_TEMP_REMOVE:
      g_value_set_boolean (value, thiz->temp_remove);
      break;

    case PROP_TEMP_LOCATION:
      g_value_set_string (value, thiz->temp_location);
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
gst_bt_demux_init (GstBtDemux * thiz)
{
  using namespace libtorrent;
  GstPad *pad;
  session *s;

  pad = gst_pad_new_from_static_template (&sink_factory, "sink");
  gst_pad_set_chain_function (pad, gst_bt_demux_sink_chain);
  gst_pad_set_event_function (pad, gst_bt_demux_sink_event);

  gst_element_add_pad (GST_ELEMENT (thiz), pad);

  /* to store the buffers from upstream until we have a full torrent file */
  thiz->adapter = gst_adapter_new ();

  thiz->streams_lock = g_mutex_new ();

  /* create a new session */
  s = new session ();
  /* set the error alerts and the progress alerts */
  s->set_alert_mask (alert::error_notification | alert::progress_notification |
      alert::status_notification);
  thiz->session = s;

  g_static_rec_mutex_init (&thiz->task_lock);
  g_static_rec_mutex_init (&thiz->push_task_lock);

  /* default properties */
  thiz->policy = GST_BT_DEMUX_SELECTOR_POLICY_LARGER;
  thiz->buffer_pieces = DEFAULT_BUFFER_PIECES;
  thiz->typefind = DEFAULT_TYPEFIND;
  thiz->temp_location = g_build_path (G_DIR_SEPARATOR_S, g_get_tmp_dir (), DEFAULT_DIR,
      NULL);
  thiz->temp_remove = DEFAULT_TEMP_REMOVE;
}
