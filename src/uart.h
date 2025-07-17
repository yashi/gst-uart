#pragma once

#include <termios.h>

enum UartParity {
	UART_PARITY_NO,
	UART_PARITY_EVEN,
	UART_PARITY_ODD,
};

struct uart {
	int fd;
	struct termios orig;
	struct termios current;
};

struct uart* uart_open(const char *name, int flags);
struct uart* uart_open_raw(const char *name, int flags);
void uart_close(struct uart *uart);

int uart_get_baud_rate(struct uart *uart);
int uart_set_baud_rate(struct uart *uart, int baud, GError **err);

enum UartParity uart_get_parity(struct uart *uart);
int uart_set_parity(struct uart *uart, enum UartParity parity);

int uart_get_stop_bit(struct uart *uart);
int uart_set_stop_bit_1(struct uart *uart);
int uart_set_stop_bit_2(struct uart *uart);

int uart_flush(struct uart *uart);
