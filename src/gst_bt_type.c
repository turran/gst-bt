#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <gst/gsttypefind.h>

#include "gst_bt_type.h"

static GstStaticCaps gst_bt_type_caps = GST_STATIC_CAPS ("application/x-bittorrent");
#define GST_BT_CAPS (gst_static_caps_get(&gst_bt_type_caps))
#define GST_BT_MAGIC "d8:announce"


static void
gst_bt_type_find (GstTypeFind * tf, gpointer unused)
{
  guint64 offset = 0;
  const guint8 *data;

  data = gst_type_find_peek (tf, 0, strlen (GST_BT_MAGIC));
  if (data) {
    if (!strncmp (data, GST_BT_MAGIC, strlen (GST_BT_MAGIC)))
      gst_type_find_suggest (tf, GST_TYPE_FIND_MAXIMUM, GST_BT_CAPS);
  }
  return;
}

gboolean
gst_bt_type_init (GstPlugin * plugin)
{
  /* register the mpd type find */
  if (!gst_type_find_register (plugin, "application/x-bittorrent",
      GST_RANK_PRIMARY, gst_bt_type_find, "torrent", GST_BT_CAPS,
      NULL, NULL))
    return FALSE;

  return TRUE;
}

