#pragma once

/* GStreamer
 * Copyright (C) 2025 Yasushi SHOJI <yashi@spacecubics.com>
 *
 * gstuartsink.h:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS

#define GST_TYPE_UART_SINK gst_uart_sink_get_type ()

G_DECLARE_DERIVABLE_TYPE (GstUartSink, gst_uart_sink, GST, UART_SINK, GstBaseSink)

struct _GstUartSinkClass {
	GstBaseSinkClass parent_class;
};

G_END_DECLS
