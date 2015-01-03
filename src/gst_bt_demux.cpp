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

#define gst_bt_demux_parent_class parent_class
G_DEFINE_TYPE (GstBtDemux, gst_bt_demux, GST_TYPE_ELEMENT);

static GstStaticPadTemplate sink_factory =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-bittorrent"));

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
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
          torrent_handle h = p->handle;
          int i;

          GST_INFO_OBJECT (thiz, "Start downloading");
          GST_DEBUG_OBJECT (thiz, "num pieces: %d, piece length: %d",
              p->params.ti->num_pieces (), p->params.ti->piece_length ());

          /* prioritize the first piece */
          h.piece_priority (0, 7);
          /* mark every piece to none-priority */
          for (i = 1; i < p->params.ti->num_pieces (); i++) {
            h.piece_priority (i, 0);
          }
          /* make sure to download sequentially */
          h.set_sequential_download (true);
        }
        break;
      }

    case piece_finished_alert::alert_type:
     {
        piece_finished_alert *p = alert_cast<piece_finished_alert>(a);
        torrent_handle h = p->handle;

        GST_DEBUG_OBJECT (thiz, "Piece %d completed", p->piece_index);
        /* read the piece once it is finished and send downstream in order */
        h.read_piece (p->piece_index);
        break;
     }

    case read_piece_alert::alert_type:
      {
        read_piece_alert *p = alert_cast<read_piece_alert>(a);
        torrent_handle h = p->handle;

        /* TODO send the new segment */
        /* TODO send the data downstream */
        /* prioritize the next piece */
        if (p->piece + 1 < h.get_torrent_info ().num_pieces ()) {
          GST_DEBUG_OBJECT (thiz, "Requesting piece %d", p->piece + 1);
          h.piece_priority (p->piece + 1, 7);
        }
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
  while (true) {
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

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
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

  GST_CALL_PARENT (G_OBJECT_CLASS, dispose, (object));
}

static void
gst_bt_demux_class_init (GstBtDemuxClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  /* initialize the object class */
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_bt_demux_dispose);

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
}
