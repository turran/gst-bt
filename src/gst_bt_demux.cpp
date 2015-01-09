#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt_demux.hpp"

#include <iterator>
#include <deque>

#include "libtorrent/session.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/time.hpp"

GST_DEBUG_CATEGORY_EXTERN (gst_bt_demux_debug);
#define GST_CAT_DEFAULT gst_bt_demux_debug

typedef struct _GstBtDemuxBufferData
{
  boost::shared_array <char> buffer;
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

static void
gst_bt_demux_stream_setup_buffering (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int piece, int max_pieces)
{
  using namespace libtorrent;
  int i;
  int end = piece + max_pieces;

  /* calculate the buffering block of this stream */
  GST_ERROR ("Start buffering");
  thiz->needed_buffering_blocks = 0;
  for (i = piece; i < thiz->end_piece && i < end; i++) {
    int piece_size;
    int block_count;

    piece_size = h.get_torrent_info ().piece_size (i);
    block_count = piece_size / h.status ().block_size;

    thiz->needed_buffering_blocks += block_count;
  }

  thiz->buffering = TRUE;
  thiz->blocks = 0;
  thiz->buffering_level = 0;
}

static void
gst_bt_demux_stream_add_piece (GstBtDemuxStream * thiz,
    libtorrent::torrent_handle h, int piece, int max_pieces)
{
  using namespace libtorrent;
  int i = thiz->downloading_pieces;

  GST_DEBUG_OBJECT (thiz, "Adding more pieces, current_downloading: %d, "
      "max: %d", i, max_pieces);
  for (piece; piece < thiz->end_piece && i < max_pieces; piece++) {
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
    thiz->downloading_pieces++;
    GST_DEBUG_OBJECT (thiz, "Requesting piece %d, prio: %d, current: %d, "
        "downloading: %d", piece, priority, thiz->current_piece,
        thiz->downloading_pieces);
    i++;
  }
}


static gboolean
gst_bt_demux_stream_event (GstPad * pad, GstEvent * event)
{
  GstBtDemux *thiz;
  gboolean ret;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Event");

  ret = TRUE;

  gst_object_unref (thiz);

  return ret;
}

static gboolean
gst_bt_demux_stream_query (GstPad * pad, GstQuery * query)
{
  GstBtDemux *thiz;
  gboolean ret;

  thiz = GST_BT_DEMUX (gst_pad_get_parent (pad));
  
  GST_DEBUG_OBJECT (thiz, "Quering");

  ret = TRUE;

  gst_object_unref (thiz);

  return ret;
}

static void
gst_bt_demux_stream_dispose (GObject * object)
{
  GstBtDemuxStream *thiz;

  thiz = GST_BT_DEMUX_STREAM (object);

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
  thiz->current_piece = -1;
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
  PROP_N_STREAMS,
  PROP_CURRENT_STREAM,
  PROP_TMP_LOCATION,
  PROP_TMP_REMOVE,
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
gst_bt_demux_activate_streams (GstBtDemux * thiz)
{
  using namespace libtorrent;
  GSList *streams = NULL;
  GSList *walk;
  session *s;
  torrent_handle h;

  if (!thiz->streams)
    return;

  if (!thiz->requested_streams) {
    /* use the policy */
    streams = gst_bt_demux_get_policy_streams (thiz);
  } else {
    /* TODO use the value */
  }

  s = (session *)thiz->session;
  h = s->get_torrents ()[0];

  /* TODO lock the streams */

  /* mark every stream as not requested */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);
    stream->requested = FALSE;
  }

  /* prioritize the first piece of every requested stream */
  for (walk = streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    stream->requested = TRUE;

    GST_DEBUG_OBJECT (thiz, "Requesting stream %s", GST_PAD_NAME (stream));

    if (h.have_piece (stream->start_piece)) {
      /* request the first non-downloaded piece */
      if (stream->start_piece != stream->end_piece) {
        gst_bt_demux_stream_add_piece (stream, h, stream->start_piece,
          thiz->buffer_pieces);
      }

      GST_DEBUG_OBJECT (thiz, "Reading piece %d, current: %d",
          stream->start_piece, stream->current_piece);
      h.read_piece (stream->start_piece);
    } else {
      gst_bt_demux_stream_add_piece (stream, h, stream->start_piece,
          thiz->buffer_pieces);
    }
  }

  /* TODO unlock the streams */

  g_slist_free_full (streams, gst_object_unref);
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
gst_bt_demux_send_buffering (GstBtDemux * thiz)
{
  GSList *walk;
  int num_buffering = 0;
  int buffering = 0;

  /* generate the real buffering level */
  for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
    GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

    if (!stream->requested)
      continue;
    if (!stream->buffering)
      continue;
    buffering += stream->buffering_level;
    /* unset the stream buffering */
    if (stream->buffering_level == 100) {
      GST_ERROR ("End buffering");
      stream->buffering = FALSE;
      stream->buffering_level = 0;
    }
    num_buffering++;
  }

  if (num_buffering) {
    gdouble level = ((gdouble) buffering) / num_buffering;
    if (thiz->buffering) {
      gst_element_post_message (GST_ELEMENT_CAST (thiz),
          gst_message_new_buffering (GST_OBJECT_CAST (thiz), level));
      if (level >= 100.0) {
        thiz->buffering = FALSE;
      }
    } else if (level < 100.0) {
      gst_element_post_message (GST_ELEMENT_CAST (thiz),
          gst_message_new_buffering (GST_OBJECT_CAST (thiz), level));
      thiz->buffering = TRUE;
    }
  }
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
            int piece_length;

            /* create the pads */
            name = g_strdup_printf ("src_%02d", i);

            /* initialize the streams */
            stream = (GstBtDemuxStream *) g_object_new (
                GST_TYPE_BT_DEMUX_STREAM, "name", name, "direction",
                GST_PAD_SRC, "template", gst_static_pad_template_get (&src_factory), NULL);
            g_free (name);

            /* get the pieces and offsets related to the file */
            piece_length = p->params.ti->piece_length ();
            fe = p->params.ti->file_at (i);
            stream->start_piece = fe.offset / piece_length;
            stream->start_offset = fe.offset % piece_length;
            stream->end_piece = (fe.offset + fe.size) /piece_length;
            stream->end_offset = (fe.offset + fe.size) % piece_length;

            GST_DEBUG_OBJECT (thiz, "Adding stream %s for file '%s', "
                " start_piece: %d, start_offset: %d, end_piece: %d, "
                "end_offset: %d", GST_PAD_NAME (stream), fe.path.c_str (),
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
        gboolean download_more = TRUE;

        GST_DEBUG_OBJECT (thiz, "Piece %d completed (down: %d kb/s, "
            "up: %d kb/s, peers: %d)", p->piece_index, s.download_rate / 1000,
            s.upload_rate  / 1000, s.num_peers);
        /* read the piece once it is finished and send downstream in order */
        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          if (p->piece_index < stream->start_piece &&
              p->piece_index > stream->end_piece)
            continue;

          if (!stream->requested)
            continue;

          /* low the priority again */
          h.piece_priority (p->piece_index, 0);

          /* decrement the number of downloading pieces */
          stream->downloading_pieces--;

          /* finish the buffering */
          if (stream->buffering) {

              /* TODO update the buffer level */
              if (!stream->downloading_pieces) {
                stream->blocks = 0;
                stream->buffering_level = 100;
                update_buffering = TRUE;

                GST_DEBUG_OBJECT (thiz, "Reading piece %d, current: %d",
                    p->piece_index, stream->current_piece);
                h.read_piece (stream->current_piece + 1);
              } else {
                /* do not download more until we reach the 100% of buffering */
                download_more = FALSE;
              }
          }

          if (download_more) {
            /* download the next pieces */
            gst_bt_demux_stream_add_piece (stream, h, p->piece_index + 1,
                thiz->buffer_pieces);
          }

          if (update_buffering)
            gst_bt_demux_send_buffering (thiz);
        }
        break;
      }
    case block_downloading_alert::alert_type:
      {
        GSList *walk;
        block_downloading_alert *p = alert_cast<block_downloading_alert>(a);
        torrent_handle h = p->handle;

        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          if (p->piece_index < stream->start_piece &&
              p->piece_index > stream->end_piece)
            continue;

          if (!stream->requested)
            continue;

          if (stream->buffering)
            continue;

          /* time to mark the stream as buffering with 0% */
          if (stream->current_piece + 1 == p->piece_index) {
            gst_bt_demux_stream_setup_buffering (stream, h, p->piece_index,
                thiz->buffer_pieces);
          }
        }
        gst_bt_demux_send_buffering (thiz);
        break;
      }
    case block_finished_alert::alert_type:
      {
        GSList *walk;
        block_finished_alert *p = alert_cast<block_finished_alert>(a);
        torrent_handle h = p->handle;

        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          if (p->piece_index < stream->start_piece &&
              p->piece_index > stream->end_piece)
            continue;

          if (!stream->requested)
            continue;

          if (!stream->buffering)
            continue;

          /* in case this block belongs to the pieces we are waiting for,
           * update the buffering level
           */
          if (p->piece_index > stream->current_piece + 1 && p->piece_index <
              stream->current_piece + thiz->buffer_pieces) {

            stream->blocks++;
            stream->buffering_level = (stream->blocks * 100) / stream->needed_buffering_blocks;
          }
        }
        gst_bt_demux_send_buffering (thiz);
        break;
      }

    case read_piece_alert::alert_type:
      {
        GstFlowReturn ret;
        GstBuffer *buf;
        GstBtDemuxBufferData *buf_data;
        GSList *walk;
        read_piece_alert *p = alert_cast<read_piece_alert>(a);
        torrent_handle h = p->handle;
        gboolean topology_changed = FALSE;
        gboolean update_buffering = FALSE;
        int i;

        /* TODO send the new segment */
        /* send the data downstream */
        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          if (p->piece < stream->start_piece && p->piece > stream->end_piece)
            continue;

          /* in case the pad is active but not requested, disable it */
          if (gst_pad_is_active (GST_PAD (stream)) && !stream->requested) {
            topology_changed = TRUE;
            gst_pad_set_active (GST_PAD (stream), FALSE);
            gst_element_remove_pad (GST_ELEMENT (thiz), GST_PAD (stream));
            continue;
          }

          if (!stream->requested)
            continue;

          /* create the pad if needed */
          if (!gst_pad_is_active (GST_PAD (stream))) {
            gst_pad_set_active (GST_PAD (stream), TRUE);
            gst_element_add_pad (GST_ELEMENT (thiz), GST_PAD (
                gst_object_ref (stream)));
            topology_changed = TRUE;
          }

          GST_DEBUG_OBJECT (thiz, "Received piece %d of size %d on file %d",
              p->piece, p->size, i);

          /* read the next piece */
          if (p->piece + 1 <= stream->end_piece) {
            if (h.have_piece (p->piece + 1)) {
              GST_DEBUG_OBJECT (thiz, "Reading piece %d, current: %d",
                  p->piece + 1, stream->current_piece);
              h.read_piece (p->piece + 1);
            } else {
              int i;

              /* calculate the buffering block of this stream */
              gst_bt_demux_stream_setup_buffering (stream, h, p->piece + 1,
                  thiz->buffer_pieces);
              update_buffering = TRUE;
            }
          }

          buf = gst_buffer_new ();
          GST_BUFFER_DATA (buf) = (guint8 *)p->buffer.get ();

          buf_data = g_new0 (GstBtDemuxBufferData, 1);
          buf_data->buffer = p->buffer;
          GST_BUFFER_MALLOCDATA (buf) = (guint8 *)buf_data;
          GST_BUFFER_FREE_FUNC (buf) = gst_bt_demux_buffer_data_free;

          GST_BUFFER_SIZE (buf) = p->size;

          /* handle the offsets */
          if (p->piece == stream->start_piece) {
            GST_BUFFER_DATA (buf) = GST_BUFFER_DATA (buf) + stream->start_offset;
            GST_BUFFER_SIZE (buf) -= stream->start_offset;
          }

          if (p->piece == stream->end_piece) {
            GST_BUFFER_SIZE (buf) -= GST_BUFFER_SIZE (buf) - stream->end_offset;
          }


          GST_DEBUG_OBJECT (thiz, "Pushing buffer of size %d on file %d",
              GST_BUFFER_SIZE (buf), i);
          ret = gst_pad_push (GST_PAD (stream), buf);

          /* send the EOS downstream */
          if (p->piece == stream->end_piece) {
            GstEvent *eos;

            eos = gst_event_new_eos ();
            GST_DEBUG_OBJECT (thiz, "Sending EOS on file %d", i);
            gst_pad_push_event (GST_PAD (stream), eos);
            stream->finished = TRUE;
          }

          /* keep track of the current piece */
          stream->current_piece = p->piece;
        }

        if (topology_changed)
          gst_bt_demux_check_no_more_pads (thiz);
        if (update_buffering)
          gst_bt_demux_send_buffering (thiz);
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
}

static void
gst_bt_demux_task_setup (GstBtDemux * thiz)
{
   /* to pop from the libtorrent async system */
  thiz->task = gst_task_create (gst_bt_demux_loop, thiz);
  gst_task_set_lock (thiz->task, &thiz->task_lock);
  gst_task_start (thiz->task);
}

static void
gst_bt_demux_task_cleanup (GstBtDemux * thiz)
{
  using namespace libtorrent;
  session *s;
  std::vector<torrent_handle> torrents;

  s = (session *)thiz->session;
  torrents = s->get_torrents ();

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
}

static void
gst_bt_demux_cleanup (GstBtDemux * thiz)
{
  /* remove every pad reference */
  if (thiz->streams) {
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

  /* create a new session */
  s = new session ();
  /* set the error alerts and the progress alerts */
  s->set_alert_mask (alert::error_notification | alert::progress_notification |
      alert::status_notification);
  thiz->session = s;

  g_static_rec_mutex_init (&thiz->task_lock);

  /* default properties */
  thiz->policy = GST_BT_DEMUX_SELECTOR_POLICY_LARGER;
  thiz->buffer_pieces = 3;
}
