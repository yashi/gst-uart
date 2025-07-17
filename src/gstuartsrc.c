/* GStreamer
 * Copyright (C) 2025 Yasushi SHOJI <yashi@spacecubics.com>
 *
 * gstuartsrc.c:
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
#include "gstuartsrc.h"
#include "uart.h"
#include "bitswap.h"

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE("src",
								  GST_PAD_SRC,
								  GST_PAD_ALWAYS,
								  GST_STATIC_CAPS_ANY);

GST_DEBUG_CATEGORY_STATIC(gst_uart_src_debug);
#define GST_CAT_DEFAULT gst_uart_src_debug

enum {
	ARG_0,
	ARG_DEVICE,
	ARG_BAUD_RATE,
	ARG_PARITY,
	ARG_BITSWAP,
	ARG_ACKNAK,
	ARG_NAK_PROBABILITY,
};

struct _GstUartSrcPrivate {
	char *device;
	int baud_rate;
	enum UartParity parity;
	gboolean bitswap;
	gboolean acknak;
	guint nak_probability;
	struct uart *uart;
	GstPoll *fdset_read;
	GstPoll *fdset_write;
};

typedef struct _GstUartSrcPrivate GstUartSrcPrivate;

#define _do_init							\
	GST_DEBUG_CATEGORY_INIT (gst_uart_src_debug, "uartsrc", GST_DEBUG_FG_YELLOW | GST_DEBUG_BOLD, "uartsrc element"); \
	G_ADD_PRIVATE(GstUartSrc);

G_DEFINE_TYPE_WITH_CODE(GstUartSrc, gst_uart_src, GST_TYPE_PUSH_SRC, _do_init);

static void gst_uart_src_set_property(GObject * object, guint prop_id,
				      const GValue * value, GParamSpec * pspec);
static void gst_uart_src_get_property(GObject * object, guint prop_id, GValue * value,
				      GParamSpec * pspec);
static void gst_uart_src_dispose(GObject * obj);
static gboolean gst_uart_src_start(GstBaseSrc *basesrc);
static gboolean gst_uart_src_stop(GstBaseSrc *basesrc);
static gboolean gst_uart_src_unlock(GstBaseSrc *basesrc);
static gboolean gst_uart_src_unlock_stop(GstBaseSrc *basesrc);
static GstFlowReturn gst_uart_src_fill(GstPushSrc * pushsrc, GstBuffer * buffer);
static gboolean gst_uart_src_event(GstBaseSrc *src, GstEvent *event);

gboolean (*base_event) (GstBaseSrc *src, GstEvent *event);

static void
gst_uart_src_class_init(GstUartSrcClass * klass)
{
	GObjectClass *gobject_class;
	GstElementClass *gstelement_class;
	GstBaseSrcClass *gstbasesrc_class;
	GstPushSrcClass *gstpushsrc_class;

	gobject_class = G_OBJECT_CLASS(klass);
	gstelement_class = GST_ELEMENT_CLASS(klass);
	gstbasesrc_class = GST_BASE_SRC_CLASS(klass);
	gstpushsrc_class = GST_PUSH_SRC_CLASS(klass);

	gobject_class->set_property = gst_uart_src_set_property;
	gobject_class->get_property = gst_uart_src_get_property;
	gobject_class->dispose = gst_uart_src_dispose;

	gst_element_class_set_static_metadata(gstelement_class, "UART Source", "Src/UART",
					      "Read data from a uart / tty",
					      "Yasushi SHOJI <yashi@spacecubics.com>");
	gst_element_class_add_static_pad_template(gstelement_class, &srctemplate);

	gstbasesrc_class->start = GST_DEBUG_FUNCPTR(gst_uart_src_start);
	gstbasesrc_class->stop = GST_DEBUG_FUNCPTR(gst_uart_src_stop);
	gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR(gst_uart_src_unlock);
	gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR(gst_uart_src_unlock_stop);
	base_event = gstbasesrc_class->event;
	gstbasesrc_class->event = GST_DEBUG_FUNCPTR(gst_uart_src_event);

	gstpushsrc_class->fill = GST_DEBUG_FUNCPTR(gst_uart_src_fill);

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
	g_object_class_install_property(gobject_class, ARG_NAK_PROBABILITY,
					g_param_spec_uint("nak-probability", "NAK Probability",
							 "In number of packet, likelihood of returning NAK instead of ACK",
							 0, G_MAXUINT, 0,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_uart_src_init(GstUartSrc * uartsrc)
{
	GstUartSrcPrivate *priv = gst_uart_src_get_instance_private(uartsrc);
	priv->device = NULL;
	priv->baud_rate = 115200;
	priv->parity = UART_PARITY_NO;
	priv->bitswap = FALSE;
	priv->acknak = FALSE;
	priv->nak_probability = 0;
	priv->uart = NULL;
	priv->fdset_read = NULL;
	priv->fdset_write = NULL;

	gst_base_src_set_live (GST_BASE_SRC (uartsrc), FALSE);
	gst_base_src_set_do_timestamp (GST_BASE_SRC (uartsrc), TRUE);
}

static void
gst_uart_src_dispose(GObject * obj)
{
	GstUartSrcPrivate *priv = gst_uart_src_get_instance_private(GST_UART_SRC(obj));
	GST_DEBUG("priv->device: %s (@%p)", priv->device, priv->device);
	if (priv->device) {
		g_free(priv->device);
		priv->device = NULL;
	}

	G_OBJECT_CLASS(gst_uart_src_parent_class)->dispose(obj);
}

static gboolean
gst_uart_src_start(GstBaseSrc *basesrc)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;
	GstPollFD fd = GST_POLL_FD_INIT;
	GError *error = NULL;
	int ret;

	uartsrc = GST_UART_SRC(basesrc);
	priv = gst_uart_src_get_instance_private(uartsrc);

	if (!priv->device || priv->device[0] == '\0')
		goto no_device;

	priv->uart = uart_open_raw(priv->device, O_RDWR);
	if (!priv->uart)
		goto open_failed;

	GST_DEBUG_OBJECT(uartsrc, "opened %s as fd %d",
			 priv->device,
			 priv->uart->fd);

	GST_DEBUG("== orginal settings ==");
	GST_DEBUG("c_iflag: 0x%x", priv->uart->orig.c_iflag);
	GST_DEBUG("c_oflag: 0x%x", priv->uart->orig.c_oflag);
	GST_DEBUG("c_cflag: 0x%x", priv->uart->orig.c_cflag);
	GST_DEBUG("ispeed: %d", priv->uart->orig.c_ispeed);
	GST_DEBUG("ospeed: %d", priv->uart->orig.c_ospeed);
	GST_DEBUG("speed: %d", uart_get_baud_rate(priv->uart));
	GST_DEBUG("== current settings ==");
	GST_DEBUG("c_iflag: 0x%x", priv->uart->current.c_iflag);
	GST_DEBUG("c_oflag: 0x%x", priv->uart->current.c_oflag);
	GST_DEBUG("c_cflag: 0x%x", priv->uart->current.c_cflag);
	GST_DEBUG("ispeed: %d", priv->uart->current.c_ispeed);
	GST_DEBUG("ospeed: %d", priv->uart->current.c_ospeed);
	GST_DEBUG("priv->baud_rate: %d", priv->baud_rate);

	ret = uart_set_baud_rate(priv->uart, priv->baud_rate, &error);
	if (error)
		goto setting_failed;
	GST_DEBUG("ret: %d", ret);
	GST_DEBUG("baud rate: %d", priv->baud_rate);

	uart_set_parity(priv->uart, priv->parity);

	GST_DEBUG("== after set parity ==");
	GST_DEBUG("c_iflag: 0x%x", priv->uart->current.c_iflag);
	GST_DEBUG("c_oflag: 0x%x", priv->uart->current.c_oflag);
	GST_DEBUG("c_cflag: 0x%x", priv->uart->current.c_cflag);
	GST_DEBUG("ispeed: %d", priv->uart->current.c_ispeed);
	GST_DEBUG("ospeed: %d", priv->uart->current.c_ospeed);
	GST_DEBUG("priv->baud_rate: %d", priv->baud_rate);


	/* data receiving fd */
	priv->fdset_read = gst_poll_new(TRUE);
	if (!priv->fdset_read)
		goto poll_failed;

	fd.fd = priv->uart->fd;
	gst_poll_add_fd (priv->fdset_read, &fd);
	gst_poll_fd_ctl_read (priv->fdset_read, &fd, TRUE);

	/* ack nak control fd */
	priv->fdset_write = gst_poll_new(TRUE);
	if (!priv->fdset_write)
		goto poll_failed;

	fd.fd = priv->uart->fd;
	gst_poll_add_fd(priv->fdset_write, &fd);
	gst_poll_fd_ctl_write(priv->fdset_write, &fd, TRUE);

	return TRUE;

no_device:
	{
		GST_ELEMENT_ERROR(uartsrc, RESOURCE, NOT_FOUND,
				  ("No device name specified for data communication."), (NULL));
		return FALSE;
	}
open_failed:
	{
		GST_ELEMENT_ERROR(uartsrc, RESOURCE, OPEN_WRITE,
				  ("Could not open device \"%s\" for data communication.", priv->device),
				  GST_ERROR_SYSTEM);
		return FALSE;
	}
setting_failed:
	{
		GST_ELEMENT_ERROR(uartsrc, RESOURCE, SETTINGS,
				  ("%s", error->message), GST_ERROR_SYSTEM);
		g_close(priv->uart->fd, NULL);
		priv->uart->fd = -1;
		return FALSE;
	}
poll_failed:
	{
		g_close(priv->uart->fd, NULL);
		priv->uart->fd = -1;
		GST_ELEMENT_ERROR(uartsrc, RESOURCE, OPEN_READ_WRITE, (NULL),
				  GST_ERROR_SYSTEM);
		return FALSE;
	}
}

static gboolean
gst_uart_src_stop(GstBaseSrc *basesrc)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;
	GstPollFD fd;

	uartsrc = GST_UART_SRC(basesrc);
	priv = gst_uart_src_get_instance_private(uartsrc);

	GST_DEBUG_OBJECT(uartsrc, "%s", __func__);

	if (priv->uart) {
		GST_DEBUG("%s: close", __func__);
		gst_poll_fd_init(&fd);
		fd.fd = priv->uart->fd;
		gst_poll_remove_fd(priv->fdset_read, &fd);
		gst_poll_remove_fd(priv->fdset_write, &fd);

		uart_close(priv->uart);
		priv->uart = NULL;

		gst_poll_free(priv->fdset_read);
		gst_poll_free(priv->fdset_write);
		priv->fdset_read = NULL;
		priv->fdset_write = NULL;
	}

	return TRUE;
}

static gboolean
gst_uart_src_unlock(GstBaseSrc *basesrc)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;

	uartsrc = GST_UART_SRC(basesrc);
	priv = gst_uart_src_get_instance_private(uartsrc);

	GST_DEBUG_OBJECT(uartsrc, "%s", __func__);

	gst_poll_set_flushing(priv->fdset_read, TRUE);
	gst_poll_set_flushing(priv->fdset_write, TRUE);

	return TRUE;
}

static gboolean
gst_uart_src_unlock_stop(GstBaseSrc *basesrc)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;

	uartsrc = GST_UART_SRC(basesrc);
	priv = gst_uart_src_get_instance_private(uartsrc);

	GST_DEBUG_OBJECT(uartsrc, "%s", __func__);

	gst_poll_set_flushing(priv->fdset_read, FALSE);
	gst_poll_set_flushing(priv->fdset_write, FALSE);

	return TRUE;
}

static GstFlowReturn
gst_uart_src_fill(GstPushSrc * pushsrc, GstBuffer * buffer)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;
	GstMapInfo info;
	gsize size;
	ssize_t red;
	gsize max;
	GstPollFD fd = GST_POLL_FD_INIT;
	gint ret;

	uartsrc = GST_UART_SRC(pushsrc);
	priv = gst_uart_src_get_instance_private(uartsrc);

	size = gst_buffer_get_sizes(buffer, NULL, &max);

	GST_DEBUG_OBJECT(uartsrc, "given buffer's size (%" G_GSIZE_FORMAT ") and max size (%" G_GSIZE_FORMAT ")",
			 size, max);

	ret = gst_poll_wait(priv->fdset_read, GST_CLOCK_TIME_NONE);
	GST_DEBUG_OBJECT(uartsrc, "gst_poll_wait() returned %d", ret);
	if (ret < 0)
		return GST_FLOW_FLUSHING;

	fd.fd = priv->uart->fd;
	if (gst_poll_fd_can_read(priv->fdset_read, &fd)) {
		gst_buffer_map(buffer, &info, GST_MAP_WRITE);
		red = read(priv->uart->fd, info.data, size);
		if (priv->bitswap)
			bitswap(info.data, size);
		GST_DEBUG_OBJECT(uartsrc, "the first byte %x", *info.data);
		gst_buffer_unmap(buffer, &info);
		gst_buffer_set_size(buffer, red);
		GST_DEBUG_OBJECT(uartsrc, "read %zu bytes from \"%s\" (%d)", red, priv->device, priv->uart->fd);
		GST_DEBUG_OBJECT(uartsrc, "%" GST_PTR_FORMAT, buffer);
	}

	return GST_FLOW_OK;
}

static void
gst_uart_src_set_property(GObject * object, guint prop_id, const GValue * value,
			   GParamSpec * pspec)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;

	uartsrc = GST_UART_SRC(object);
	priv = gst_uart_src_get_instance_private(uartsrc);

	switch (prop_id) {
	case ARG_DEVICE:
		priv->device = g_value_dup_string(value);
		GST_INFO("setting property \'%s\' to \"%s\"", g_param_spec_get_name(pspec), priv->device);
		break;

	case ARG_BAUD_RATE:
		priv->baud_rate = g_value_get_int(value);
		GST_INFO("setting property \'%s\' to %d", g_param_spec_get_name(pspec), priv->baud_rate);
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

		GST_INFO("setting property \'%s\' to \"%s\"", g_param_spec_get_name(pspec), s);
		break;
	}
	case ARG_BITSWAP:
		priv->bitswap = g_value_get_boolean(value);
		GST_INFO("setting property \'%s\' to %d", g_param_spec_get_name(pspec), priv->bitswap);
		break;

	case ARG_ACKNAK:
		priv->acknak = g_value_get_boolean(value);
		GST_INFO("setting property \'%s\' to %d", g_param_spec_get_name(pspec), priv->acknak);
		break;

	case ARG_NAK_PROBABILITY:
		priv->nak_probability = g_value_get_uint(value);
		GST_INFO("setting property \'%s\' to %u", g_param_spec_get_name(pspec), priv->nak_probability);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gst_uart_src_get_property(GObject * object, guint prop_id, GValue * value,
			   GParamSpec * pspec)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;

	uartsrc = GST_UART_SRC(object);
	priv = gst_uart_src_get_instance_private(uartsrc);

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

	case ARG_NAK_PROBABILITY:
		g_value_set_uint(value, priv->nak_probability);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static gboolean
gst_uart_src_event(GstBaseSrc *src, GstEvent *event)
{
	GstUartSrc *uartsrc;
	GstUartSrcPrivate *priv;
	guint8 ack = 0x6;
	guint8 nak = 0x15;
	guint8 response;
	gboolean result;
	static guint64 count = 1;

	uartsrc = GST_UART_SRC(src);
	priv = gst_uart_src_get_instance_private(uartsrc);


	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_CUSTOM_UPSTREAM:
		count++;
		GST_DEBUG_OBJECT(src, "got a custom event %" GST_PTR_FORMAT, event);
		GST_DEBUG_OBJECT(src, "=== count %" G_GUINT64_FORMAT, count);
		if (priv->acknak) {
			if (priv->nak_probability && ((count % priv->nak_probability) == 0)) {
				response = nak;
				GST_WARNING_OBJECT(src, "Sending nak");
			}
			else {
				response = ack;
				GST_DEBUG_OBJECT(src, "Sending ack");
			}
			write(priv->uart->fd, &response, 1);

		}
		else
			GST_INFO_OBJECT(src, "but not sending it since ack/nak is not enabled");

		result = TRUE;
		break;
	default:
		result = base_event(src, event);
		break;
	}
	return result;

}
