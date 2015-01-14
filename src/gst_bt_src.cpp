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
 * TODO
 * make this the source for magnet uris
 */

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

static const gchar *
gst_bt_src_uri_get_uri (GstURIHandler * handler)
{
  GstBtSrc *thiz = GST_BT_SRC (handler);

  return thiz->uri;
}

static gboolean
gst_bt_src_uri_set_uri (GstURIHandler * handler, const gchar * uri)
{
  return FALSE;
}

static void
gst_bt_src_uri_handler_init (gpointer g_iface, gpointer iface_data)
{
  GstURIHandlerInterface *iface = (GstURIHandlerInterface *) g_iface;

  iface->get_type = gst_bt_src_uri_get_type;
  iface->get_protocols = gst_bt_src_uri_get_protocols;
  iface->get_uri = gst_bt_src_uri_get_uri;
  iface->set_uri = gst_bt_src_uri_set_uri;
}

