#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include "uart.h"

typedef enum {
	UART_SETTING_ERROR_NO_BAUD,
	UART_SETTING_ERROR_INVALID_ARGS,
} UartSettingError;

#define UART_SETTING_ERROR uart_setting_error_quark()
GQuark uart_setting_error_quark(void);

static int speed_to_baud(speed_t speed)
{
	int ret;

	switch (speed) {
	case   B2400: ret =   2400; break;
	case   B4800: ret =   4800; break;
	case   B9600: ret =   9600; break;
	case  B19200: ret =  19200; break;
	case  B38400: ret =  38400; break;
	case  B57600: ret =  57600; break;
	case B115200: ret = 115200; break;
	case B230400: ret = 230400; break;
	case B460800: ret = 460800; break;
	default: ret = -1; break;
	}
	return ret;
}

static speed_t baud_to_speed(int baud)
{
	speed_t ret;

	switch (baud) {
	case   2400: ret =   B2400; break;
	case   4800: ret =   B4800; break;
	case   9600: ret =   B9600; break;
	case  19200: ret =  B19200; break;
	case  38400: ret =  B38400; break;
	case  57600: ret =  B57600; break;
	case 115200: ret = B115200; break;
	case 230400: ret = B230400; break;
	case 460800: ret = B460800; break;
	default: ret = B0; break;
	}
	return ret;
}

struct uart* uart_open(const char *name, int flags)
{
	struct uart *uart;

	uart = g_new(struct uart, 1);
	uart->fd = g_open(name, flags | O_NOCTTY | O_CLOEXEC);
	tcflush(uart->fd, TCIOFLUSH);
	tcgetattr(uart->fd, &uart->current);
	uart->orig = uart->current;

	return uart;
}

struct uart* uart_open_raw(const char *name, int flags)
{
	struct uart *uart;

	uart = uart_open(name, flags);
	uart->current.c_iflag = 0;
	uart->current.c_oflag = 0;
	cfmakeraw(&uart->current);
	tcsetattr(uart->fd, TCSAFLUSH, &uart->current);
	/* just in case; read back the current setting from the serial port */
	tcgetattr(uart->fd, &uart->current);

	return uart;
}

void uart_close(struct uart *uart)
{
	g_return_if_fail(uart);

	tcsetattr(uart->fd, TCSAFLUSH, &uart->orig);
	g_close(uart->fd, NULL);
	g_free(uart);
}

int uart_get_baud_rate(struct uart *uart)
{
	struct termios options;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);

	return speed_to_baud(cfgetispeed(&options));
}

int uart_set_baud_rate(struct uart *uart, int baud, GError **error)
{
	struct termios options;
	speed_t speed;
	int ret;

	g_return_val_if_fail(uart, -1);

	speed = baud_to_speed(baud);
	if (speed == B0) {
		g_set_error(error, UART_SETTING_ERROR, UART_SETTING_ERROR_NO_BAUD, "Unsupported baud rate %d", baud);
		return -1;
	}

	ret = tcgetattr(uart->fd, &options);
	if (ret < 0) {
		g_error("tcgetattr: %s", strerror(errno));
		return -1;
	}
	ret = cfsetspeed(&options, speed);
	if (ret < 0) {
		g_error("cfsetspeed: %s", strerror(errno));
		return -1;
	}
	ret = tcsetattr(uart->fd, TCSAFLUSH, &options);
	if (ret < 0) {
		g_error("tcsetattr: %s", strerror(errno));
	}
	ret = tcgetattr(uart->fd, &options);
	if (ret < 0) {
		g_error("tcgetattr: %s", strerror(errno));
		return -1;
	}
	uart->current = options;

	return ret;
}

enum UartParity uart_get_parity(struct uart *uart)
{
	struct termios options;
	enum UartParity ret = UART_PARITY_NO;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);
	if (options.c_cflag & PARENB) {
		if (options.c_cflag & PARODD)
			ret = UART_PARITY_ODD;
		else
			ret = UART_PARITY_EVEN;
	}

	return ret;
}

int uart_set_parity(struct uart *uart, enum UartParity parity)
{
	struct termios options;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);
	switch (parity) {
	case UART_PARITY_EVEN:
		options.c_cflag |= PARENB;
		options.c_cflag &= ~PARODD;
		break;
	case UART_PARITY_ODD:
		options.c_cflag |= PARENB;
		options.c_cflag |= PARODD;
		break;
	case UART_PARITY_NO:
	default:
		options.c_cflag &= ~PARENB;
		break;
	}

	return tcsetattr(uart->fd, TCSAFLUSH, &options);
}

int uart_get_stop_bit(struct uart *uart)
{
	struct termios options;
	int ret = 1;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);
	if (options.c_cflag & CSTOPB)
		ret = 2;

	return ret;
}

int uart_set_stop_bit_1(struct uart *uart)
{
	struct termios options;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);
	options.c_cflag &= ~CSTOPB;

	return tcsetattr(uart->fd, TCSAFLUSH, &options);
}

int uart_set_stop_bit_2(struct uart *uart)
{
	struct termios options;

	g_return_val_if_fail(uart, -1);

	tcgetattr(uart->fd, &options);
	options.c_cflag |= CSTOPB;

	return tcsetattr(uart->fd, TCSAFLUSH, &options);
}

int uart_flush(struct uart *uart)
{
	g_return_val_if_fail(uart, -1);

	return tcdrain(uart->fd);
}

GQuark uart_setting_error_quark(void)
{
	return g_quark_from_static_string("uart-setting-error-quark");
}
