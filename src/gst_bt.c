#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include "gst_bt_demux.h"

GST_DEBUG_CATEGORY (gst_bt_demux_debug);

static gboolean
plugin_init (GstPlugin * plugin)
{
  /* first register the debug categories */
  GST_DEBUG_CATEGORY_INIT (gst_bt_demux_debug, "btdemux", 0, "BitTorrent demuxer");

  if (!gst_element_register (plugin, "btdemux",
          GST_RANK_PRIMARY + 1, GST_TYPE_BT_DEMUX))
    return FALSE;

  return TRUE;
}

/* this is the structure that gstreamer looks for to register plugins
 */
GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR,
    "bt", "BitTorrent Plugin",
    plugin_init, VERSION, GST_LICENSE_UNKNOWN, PACKAGE_NAME,
    "http://www.turran.org");

