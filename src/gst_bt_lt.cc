#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gst_bt_lt.hh"
#include "libtorrent/session.hpp"

struct _GstBtLt
{
  libtorrent::session *session;
};

GstBtLt * gst_bt_lt_new (void)
{
  GstBtLt *thiz;

  thiz = g_new0 (GstBtLt, 1);

  /* create a new session */
  /* set the options, like the tmp path */
  /* add the passing in torrent buffer */

  return thiz;
}
