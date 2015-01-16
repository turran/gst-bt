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

#ifndef GST_BT_SRC_H
#define GST_BT_SRC_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_BT_SRC            (gst_bt_src_get_type())
#define GST_BT_SRC(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),\
                                         GST_TYPE_BT_SRC, GstBtSrc))
#define GST_BT_SRC_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),\
                                         GST_TYPE_BT_SRC, GstBtSrcClass))
#define GST_BT_SRC_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),\
                                         GST_TYPE_BT_SRC, GstBtSrcClass))
#define GST_IS_BT_SRC(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),\
                                         GST_TYPE_BT_SRC))
#define GST_IS_BT_SRC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),\
                                         GST_TYPE_BT_SRC))
typedef struct _GstBtSrc
{
  GstElement parent;
  gpointer session;
  gchar *uri;

  gboolean finished;

  GstTask *task;
#if HAVE_GST_1
  GRecMutex task_lock;
#else
  GStaticRecMutex task_lock;
#endif
} GstBtSrc;

typedef struct _GstBtSrcClass
{
  GstElementClass parent_class;
} GstBtSrcClass;

GType gst_bt_src_get_type (void);

G_END_DECLS

#endif

