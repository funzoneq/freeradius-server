/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/** A generic string buffer structure for string printing and parsing
 *
 * @file src/lib/util/sbuff.c
 *
 * @copyright 2020 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/util/sbuff.h>
#include <freeradius-devel/util/print.h>
#include <freeradius-devel/util/talloc.h>

#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static_assert(sizeof(long long) >= sizeof(int64_t), "long long must be as wide or wider than an int64_t");
static_assert(sizeof(unsigned long long) >= sizeof(uint64_t), "long long must be as wide or wider than an uint64_t");

fr_table_num_ordered_t const sbuff_parse_error_table[] = {
	{ "ok",			FR_SBUFF_PARSE_OK				},
	{ "token not found",	FR_SBUFF_PARSE_ERROR_NOT_FOUND			},
	{ "integer overflow",	FR_SBUFF_PARSE_ERROR_INTEGER_OVERFLOW		},
	{ "integer underflow",	FR_SBUFF_PARSE_ERROR_INTEGER_UNDERFLOW		},
};
size_t sbuff_parse_error_table_len = NUM_ELEMENTS(sbuff_parse_error_table);

/** Wind position to first instance of specified multibyte utf8 char
 *
 * Only use this function if the search char could be multibyte,
 * as there's a large performance penalty.
 *
 * @param[in,out] in		Sbuff to search in.
 * @param[in] chr		to search for.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the multi-byte chr was found.
 */
size_t fr_sbuff_strchr_utf8(fr_sbuff_t *in, char *chr)
{
	char const *found;
	char const *p = in->p_i;

	found = fr_utf8_strchr(NULL, p, in->end - in->p, chr);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(in, found - p);
}

/** Wind position to first instance of specified char
 *
 * @param[in,out] in		Sbuff to search in.
 * @param[in] c			to search for.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the char was found.
 */
size_t fr_sbuff_strchr(fr_sbuff_t *in, char c)
{
	char const *found;
	char const *p = in->p_i;

	found = memchr(in->p, c, in->end - in->p);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(in, found - p);
}

/** Wind position to the first instance of the specified needle
 *
 * @param[in,out] in		sbuff to search in.
 * @param[in] needle		to search for.
 * @param[in] len		Length of the needle.  -1 to use strlen.
 * @return
 *	- 0, no instances found.
 *	- >0 the offset at which the first occurrence of the needle was found.
 */
size_t fr_sbuff_strstr(fr_sbuff_t *in, char const *needle, ssize_t len)
{
	char const *found;
	char const *p = in->p;

	if (len < 0) len = strlen(needle);

	found = memmem(in->p, in->end - in->p, needle, len);
	if (!found) return 0;

	return (size_t)fr_sbuff_advance(in, found - p);
}

/** Wind position to the first non-whitespace character
 *
 * @param[in] in		sbuff to search in.
 * @return
 *	- 0, first character is not a whitespace character.
 *	- >0 how many whitespace characters we skipped.
 */
size_t fr_sbuff_skip_whitespace(fr_sbuff_t *in)
{
	char const *p = in->p;

	while ((in->p < in->end) && isspace(*(in->p))) in->p++;

	return (size_t)fr_sbuff_advance(in, in->p - p);
}

/** Copy n bytes from the sbuff to a talloced buffer
 *
 * Will fail if output buffer is too small, or insufficient data is available in sbuff.
 *
 * @param[in] ctx	to allocate talloced buffer in.
 * @param[out] out	Where to copy to.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *      - 0 if insufficient bytes are available in the sbuff.
 *	- >0 the number of bytes copied to out.
 */
size_t fr_sbuff_talloc_bstrncpy_out_exact(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len)
{
	if (len == SIZE_MAX) len = in->end - in->p;
	if ((in->p + len) > in->end) return 0;	/* Copying off the end of sbuff */

	*out = talloc_bstrndup(ctx, in->p, len);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many bytes as possible from the sbuff to a talloced buffer.
 *
 * Copy size is limited by available data in sbuff.
 *
 * @param[in] ctx	to allocate talloced buffer in.
 * @param[out] out	Where to copy to.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_talloc_bstrncpy_out(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len)
{
	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	*out = talloc_bstrndup(ctx, in->p, len);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many allowed characters as possible from the sbuff to a talloced buffer.
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[in] ctx		to allocate talloced buffer in.
 * @param[out] out		Where to copy to.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] allowed_chars	Characters to include the copy.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_talloc_bstrncpy_out_allowed(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len,
					    bool const allowed_chars[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	size_t		to_copy;

	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	end = p + len;

	while ((p < end) && allowed_chars[(uint8_t)*p]) p++;
	to_copy = (p - in->p);

	*out = talloc_bstrndup(ctx, in->p, to_copy);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, to_copy);

	return to_copy;
}

/** Copy as many allowed characters as possible from the sbuff to a talloced buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[in] ctx		to allocate talloced buffer in.
 * @param[out] out		Where to copy to.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] until		Characters which stop the copy operation.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_talloc_bstrncpy_out_until(TALLOC_CTX *ctx, char **out, fr_sbuff_t *in, size_t len,
					  bool const until[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	size_t		to_copy;

	if (len > fr_sbuff_remaining(in)) len = fr_sbuff_remaining(in);
	if (len == 0) {
		*out = talloc_bstrndup(ctx, "", 0);
		return 0;
	}

	end = p + len;

	while ((p < end) && !until[(uint8_t)*p]) p++;
	to_copy = (p - in->p);

	*out = talloc_bstrndup(ctx, in->p, to_copy);
	if (unlikely(!*out)) return 0;

	fr_sbuff_advance(in, to_copy);

	return to_copy;
}

/** Copy n bytes from the sbuff to another buffer
 *
 * Will fail if output buffer is too small, or insufficient data is available in sbuff.
 *
 * @param[out] out	Where to copy to.
 * @param[in] outlen	Size of output buffer.
 * @param[in] in	Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len	How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *      - 0 if insufficient bytes are available in the sbuff.
 *	- <0 the number of additional bytes we'd need in the output buffer as a negative value.
 *	- >0 the number of bytes copied to out.
 */
ssize_t fr_sbuff_bstrncpy_out_exact(char *out, size_t outlen, fr_sbuff_t *in, size_t len)
{
	if (len == SIZE_MAX) len = in->end - in->p;
	if (unlikely(outlen == 0)) return -(len + 1);

	outlen--;	/* Account the \0 byte */

	if (len > outlen) return outlen - len;	/* Return how many bytes we'd need */
	if ((in->p + len) > in->end) return 0;	/* Copying off the end of sbuff */

	memcpy(out, in->p, len);
	out[len] = '\0';

	fr_sbuff_advance(in, len);

	return len;
}

#define STRNCPY_TRIM_LEN(_len, _in, _outlen) \
do { \
	if (_len == SIZE_MAX) _len = _in->end - _in->p; \
	if (_len > _outlen) _len = _outlen; \
	if ((_in->p + _len) > _in->end) _len = (_in->end - _in->p); \
} while(0)

/** Copy as many bytes as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_bstrncpy_out(char *out, size_t outlen, fr_sbuff_t *in, size_t len)
{
	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */ \

	STRNCPY_TRIM_LEN(len, in, outlen);

	memcpy(out, in->p, len);
	out[len] = '\0';

	fr_sbuff_advance(in, len);

	return len;
}

/** Copy as many allowed characters as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] allowed_chars	Characters to include the copy.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_bstrncpy_out_allowed(char *out, size_t outlen, fr_sbuff_t *in, size_t len,
				     bool const allowed_chars[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	char		*out_p = out;
	size_t		copied;

	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */

	STRNCPY_TRIM_LEN(len, in, outlen);

	end = p + len;

	while ((p < end) && allowed_chars[(uint8_t)*p]) *out_p++ = *p++;
	*out_p = '\0';

	copied = (p - in->p);

	fr_sbuff_advance(in, copied);

	return copied;
}

/** Copy as many allowed characters as possible from the sbuff to another buffer
 *
 * Copy size is limited by available data in sbuff and output buffer length.
 *
 * As soon as a disallowed character is found the copy is stopped.
 *
 * @param[out] out		Where to copy to.
 * @param[in] outlen		Size of output buffer.
 * @param[in] in		Where to copy from.  Will copy len bytes from current position in buffer.
 * @param[in] len		How many bytes to copy.  If SIZE_MAX the entire buffer will be copied.
 * @param[in] until		Characters which stop the copy operation.
 * @return
 *	- 0 no bytes copied.
 *	- >0 the number of bytes copied.
 */
size_t fr_sbuff_bstrncpy_out_until(char *out, size_t outlen, fr_sbuff_t *in, size_t len,
				   bool const until[static UINT8_MAX + 1])
{
	char const	*p = in->p;
	char const	*end;
	char		*out_p = out;
	size_t		copied;

	if (unlikely(outlen == 0)) return 0;

	outlen--;	/* Account the \0 byte */

	STRNCPY_TRIM_LEN(len, in, outlen);

	end = p + len;

	while ((p < end) && !until[(uint8_t)*p]) *out_p++ = *p++;
	*out_p = '\0';

	copied = (p - in->p);

	fr_sbuff_advance(in, copied);

	return copied;
}

/** Used to define a number parsing functions for singed integers
 *
 * @param[in] _name	Function suffix.
 * @param[in] _type	Output type.
 * @param[in] _min	value.
 * @param[in] _max	value.
 * @param[in] _max_char	Maximum digits that can be used to represent an integer.
 *			Can't use stringify because of width modifiers like 'u'
 *			used in <stdint.h>.
 * @return
 *	- 0 no bytes copied.  Examine err.
 *	- >0 the number of bytes copied.
 */
#define PARSE_INT_DEF(_name, _type, _min, _max, _max_char) \
size_t fr_sbuff_parse_##_name(fr_sbuff_parse_error_t *err, _type *out, fr_sbuff_t *in, bool no_trailing) \
{ \
	char		buff[_max_char + 1]; \
	char		*end; \
	size_t		len; \
	long long	num; \
	fr_sbuff_t	our_in = FR_SBUFF_NO_ADVANCE(in); \
	len = fr_sbuff_bstrncpy_out(buff, sizeof(buff), &our_in, _max_char); \
	if ((len == 0) && err) { \
		*err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	num = strtoll(buff, &end, 10); \
	if (end == buff) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	if ((num > (_max)) || ((errno == EINVAL) && (num == LLONG_MAX)) || (no_trailing && isdigit(*(in->p + (end - buff)))))  { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_INTEGER_OVERFLOW; \
		*out = (_type)(_max); \
		return 0; \
	} else if (num < (_min) || ((errno == EINVAL) && (num == LLONG_MIN))) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_INTEGER_UNDERFLOW; \
		*out = (_type)(_min); \
		return 0; \
	} else { \
		if (err) *err = FR_SBUFF_PARSE_OK; \
		*out = (_type)(num); \
	} \
	fr_sbuff_advance(in, end - buff); /* Advance by the length strtoll gives us */ \
	return end - buff; \
}

PARSE_INT_DEF(int8, int8_t, INT8_MIN, INT8_MAX, 2)
PARSE_INT_DEF(int16, int16_t, INT16_MIN, INT16_MAX, 6)
PARSE_INT_DEF(int32, int32_t, INT32_MIN, INT32_MAX, 11)
PARSE_INT_DEF(int64, int64_t, INT64_MIN, INT64_MAX, 20)

/** Used to define a number parsing functions for singed integers
 *
 * @param[in] _name	Function suffix.
 * @param[in] _type	Output type.
 * @param[in] _max	value.
 * @param[in] _max_char	Maximum digits that can be used to represent an integer.
 *			Can't use stringify because of width modifiers like 'u'
 *			used in <stdint.h>.
 */
#define PARSE_UINT_DEF(_name, _type, _max, _max_char) \
size_t fr_sbuff_parse_##_name(fr_sbuff_parse_error_t *err, _type *out, fr_sbuff_t *in, bool no_trailing) \
{ \
	char			buff[_max_char + 1]; \
	char			*end; \
	size_t			len; \
	unsigned long long	num; \
	fr_sbuff_t		our_in = FR_SBUFF_NO_ADVANCE(in); \
	len = fr_sbuff_bstrncpy_out(buff, sizeof(buff), &our_in, _max_char); \
	if ((len == 0) && err) { \
		*err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	num = strtoull(buff, &end, 10); \
	if (end == buff) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_NOT_FOUND; \
		return 0; \
	} \
	if ((num > (_max)) || ((errno == EINVAL) && (num == ULLONG_MAX)) || (no_trailing && isdigit(*(in->p + (end - buff))))) { \
		if (err) *err = FR_SBUFF_PARSE_ERROR_INTEGER_OVERFLOW; \
		*out = (_type)(_max); \
		return 0; \
	} else { \
		if (err) *err = FR_SBUFF_PARSE_OK; \
		*out = (_type)(num); \
	} \
	fr_sbuff_advance(in, end - buff); /* Advance by the length strtoull gives us */ \
	return end - buff; \
}

PARSE_UINT_DEF(uint8, uint8_t, UINT8_MAX, 1)
PARSE_UINT_DEF(uint16, uint16_t, UINT16_MAX, 5)
PARSE_UINT_DEF(uint32, uint32_t, UINT32_MAX, 10)
PARSE_UINT_DEF(uint64, uint64_t, UINT64_MAX, 20)
