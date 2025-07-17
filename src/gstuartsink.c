/* GStreamer
 * Copyright (C) 2025 Yasushi SHOJI <yashi@spacecubics.com>
 *
 * gstuartsink.c:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include <glib-object.h>

#include "config.h"
#include "gstuartsink.h"
#include "uart.h"
#include "bitswap.h"

#define ACKNAK_DEFAULT_WAIT_TIME (100) /* 100 us */

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE("sink",
								   GST_PAD_SINK,
								   GST_PAD_ALWAYS,
								   GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC(gst_uart_sink_debug);
#define GST_CAT_DEFAULT gst_uart_sink_debug

enum {
	ARG_0,
	ARG_DEVICE,
	ARG_BAUD_RATE,
	ARG_PARITY,
	ARG_BITSWAP,
	ARG_ACKNAK,
	ARG_ACKNAK_WAIT,
};

struct _GstUartSinkPrivate {
	char *device;
	int baud_rate;
	enum UartParity parity;
	gboolean bitswap;
	gboolean acknak;
	guint32 acknak_wait;
	struct uart *uart;
	GstPoll *fdset_write;
	GstPoll *fdset_read;
	guint64 bytes_written;
	guint64 current_pos;
};

typedef struct _GstUartSinkPrivate GstUartSinkPrivate;

#define _do_init							\
	GST_DEBUG_CATEGORY_INIT (gst_uart_sink_debug, "uartsink", GST_DEBUG_FG_YELLOW | GST_DEBUG_BOLD, "uartsink element"); \
	G_ADD_PRIVATE(GstUartSink);

G_DEFINE_TYPE_WITH_CODE(GstUartSink, gst_uart_sink, GST_TYPE_BASE_SINK, _do_init);

static void gst_uart_sink_set_property(GObject * object, guint prop_id,
				       const GValue * value, GParamSpec * pspec);
static void gst_uart_sink_get_property(GObject * object, guint prop_id, GValue * value,
				       GParamSpec * pspec);

static void gst_uart_sink_dispose(GObject * obj);

static gboolean gst_uart_sink_query(GstBaseSink * basesink, GstQuery * query);
static GstFlowReturn gst_uart_sink_render(GstBaseSink * sink, GstBuffer * buffer);
static gboolean gst_uart_sink_start(GstBaseSink * basesink);
static gboolean gst_uart_sink_stop(GstBaseSink * basesink);
static gboolean gst_uart_sink_unlock(GstBaseSink * basesink);
static gboolean gst_uart_sink_unlock_stop(GstBaseSink * basesink);
static gboolean gst_uart_sink_event(GstBaseSink * sink, GstEvent * event);

static void
gst_uart_sink_class_init(GstUartSinkClass * klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;
	GstBaseSinkClass *gstbasesink_class;

	gobject_class = G_OBJECT_CLASS(klass);
	gstelement_class = GST_ELEMENT_CLASS(klass);
	gstbasesink_class = GST_BASE_SINK_CLASS(klass);

	gobject_class->set_property = gst_uart_sink_set_property;
	gobject_class->get_property = gst_uart_sink_get_property;
	gobject_class->dispose = gst_uart_sink_dispose;

	gst_element_class_set_static_metadata(gstelement_class, "UART Sink", "Sink/UART",
					      "Write data to a uart / tty",
					      "Yasushi SHOJI <yashi@spacecubics.com>");
	gst_element_class_add_static_pad_template(gstelement_class, &sinktemplate);

	gstbasesink_class->render = GST_DEBUG_FUNCPTR(gst_uart_sink_render);
	gstbasesink_class->start = GST_DEBUG_FUNCPTR(gst_uart_sink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR(gst_uart_sink_stop);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR(gst_uart_sink_unlock);
	gstbasesink_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_uart_sink_unlock_stop);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR(gst_uart_sink_event);
	gstbasesink_class->query = GST_DEBUG_FUNCPTR(gst_uart_sink_query);

	g_object_class_install_property(gobject_class, ARG_DEVICE,
					g_param_spec_string("device", "Device",
							    "UART / tty device to write to",
							    "ttyS0",
							    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class, ARG_BAUD_RATE,
					g_param_spec_int("baud-rate", "Baud rate",
							 "baud rate for the device",
							 50, 460800, 115200,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class, ARG_PARITY,
					g_param_spec_string("parity", "Parity",
							    "Parity checking for the device",
							    "no",
							    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class, ARG_BITSWAP,
					g_param_spec_boolean("bitswap", "Bit Swap",
							     "Swap bits in a byte",
							     FALSE,
							     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class, ARG_ACKNAK,
					g_param_spec_boolean("acknak", "Acknowledgement",
							     "Enable acknowledgement arbitration",
							     FALSE,
							     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(gobject_class, ARG_ACKNAK_WAIT,
					g_param_spec_uint("acknak-wait", "Ack/Nak Wait Time (usec)",
							  "Wait time for Ack / Nak in micro sec",
							  0, 1000000, ACKNAK_DEFAULT_WAIT_TIME,
							  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_uart_sink_init(GstUartSink * uartsink)
{
	GstUartSinkPrivate *priv = gst_uart_sink_get_instance_private(uartsink);
	priv->device = NULL;
	priv->baud_rate = 115200;
	priv->parity = UART_PARITY_NO;
	priv->bitswap = FALSE;
	priv->acknak = FALSE;
	priv->acknak_wait = ACKNAK_DEFAULT_WAIT_TIME;
	priv->uart = NULL;
	priv->fdset_write = NULL;
	priv->bytes_written = 0;
	priv->current_pos = 0;
	priv->fdset_read = NULL;

	gst_base_sink_set_sync(GST_BASE_SINK(uartsink), FALSE);
}

static void
gst_uart_sink_dispose(GObject * obj)
{
	GstUartSinkPrivate *priv = gst_uart_sink_get_instance_private(GST_UART_SINK(obj));
	GST_DEBUG("priv->device: %s (@%p)", priv->device, priv->device);
	if (priv->device) {
		g_free(priv->device);
		priv->device = NULL;
	}

	G_OBJECT_CLASS(gst_uart_sink_parent_class)->dispose(obj);
}

static gboolean
gst_uart_sink_query(GstBaseSink * basesink, GstQuery * query)
{
	gboolean res = FALSE;
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;

	uartsink = GST_UART_SINK(basesink);
	priv = gst_uart_sink_get_instance_private(uartsink);

	switch (GST_QUERY_TYPE(query)) {
	case GST_QUERY_POSITION:
	{
		GstFormat fmt;

		gst_query_parse_position(query, &fmt, NULL);

		switch (fmt) {
		case GST_FORMAT_DEFAULT:
		case GST_FORMAT_BYTES:
			gst_query_set_position(query, GST_FORMAT_BYTES,
					       priv->current_pos);
			res = TRUE;
			break;
		default:
			break;
		}
		break;
	}
	case GST_QUERY_FORMATS:
		gst_query_set_formats(query, 2, GST_FORMAT_DEFAULT, GST_FORMAT_BYTES);
		res = TRUE;
		break;

	case GST_QUERY_SEEKING:
	{
		GstFormat fmt;

		/* no seek support */
		gst_query_parse_seeking(query, &fmt, NULL, NULL, NULL);
		gst_query_set_seeking(query, fmt, FALSE, 0, -1);
		res = TRUE;
		break;
	}
	default:
		res =
		    GST_BASE_SINK_CLASS(gst_uart_sink_parent_class)->query(basesink, query);
		break;
	}
	return res;
}

static GstFlowReturn
gst_uart_sink_render(GstBaseSink * basesink, GstBuffer * buffer)
{
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;
	GstFlowReturn flow = GST_FLOW_OK;
	GstMapInfo info;
	gssize written;
	GstPollFD fd = GST_POLL_FD_INIT;
	gint ret;

	GST_DEBUG_OBJECT(basesink, "buffer size=%" G_GSIZE_FORMAT,
			 gst_buffer_get_size(buffer));

	uartsink = GST_UART_SINK(basesink);
	priv = gst_uart_sink_get_instance_private(uartsink);

	gst_buffer_map(buffer, &info, GST_MAP_READ);
	if (priv->bitswap)
		bitswap(info.data, info.size);
	written = write(priv->uart->fd, info.data, info.size);
	GST_DEBUG_OBJECT(basesink, "%" G_GSSIZE_FORMAT " bytes written", written);
	uart_flush(priv->uart);
	GST_DEBUG_OBJECT(basesink, "and flushed");
	if (priv->acknak) {
		GST_DEBUG_OBJECT(uartsink, "gst_poll_wait() for %d usec", priv->acknak_wait);
		ret = gst_poll_wait(priv->fdset_read, priv->acknak_wait * 1000);
		GST_DEBUG_OBJECT(uartsink, "gst_poll_wait() returned %d", ret);
		if (ret < 0)
			return GST_FLOW_FLUSHING;
		if (ret == 0) {
			GST_DEBUG_OBJECT(uartsink, "ack/nak timeout; resending");
			goto resend;
		}

		fd.fd = priv->uart->fd;
		if (gst_poll_fd_can_read(priv->fdset_read, &fd)) {
			guint8 acknak;
			gssize red;
			red = read(priv->uart->fd, &acknak, 1);
			if (red < 0) {
				GST_ERROR_OBJECT(uartsink, "read error %" G_GSIZE_FORMAT, red);
				flow = GST_FLOW_ERROR;
				goto done;
			}

			switch (acknak) {
			case 0x6:
				GST_DEBUG_OBJECT(uartsink, "ack (0x%02x) received", acknak);
				goto done;
				break;
			case 0x15:
				GST_DEBUG_OBJECT(uartsink, "nak (0x%02x) received", acknak);
				goto resend;
				break;
			default:
				GST_DEBUG_OBJECT(uartsink, "unknown byte for ack/nak (0x%02x)", acknak);
				flow = GST_FLOW_ERROR;
			}
		}
	}

done:
	gst_buffer_unmap(buffer, &info);

	return flow;

resend:
	GST_DEBUG_OBJECT(uartsink, "resending %" G_GSIZE_FORMAT" bytes", info.size);
	written = write(priv->uart->fd, info.data, info.size);
	uart_flush(priv->uart);

	goto done;
}

static gboolean
gst_uart_sink_start(GstBaseSink * basesink)
{
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;
	GstPollFD fd = GST_POLL_FD_INIT;
	GError *error = NULL;
	int ret;

	uartsink = GST_UART_SINK(basesink);
	priv = gst_uart_sink_get_instance_private(uartsink);
	GST_DEBUG("priv: %p", priv);
	GST_DEBUG("%s", priv->device);

	if (!priv->device || priv->device[0] == '\0')
		goto no_device;

	priv->uart = uart_open_raw(priv->device, O_RDWR);
	if (!priv->uart)
		goto open_failed;

	GST_DEBUG("c_iflag: 0x%x", priv->uart->orig.c_iflag);
	GST_DEBUG("c_oflag: 0x%x", priv->uart->orig.c_oflag);
	GST_DEBUG("c_cflag: 0x%x", priv->uart->orig.c_cflag);
	GST_DEBUG("c_lflag: 0x%x", priv->uart->orig.c_lflag);
	GST_DEBUG("ospeed: %d", cfgetispeed(&priv->uart->orig));
	GST_DEBUG("ospeed: %d", priv->uart->orig.c_ospeed);
	GST_DEBUG("ispeed: %d", priv->uart->orig.c_ispeed);
	GST_DEBUG("ispeed: %d", uart_get_baud_rate(priv->uart));

	ret = uart_set_baud_rate(priv->uart, priv->baud_rate, &error);
	if (error)
		goto setting_failed;
	GST_DEBUG("ret: %d", ret);
	GST_DEBUG("baud rate: %d", priv->baud_rate);

	uart_set_parity(priv->uart, priv->parity);

	GST_DEBUG("== after set parity ==");
	GST_DEBUG("c_iflag: 0x%x", priv->uart->orig.c_iflag);
	GST_DEBUG("c_oflag: 0x%x", priv->uart->orig.c_oflag);
	GST_DEBUG("c_cflag: 0x%x", priv->uart->orig.c_cflag);
	GST_DEBUG("ospeed: %d", cfgetispeed(&priv->uart->orig));
	GST_DEBUG("ospeed: %d", priv->uart->orig.c_ospeed);
	GST_DEBUG("ispeed: %d", priv->uart->orig.c_ispeed);
	GST_DEBUG("ispeed: %d", uart_get_baud_rate(priv->uart));

	priv->fdset_write = gst_poll_new (TRUE);
	if (!priv->fdset_write)
		goto poll_failed;

	fd.fd = priv->uart->fd;
	gst_poll_add_fd(priv->fdset_write, &fd);
	gst_poll_fd_ctl_write(priv->fdset_write, &fd, TRUE);

	priv->fdset_read = gst_poll_new (TRUE);
	if (!priv->fdset_read)
		goto poll_failed;

	fd.fd = priv->uart->fd;
	gst_poll_add_fd(priv->fdset_read, &fd);
	gst_poll_fd_ctl_read(priv->fdset_read, &fd, TRUE);

	priv->bytes_written = 0;
	priv->current_pos = 0;

	return TRUE;

	/* ERRORS */
no_device:
	{
		GST_ELEMENT_ERROR(uartsink, RESOURCE, NOT_FOUND,
				  ("No device name specified for data communication."), (NULL));
		return FALSE;
	}
open_failed:
	{
		GST_ELEMENT_ERROR(uartsink, RESOURCE, OPEN_WRITE,
				  ("Could not open device \"%s\" for data communication.", priv->device),
				  GST_ERROR_SYSTEM);
		return FALSE;
	}
setting_failed:
	{
		GST_ELEMENT_ERROR(uartsink, RESOURCE, SETTINGS,
				  ("%s", error->message), GST_ERROR_SYSTEM);
		g_close(priv->uart->fd, NULL);
		priv->uart->fd = -1;
		return FALSE;

	}
poll_failed:
	{
		g_close(priv->uart->fd, NULL);
		priv->uart->fd = -1;
		GST_ELEMENT_ERROR(uartsink, RESOURCE, OPEN_READ_WRITE, (NULL),
				  GST_ERROR_SYSTEM);
		return FALSE;
	}
}

static gboolean
gst_uart_sink_stop(GstBaseSink * basesink)
{
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;
	GstPollFD fd = GST_POLL_FD_INIT;

	GST_DEBUG("%s", __func__);
	uartsink = GST_UART_SINK(basesink);
	priv = gst_uart_sink_get_instance_private(uartsink);

	if (priv->uart) {
		GST_DEBUG("%s: close", __func__);
		fd.fd = priv->uart->fd;
		gst_poll_remove_fd(priv->fdset_write, &fd);
		gst_poll_remove_fd(priv->fdset_read, &fd);
		uart_close(priv->uart);
		priv->uart = NULL;

		gst_poll_free(priv->fdset_write);
		gst_poll_free(priv->fdset_read);
		priv->fdset_write = NULL;
		priv->fdset_read = NULL;
	}

	return TRUE;
}

static gboolean
gst_uart_sink_unlock(GstBaseSink * basesink)
{
	GstUartSink *uartsink = GST_UART_SINK(basesink);

	GST_LOG_OBJECT(uartsink, "Flushing");
	GST_OBJECT_LOCK(uartsink);
	/* uartsink->unlock = TRUE; */
	/* gst_poll_set_flushing (uartsink->fdset_write, TRUE); */
	GST_OBJECT_UNLOCK(uartsink);

	return TRUE;
}

static gboolean
gst_uart_sink_unlock_stop(GstBaseSink * basesink)
{
	GstUartSink *uartsink = GST_UART_SINK(basesink);

	GST_LOG_OBJECT(uartsink, "No longer flushing");
	GST_OBJECT_LOCK(uartsink);
	/* uartsink->unlock = FALSE; */
	/* gst_poll_set_flushing (uartsink->fdset_write, FALSE); */
	GST_OBJECT_UNLOCK(uartsink);

	return TRUE;
}

static void
gst_uart_sink_set_property(GObject * object, guint prop_id, const GValue * value,
			   GParamSpec * pspec)
{
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;

	uartsink = GST_UART_SINK(object);
	priv = gst_uart_sink_get_instance_private(uartsink);

	switch (prop_id) {
	case ARG_DEVICE:
		priv->device = g_value_dup_string(value);
		GST_DEBUG("device node: '%s'", priv->device);
		break;

	case ARG_BAUD_RATE:
		priv->baud_rate = g_value_get_int(value);
		GST_DEBUG("baud rate: '%d'", priv->baud_rate);
		break;

	case ARG_PARITY:
	{
		const char *s = g_value_get_string(value);
		if (g_str_equal(s, "no"))
			priv->parity = UART_PARITY_NO;
		else if (g_str_equal(s, "even"))
			priv->parity = UART_PARITY_EVEN;
		else if (g_str_equal(s, "odd"))
			priv->parity = UART_PARITY_ODD;

		GST_DEBUG("parity: '%s'", s);
		break;
	}
	case ARG_BITSWAP:
		priv->bitswap = g_value_get_boolean(value);
		GST_DEBUG("bitswap: '%d'", priv->bitswap);
		break;

	case ARG_ACKNAK:
		priv->acknak = g_value_get_boolean(value);
		GST_DEBUG("acknak: '%d'", priv->acknak);
		break;

	case ARG_ACKNAK_WAIT:
		priv->acknak_wait = g_value_get_uint(value);
		GST_DEBUG("acknak-wait: '%u'", priv->acknak_wait);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_uart_sink_get_property(GObject * object, guint prop_id, GValue * value,
			   GParamSpec * pspec)
{
	GstUartSink *uartsink;
	GstUartSinkPrivate *priv;

	uartsink = GST_UART_SINK(object);
	priv = gst_uart_sink_get_instance_private(uartsink);

	switch (prop_id) {
	case ARG_DEVICE:
		g_value_set_string(value, priv->device);
		break;

	case ARG_BAUD_RATE:
		g_value_set_int(value, priv->baud_rate);
		break;

	case ARG_PARITY:
		switch (priv->parity) {
		default:
			g_value_set_string(value, "no");
			break;
		case UART_PARITY_EVEN:
			g_value_set_string(value, "even");
			break;
		case UART_PARITY_ODD:
			g_value_set_string(value, "odd");
			break;
		}
		break;

	case ARG_BITSWAP:
		g_value_set_boolean(value, priv->bitswap);
		break;

	case ARG_ACKNAK:
		g_value_set_boolean(value, priv->acknak);
		break;

	case ARG_ACKNAK_WAIT:
		g_value_set_uint(value, priv->acknak_wait);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static gboolean
gst_uart_sink_event(GstBaseSink * sink, GstEvent * event)
{
	GstUartSink *uartsink;
	GstEventType type;

	uartsink = GST_UART_SINK(sink);
	type = GST_EVENT_TYPE(event);

	switch (type) {
	case GST_EVENT_SEGMENT:
		GST_DEBUG("segment");
		break;
	default:
		GST_DEBUG("%s", GST_EVENT_TYPE_NAME(event));
		break;
	}

	GST_DEBUG_OBJECT(uartsink, "NOT YET IMPLEMENTED");

	return GST_BASE_SINK_CLASS(gst_uart_sink_parent_class)->event(sink, event);
}
