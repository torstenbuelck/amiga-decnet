/*
 * miniprintf.c - small vsnprintf/snprintf without stdio, for code that runs
 * without C startup (the DOS handler). libnix's versions pull in its stdio
 * initialisation. Supports %s %c %d %i %u %x %X %%, flags '-' '0', width,
 * precision for %s and the 'l' modifier.
 *
 * Copyright (C) 2026  Torsten Bülck (tuti)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <stddef.h>

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	size_t n = 0;

#define PUT(c) do { if (n + 1 < size) buf[n] = (c); n++; } while (0)
	for (; *fmt; fmt++) {
		char tmp[12], *s;
		int left = 0, zero = 0, width = 0, prec = -1, len, neg = 0, i;
		unsigned long v;

		if (*fmt != '%') {
			PUT(*fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			prec = 0;
			fmt++;
			while (*fmt >= '0' && *fmt <= '9')
				prec = prec * 10 + (*fmt++ - '0');
		}
		if (*fmt == 'l')
			fmt++;
		switch (*fmt) {
		case 's':
			s = va_arg(ap, char *);
			if (!s)
				s = "(null)";
			for (len = 0; s[len] && (prec < 0 || len < prec); len++)
				;
			break;
		case 'c':
			tmp[0] = (char)va_arg(ap, int);
			s = tmp;
			len = 1;
			break;
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X': {
			const char *digits = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			unsigned base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
			if (*fmt == 'd' || *fmt == 'i') {
				long sv = va_arg(ap, long);
				neg = sv < 0;
				v = neg ? -(unsigned long)sv : (unsigned long)sv;
			} else {
				v = va_arg(ap, unsigned long);
			}
			s = tmp + sizeof tmp;
			len = 0;
			do {
				*--s = digits[v % base];
				v /= base;
				len++;
			} while (v);
			break;
		}
		case '%':
			PUT('%');
			continue;
		default:
			if (!*fmt)
				goto done;
			PUT('%');
			PUT(*fmt);
			continue;
		}
		if (neg && zero)
			PUT('-');
		if (!left)
			for (i = len + neg; i < width; i++)
				PUT(zero ? '0' : ' ');
		if (neg && !zero)
			PUT('-');
		for (i = 0; i < len; i++)
			PUT(s[i]);
		if (left)
			for (i = len + neg; i < width; i++)
				PUT(' ');
	}
done:
	if (size)
		buf[n < size ? n : size - 1] = 0;
	return (int)n;
#undef PUT
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return r;
}
