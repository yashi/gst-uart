#include <gst/gst.h>
#include "config.h"
#include "gstuartsink.h"
#include "gstuartsrc.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
        gst_element_register(plugin, "uartsink", GST_RANK_NONE, gst_uart_sink_get_type());
        gst_element_register(plugin, "uartsrc", GST_RANK_NONE, gst_uart_src_get_type());
        return TRUE;
}

GST_PLUGIN_DEFINE (
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        uart,
        "UART / TTY Plugins for GStreamer",
        plugin_init,
        VERSION,
        "LGPL",
        "GStreamer UART Package",
        "https://github.com/yashi/gst-uart")
