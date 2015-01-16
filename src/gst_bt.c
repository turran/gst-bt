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

#include <gst/gst.h>
#include "gst_bt_src.hpp"
#include "gst_bt_demux.hpp"
#include "gst_bt_type.h"

#if HAVE_GST_1
#define PLUGIN_NAME bt
#else
#define PLUGIN_NAME "bt"
#endif

GST_DEBUG_CATEGORY (gst_bt_demux_debug);
GST_DEBUG_CATEGORY (gst_bt_src_debug);

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* first register the debug categories */
  GST_DEBUG_CATEGORY_INIT (gst_bt_demux_debug, "btdemux", 0, "BitTorrent demuxer");
  GST_DEBUG_CATEGORY_INIT (gst_bt_src_debug, "btsrc", 0, "BitTorrent source");

  if (!gst_element_register (plugin, "btdemux",
          GST_RANK_PRIMARY + 1, GST_TYPE_BT_DEMUX))
    return FALSE;

  if (!gst_element_register (plugin, "btsrc",
          GST_RANK_PRIMARY + 1, GST_TYPE_BT_SRC))
    return FALSE;

  gst_bt_type_init (plugin);

  return TRUE;
}

/* this is the structure that gstreamer looks for to register plugins
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    PLUGIN_NAME, "BitTorrent Plugin",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME,
    "http://www.turran.org");

