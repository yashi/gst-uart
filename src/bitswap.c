#include <stddef.h>

void bitswap_non64bit(unsigned char *buf, size_t size)
{
	size_t i;

	for (i=0; i<size; i++)
		buf[i] = ((buf[i] * 0x0802LU & 0x22110LU) | (buf[i] * 0x8020LU & 0x88440LU)) * 0x10101LU >> 16;
}
