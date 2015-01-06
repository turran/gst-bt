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

static gboolean
gst_bt_demux_stream_event (GstPad * pad, GstEvent * event)
{
  GstBtDemuxStream *thiz;
  gboolean ret;

  thiz = GST_BT_DEMUX_STREAM (gst_pad_get_parent (pad));

  GST_DEBUG_OBJECT (thiz, "Event");

  ret = TRUE;

  gst_object_unref (thiz);

  return ret;
}

static gboolean
gst_bt_demux_stream_query (GstPad * pad, GstQuery * query)
{
  GstBtDemuxStream *thiz;
  gboolean ret;

  thiz = GST_BT_DEMUX_STREAM (gst_pad_get_parent (pad));
  
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
    /* TODO use the segment of every pad instead of the first piece */
    if (h.piece_priority (stream->start_piece) != 7) {
      GST_DEBUG_OBJECT (thiz, "Requesting piece %d", stream->start_piece);
      h.piece_priority (stream->start_piece, 7);
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

/* thread reading messages from libtorrent */
static void
gst_bt_demux_handle_alert (GstBtDemux * thiz, libtorrent::alert * a)
{
  using namespace libtorrent;

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

          /* time to activate the streams */
          gst_bt_demux_activate_streams (thiz);
        }
        break;
      }

    case piece_finished_alert::alert_type:
      {
         piece_finished_alert *p = alert_cast<piece_finished_alert>(a);
         torrent_handle h = p->handle;
         torrent_status s = h.status();

         GST_DEBUG_OBJECT (thiz, "Piece %d completed (down: %d kb/s, "
             "up: %d kb/s, peers: %d)", p->piece_index, s.download_rate / 1000,
             s.upload_rate  / 1000, s.num_peers);
         /* read the piece once it is finished and send downstream in order */
         h.read_piece (p->piece_index);
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
        int i;

        /* TODO send the new segment */
        /* send the data downstream */
        for (walk = thiz->streams; walk; walk = g_slist_next (walk)) {
          GstBtDemuxStream *stream = GST_BT_DEMUX_STREAM (walk->data);

          if (p->piece < stream->start_piece && p->piece > stream->end_piece)
            continue;

          /* in case the pad is active but not requested, disable it */
          if (gst_pad_is_active (GST_PAD (stream)) && !stream->requested) {
            gst_element_remove_pad (GST_ELEMENT (thiz), GST_PAD (stream));
            continue;
          }

          if (!stream->requested)
            continue;

          /* check that we are being sequential */
          if (stream->current_piece + 1 != p->piece)
            continue;

          /* prioritize the next piece */
          if (p->piece + 1 <= stream->end_piece &&
              h.piece_priority (p->piece + 1) != 7) {
              GST_DEBUG_OBJECT (thiz, "Requesting piece %d", p->piece + 1);
              h.piece_priority (p->piece + 1, 7);
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

          GST_DEBUG_OBJECT (thiz, "Received piece %d of size %d, "
              "pushing buffer of size %d on file %d", p->piece, p->size,
              GST_BUFFER_SIZE (buf), i);

          gst_pad_set_active (GST_PAD (stream), TRUE);
          gst_element_add_pad (GST_ELEMENT (thiz), GST_PAD (stream));

          ret = gst_pad_push (GST_PAD (stream), buf);

          /* send the EOS downstream */
          if (p->piece == stream->end_piece) {
            GstEvent *eos;

            eos = gst_event_new_eos ();
            GST_DEBUG_OBJECT (thiz, "Sending EOS on file %d", i);
            gst_pad_push_event (GST_PAD (stream), eos);
            stream->finished = TRUE;
          }

          stream->current_piece = p->piece;
        }
        gst_bt_demux_check_no_more_pads (thiz);
      }

    case file_completed_alert::alert_type:
      /* TODO send the EOS downstream */
      /* TODO mark ourselves as done */
      /* TODO quit the main demux loop */
      break;
    default:
      break;
  }
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

        gst_bt_demux_handle_alert (thiz, *i);
        delete *i;
      }
      alerts.clear();
    }
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
      gst_task_start (thiz->task);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_bt_demux_parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      /* TODO stop downloading */
      thiz->finished = TRUE;
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      /* TODO destroy the loop */
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

  /* TODO move this to the cleanup() */
  if (thiz->streams) {
    g_slist_free_full (thiz->streams, gst_object_unref);
    thiz->streams = NULL;
  }

  if (thiz->session) {
    libtorrent::session *session;

    session = (libtorrent::session *)thiz->session;
    delete (session);
    thiz->session = NULL;
  }

  if (thiz->task) { 
    gst_task_stop (thiz->task); 
    gst_task_join (thiz->task);
    gst_object_unref (thiz->task);
    thiz->task = NULL;
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

  /* to pop from the libtorrent async system */
  g_static_rec_mutex_init (&thiz->task_lock);
  thiz->task = gst_task_create (gst_bt_demux_loop, thiz);
  gst_task_set_lock (thiz->task, &thiz->task_lock);

  /* default properties */
  thiz->policy = GST_BT_DEMUX_SELECTOR_POLICY_LARGER;
}
