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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt.h"
#include "gst_bt_src.hpp"

#include "libtorrent/session.hpp"
#include "libtorrent/magnet_uri.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/alert_types.hpp"
#include "libtorrent/create_torrent.hpp"

GST_DEBUG_CATEGORY_EXTERN (gst_bt_src_debug);
#define GST_CAT_DEFAULT gst_bt_src_debug

/* Forward declarations */
static void
gst_bt_src_set_uri (GstBtSrc * thiz, const gchar * uri);
/*----------------------------------------------------------------------------*
 *                            The URI interface                               *
 *----------------------------------------------------------------------------*/
#if HAVE_GST_1
static GstURIType
gst_bt_src_uri_get_type (GType type)
{
  return GST_URI_SRC;
}

static const gchar * const *
gst_bt_src_uri_get_protocols (GType type)
{
  static const gchar *protocols[] = { (char *) "magnet", NULL };

  return protocols;
}

static gboolean
gst_bt_src_uri_set_uri (GstURIHandler * handler, const gchar * uri, GError ** err)
{
  GstBtSrc *thiz = GST_BT_SRC (handler);
  gst_bt_src_set_uri (thiz, uri);
  return TRUE;
}

static gchar *
gst_bt_src_uri_get_uri (GstURIHandler * handler)
{
  GstBtSrc *thiz = GST_BT_SRC (handler);

  return g_strdup (thiz->uri);
}

#else

static GstURIType
gst_bt_src_uri_get_type (void)
{
  return GST_URI_SRC;
}

static gchar **
gst_bt_src_uri_get_protocols (void)
{
  static gchar *protocols[] = { (char *) "magnet", NULL };

  return protocols;
}

static gboolean
gst_bt_src_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  GstBtSrc *thiz = GST_BT_SRC (handler);
  gst_bt_src_set_uri (thiz, uri);
  return TRUE;
}

static const gchar *
gst_bt_src_uri_get_uri (GstURIHandler * handler)
{
  GstBtSrc *thiz = GST_BT_SRC (handler);

  return thiz->uri;
}

#endif

static void
gst_bt_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_bt_src_uri_get_type;
  iface->get_protocols = gst_bt_src_uri_get_protocols;
  iface->get_uri = gst_bt_src_uri_get_uri;
  iface->set_uri = gst_bt_src_uri_set_uri;
}

/*----------------------------------------------------------------------------*
 *                              The src class                                 *
 *----------------------------------------------------------------------------*/
enum {
  PROP_0,
  PROP_URI,
};

#if HAVE_GST_1

G_DEFINE_TYPE_WITH_CODE (GstBtSrc, gst_bt_src, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_bt_src_uri_handler_init));
#else

static gboolean
gst_bt_src_interface_supported (GstImplementsInterface * iface, GType type)
{
  if (type == GST_TYPE_URI_HANDLER)
    return TRUE;
  else
    return FALSE;
}

static void
gst_bt_src_interface_init (GstImplementsInterfaceClass * klass)
{
  klass->supported = gst_bt_src_interface_supported;
}

G_DEFINE_TYPE_WITH_CODE (GstBtSrc, gst_bt_src, GST_TYPE_ELEMENT,
    G_IMPLEMENT_INTERFACE (GST_TYPE_IMPLEMENTS_INTERFACE,
        gst_bt_src_interface_init);
    G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER,
        gst_bt_src_uri_handler_init));

#endif

static GstStaticPadTemplate src_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-bittorrent"));

static void
gst_bt_src_set_uri (GstBtSrc * thiz, const gchar * uri)
{
  if (thiz->uri) {
    g_free (thiz->uri);
    thiz->uri = NULL;
  }

  thiz->uri = g_strdup (uri);
}

/* thread reading messages from libtorrent */
static gboolean
gst_bt_src_handle_alert (GstBtSrc * thiz, libtorrent::alert * a)
{
  using namespace libtorrent;
  gboolean ret = FALSE;

  GST_LOG_OBJECT (thiz, "Received alert '%s'", a->what());

  switch (a->type()) {
    case add_torrent_alert::alert_type:
      {
        add_torrent_alert *p = alert_cast<add_torrent_alert>(a);

        if (p->error) {
          GST_ELEMENT_ERROR (thiz, STREAM, FAILED,
              ("Error while adding the torrent."),
              ("libtorrent says %s", p->error.message ().c_str ()));
          ret = TRUE;
        }
        break;
      }

    case torrent_removed_alert::alert_type:
      /* a safe cleanup, the torrent has been removed */
      ret = TRUE;
      break;

    case metadata_received_alert::alert_type:
      {
        GstFlowReturn flow;
        metadata_received_alert *p = alert_cast<metadata_received_alert>(a);
        session *s;
        torrent_handle h = p->handle;
        torrent_info ti = h.get_torrent_info ();
        std::vector<char> buffer;
        GstBuffer *buf;
        GstPad *pad;
        guint8 *data;
#if HAVE_GST_1
        GstMapInfo mi;
#endif

        s = (session *)thiz->session;
        h.pause ();
        s->remove_torrent (h);

        create_torrent ct(ti);
        entry te = ct.generate();
        bencode(std::back_inserter(buffer), te);

        buf = gst_buffer_new_and_alloc (buffer.size());
#if HAVE_GST_1
        gst_buffer_map (buf, &mi, GST_MAP_WRITE);
        data = mi.data;
#else
        data = GST_BUFFER_DATA (buf);
#endif

        memcpy (data, &buffer[0], buffer.size());

#if HAVE_GST_1
        gst_buffer_unmap (buf, &mi);
#endif

        pad = gst_element_get_static_pad (GST_ELEMENT (thiz), "src");
        GST_DEBUG_OBJECT (thiz, "Pushing torrent info downstrean");
        flow = gst_pad_push (pad, buf);
        if (flow != GST_FLOW_OK) {
          if (flow == GST_FLOW_NOT_LINKED || flow <= GST_FLOW_UNEXPECTED) {
            GST_ELEMENT_ERROR (thiz, STREAM, FAILED,
                ("Internal data flow error."),
                ("streaming task paused, reason %s (%d)",
                gst_flow_get_name (flow), flow));
          }
          ret = FALSE;
        }
        gst_pad_push_event (pad, gst_event_new_eos ());
        gst_object_unref (pad);
      }
      break;

    default:
      break;
  }

  return ret;
}

static void
gst_bt_src_loop (gpointer user_data)
{
  using namespace libtorrent;
  GstBtSrc *thiz;

  thiz = GST_BT_SRC (user_data);
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
          thiz->finished = gst_bt_src_handle_alert (thiz, *i);
        delete *i;
      }
      alerts.clear();
    }
  }
  gst_task_stop (thiz->task);
}

static void
gst_bt_src_task_cleanup (GstBtSrc * thiz)
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
gst_bt_src_task_setup (GstBtSrc * thiz)
{
  /* to pop from the libtorrent async system */
#if HAVE_GST_1
  thiz->task = gst_task_new (gst_bt_src_loop, thiz, NULL);
#else
  thiz->task = gst_task_create (gst_bt_src_loop, thiz);
#endif
  gst_task_set_lock (thiz->task, &thiz->task_lock);
  gst_task_start (thiz->task);
}

static gboolean
gst_bt_src_setup (GstBtSrc * thiz)
{
  using namespace libtorrent;
  session *session;
  add_torrent_params tp;
  error_code ec;

  if (!thiz->uri)
    return FALSE;

  gst_bt_src_task_setup (thiz);

  /* set the magnet */
  session = (libtorrent::session *)thiz->session;
  parse_magnet_uri (thiz->uri, tp, ec);
  session->async_add_torrent (tp);
  /* TODO check the error */
  return FALSE;
}

static void
gst_bt_src_cleanup (GstBtSrc * thiz)
{
  gst_bt_src_task_cleanup (thiz);
}

static GstStateChangeReturn
gst_bt_src_change_state (GstElement * element, GstStateChange transition)
{
  GstBtSrc * thiz;
  GstStateChangeReturn ret;

  thiz = GST_BT_SRC (element);

  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_bt_src_setup (thiz);
      break;

    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_bt_src_parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_bt_src_cleanup (thiz);
      break;

    case GST_STATE_CHANGE_READY_TO_NULL:
      break;

    default:
      break;
  }

  return ret;
}

static void
gst_bt_src_dispose (GObject * object)
{
  GstBtSrc *thiz;

  thiz = GST_BT_SRC (object);

  GST_DEBUG_OBJECT (thiz, "Disposing");

  gst_bt_src_task_cleanup (thiz);
  gst_bt_src_cleanup (thiz);

  if (thiz->session) {
    libtorrent::session *session;

    session = (libtorrent::session *)thiz->session;
    delete (session);
    thiz->session = NULL;
  }

  g_free (thiz->uri);

  G_OBJECT_CLASS (gst_bt_src_parent_class)->dispose (object);
}

static void
gst_bt_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstBtSrc *thiz = NULL;

  g_return_if_fail (GST_IS_BT_SRC (object));

  thiz = GST_BT_SRC (object);

  switch (prop_id) {
    case PROP_URI:
      gst_bt_src_set_uri (thiz, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_bt_src_get_property (GObject * object, guint prop_id, GValue * value,
    GParamSpec * pspec)
{
  GstBtSrc *thiz = NULL;

  g_return_if_fail (GST_IS_BT_SRC (object));

  thiz = GST_BT_SRC (object);
  switch (prop_id) {
    case PROP_URI:
      g_value_set_string (value, thiz->uri);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_bt_src_class_init (GstBtSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = (GstElementClass *) klass;

  gst_bt_src_parent_class = g_type_class_peek_parent (klass);

  /* initialize the object class */
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_bt_src_dispose);
  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_bt_src_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_bt_src_get_property);
  g_object_class_install_property (gobject_class, PROP_URI,
      g_param_spec_string ("uri", "Magnet file URI",
          "URI of the magnet file", NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  /* initialize the element class */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_factory));

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_bt_src_change_state);

  gst_element_class_set_details_simple (element_class,
      "BitTorrent Src", "Source/Magnet",
      "Streams a BitTorrent file",
      "Jorge Luis Zapata <jorgeluis.zapata@gmail.com>");
}

static void
gst_bt_src_init (GstBtSrc * thiz)
{
  using namespace libtorrent;
  GstPad *pad;
  session *s;

  pad = gst_pad_new_from_static_template (&src_factory, "src");
  gst_element_add_pad (GST_ELEMENT (thiz), pad);

  /* create a new session */
  s = new session ();
  /* set the error alerts and the progress alerts */
  s->set_alert_mask (alert::error_notification | alert::progress_notification |
      alert::status_notification);
  thiz->session = s;

#if HAVE_GST_1
  g_rec_mutex_init (&thiz->task_lock);
#else
  g_static_rec_mutex_init (&thiz->task_lock);
#endif
}
