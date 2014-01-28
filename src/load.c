/*
 * Copyright (c) 2009-2014 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "jansson.h"
#include "jansson_private.h"
#include "strbuffer.h"
#include "utf.h"

#define STREAM_STATE_OK        0
#define STREAM_STATE_EOF      -1
#define STREAM_STATE_ERROR    -2

#define TOKEN_INVALID         -1
#define TOKEN_EOF              0
#define TOKEN_STRING         256
#define TOKEN_INTEGER        257
#define TOKEN_REAL           258
#define TOKEN_TRUE           259
#define TOKEN_FALSE          260
#define TOKEN_NULL           261
#define TOKEN_BIGINTEGER     262
#define TOKEN_BIGREAL        263

/* Big number functions */
json_bigint_funcs_t* jsonp_biginteger_funcs = NULL;
json_bigreal_funcs_t* jsonp_bigreal_funcs = NULL;

/* Locale independent versions of isxxx() functions */
#define l_isupper(c)  ('A' <= (c) && (c) <= 'Z')
#define l_islower(c)  ('a' <= (c) && (c) <= 'z')
#define l_isalpha(c)  (l_isupper(c) || l_islower(c))
#define l_isdigit(c)  ('0' <= (c) && (c) <= '9')
#define l_isxdigit(c) \
    (l_isdigit(c) || ('A' <= (c) && (c) <= 'F') || ('a' <= (c) && (c) <= 'f'))

/* Read one byte from stream, convert to unsigned char, then int, and
   return. return EOF on end of file. This corresponds to the
   behaviour of fgetc(). */
typedef int (*get_func)(void *data);

typedef struct {
    get_func get;
    void *data;
    char buffer[5];
    size_t buffer_pos;
    int state;
    int line;
    int column, last_column;
    size_t position;
} stream_t;

typedef struct {
    stream_t stream;
    strbuffer_t saved_text;
    int token;
    union {
        struct {
            char *val;
            size_t len;
        } string;
        json_int_t integer;
        double real;
        json_bigz_t bigz;
        json_bigr_t bigr;
    } value;
} lex_t;

#define stream_to_lex(stream) container_of(stream, lex_t, stream)

/* forward references */
static int lex_init(lex_t *, get_func, void *);
static void lex_clear(lex_t *);
static void lex_close(lex_t *);


/*** error reporting ***/

static void error_set(json_error_t *error, const lex_t *lex,
                      const char *msg, ...)
{
    va_list ap;
    char msg_text[JSON_ERROR_TEXT_LENGTH];
    char msg_with_context[JSON_ERROR_TEXT_LENGTH];

    int line = -1, col = -1;
    size_t pos = 0;
    const char *result = msg_text;

    if(!error)
        return;

    va_start(ap, msg);
    vsnprintf(msg_text, JSON_ERROR_TEXT_LENGTH, msg, ap);
    msg_text[JSON_ERROR_TEXT_LENGTH - 1] = '\0';
    va_end(ap);

    if(lex)
    {
        const char *saved_text = strbuffer_value(&lex->saved_text);

        line = lex->stream.line;
        col = lex->stream.column;
        pos = lex->stream.position;

        if(saved_text && saved_text[0])
        {
            if(lex->saved_text.length <= 20) {
                snprintf(msg_with_context, JSON_ERROR_TEXT_LENGTH,
                         "%s near '%s'", msg_text, saved_text);
                msg_with_context[JSON_ERROR_TEXT_LENGTH - 1] = '\0';
                result = msg_with_context;
            }
        }
        else
        {
            if(lex->stream.state == STREAM_STATE_ERROR) {
                /* No context for UTF-8 decoding errors */
                result = msg_text;
            }
            else {
                snprintf(msg_with_context, JSON_ERROR_TEXT_LENGTH,
                         "%s near end of file", msg_text);
                msg_with_context[JSON_ERROR_TEXT_LENGTH - 1] = '\0';
                result = msg_with_context;
            }
        }
    }

    jsonp_error_set(error, line, col, pos, "%s", result);
}


/*** lexical analyzer ***/

static void
stream_init(stream_t *stream, get_func get, void *data)
{
    stream->get = get;
    stream->data = data;
    stream->buffer[0] = '\0';
    stream->buffer_pos = 0;

    stream->state = STREAM_STATE_OK;
    stream->line = 1;
    stream->column = 0;
    stream->position = 0;
}

static int stream_get(stream_t *stream, json_error_t *error)
{
    int c;

    if(stream->state != STREAM_STATE_OK)
        return stream->state;

    if(!stream->buffer[stream->buffer_pos])
    {
        c = stream->get(stream->data);
        if(c == EOF) {
            stream->state = STREAM_STATE_EOF;
            return STREAM_STATE_EOF;
        }

        stream->buffer[0] = c;
        stream->buffer_pos = 0;

        if(0x80 <= c && c <= 0xFF)
        {
            /* multi-byte UTF-8 sequence */
            int i, count;

            count = utf8_check_first(c);
            if(!count)
                goto out;

            assert(count >= 2);

            for(i = 1; i < count; i++)
                stream->buffer[i] = stream->get(stream->data);

            if(!utf8_check_full(stream->buffer, count, NULL))
                goto out;

            stream->buffer[count] = '\0';
        }
        else
            stream->buffer[1] = '\0';
    }

    c = stream->buffer[stream->buffer_pos++];

    stream->position++;
    if(c == '\n') {
        stream->line++;
        stream->last_column = stream->column;
        stream->column = 0;
    }
    else if(utf8_check_first(c)) {
        /* track the Unicode character column, so increment only if
           this is the first character of a UTF-8 sequence */
        stream->column++;
    }

    return c;

out:
    stream->state = STREAM_STATE_ERROR;
    error_set(error, stream_to_lex(stream), "unable to decode byte 0x%x", c);
    return STREAM_STATE_ERROR;
}

static void stream_unget(stream_t *stream, int c)
{
    if(c == STREAM_STATE_EOF || c == STREAM_STATE_ERROR)
        return;

    stream->position--;
    if(c == '\n') {
        stream->line--;
        stream->column = stream->last_column;
    }
    else if(utf8_check_first(c))
        stream->column--;

    assert(stream->buffer_pos > 0);
    stream->buffer_pos--;
    assert(stream->buffer[stream->buffer_pos] == c);
}


static int lex_get(lex_t *lex, json_error_t *error)
{
    return stream_get(&lex->stream, error);
}

static void lex_save(lex_t *lex, int c)
{
    strbuffer_append_byte(&lex->saved_text, c);
}

static int lex_get_save(lex_t *lex, json_error_t *error)
{
    int c = stream_get(&lex->stream, error);
    if(c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR)
        lex_save(lex, c);
    return c;
}

static void lex_unget(lex_t *lex, int c)
{
    stream_unget(&lex->stream, c);
}

static void lex_unget_unsave(lex_t *lex, int c)
{
    if(c != STREAM_STATE_EOF && c != STREAM_STATE_ERROR) {
        /* Since we treat warnings as errors, when assertions are turned
         * off the "d" variable would be set but never used. Which is
         * treated as an error by GCC.
         */
        #ifndef NDEBUG
        char d;
        #endif
        stream_unget(&lex->stream, c);
        #ifndef NDEBUG
        d = 
        #endif
            strbuffer_pop(&lex->saved_text);
        assert(c == d);
    }
}

static void lex_save_cached(lex_t *lex)
{
    while(lex->stream.buffer[lex->stream.buffer_pos] != '\0')
    {
        lex_save(lex, lex->stream.buffer[lex->stream.buffer_pos]);
        lex->stream.buffer_pos++;
        lex->stream.position++;
    }
}

static void lex_free_string(lex_t *lex)
{
    jsonp_free(lex->value.string.val);
    lex->value.string.val = NULL;
    lex->value.string.len = 0;
}

/* assumes that str points to 'u' plus at least 4 valid hex digits */
static int32_t decode_unicode_escape(const char *str)
{
    int i;
    int32_t value = 0;

    assert(str[0] == 'u');

    for(i = 1; i <= 4; i++) {
        char c = str[i];
        value <<= 4;
        if(l_isdigit(c))
            value += c - '0';
        else if(l_islower(c))
            value += c - 'a' + 10;
        else if(l_isupper(c))
            value += c - 'A' + 10;
        else
            return -1;
    }

    return value;
}

static void lex_scan_string(lex_t *lex, json_error_t *error)
{
    int c;
    const char *p;
    char *t;
    int i;

    lex->value.string.val = NULL;
    lex->token = TOKEN_INVALID;

    c = lex_get_save(lex, error);

    while(c != '"') {
        if(c == STREAM_STATE_ERROR)
            goto out;

        else if(c == STREAM_STATE_EOF) {
            error_set(error, lex, "premature end of input");
            goto out;
        }

        else if(0 <= c && c <= 0x1F) {
            /* control character */
            lex_unget_unsave(lex, c);
            if(c == '\n')
                error_set(error, lex, "unexpected newline", c);
            else
                error_set(error, lex, "control character 0x%x", c);
            goto out;
        }

        else if(c == '\\') {
            c = lex_get_save(lex, error);
            if(c == 'u') {
                c = lex_get_save(lex, error);
                for(i = 0; i < 4; i++) {
                    if(!l_isxdigit(c)) {
                        error_set(error, lex, "invalid escape");
                        goto out;
                    }
                    c = lex_get_save(lex, error);
                }
            }
            else if(c == '"' || c == '\\' || c == '/' || c == 'b' ||
                    c == 'f' || c == 'n' || c == 'r' || c == 't')
                c = lex_get_save(lex, error);
            else {
                error_set(error, lex, "invalid escape");
                goto out;
            }
        }
        else
            c = lex_get_save(lex, error);
    }

    /* the actual value is at most of the same length as the source
       string, because:
         - shortcut escapes (e.g. "\t") (length 2) are converted to 1 byte
         - a single \uXXXX escape (length 6) is converted to at most 3 bytes
         - two \uXXXX escapes (length 12) forming an UTF-16 surrogate pair
           are converted to 4 bytes
    */
    t = jsonp_malloc(lex->saved_text.length + 1);
    if(!t) {
        /* this is not very nice, since TOKEN_INVALID is returned */
        goto out;
    }
    lex->value.string.val = t;

    /* + 1 to skip the " */
    p = strbuffer_value(&lex->saved_text) + 1;

    while(*p != '"') {
        if(*p == '\\') {
            p++;
            if(*p == 'u') {
                size_t length;
                int32_t value;

                value = decode_unicode_escape(p);
                if(value < 0) {
                    error_set(error, lex, "invalid Unicode escape '%.6s'", p - 1);
                    goto out;
                }
                p += 5;

                if(0xD800 <= value && value <= 0xDBFF) {
                    /* surrogate pair */
                    if(*p == '\\' && *(p + 1) == 'u') {
                        int32_t value2 = decode_unicode_escape(++p);
                        if(value2 < 0) {
                            error_set(error, lex, "invalid Unicode escape '%.6s'", p - 1);
                            goto out;
                        }
                        p += 5;

                        if(0xDC00 <= value2 && value2 <= 0xDFFF) {
                            /* valid second surrogate */
                            value =
                                ((value - 0xD800) << 10) +
                                (value2 - 0xDC00) +
                                0x10000;
                        }
                        else {
                            /* invalid second surrogate */
                            error_set(error, lex,
                                      "invalid Unicode '\\u%04X\\u%04X'",
                                      value, value2);
                            goto out;
                        }
                    }
                    else {
                        /* no second surrogate */
                        error_set(error, lex, "invalid Unicode '\\u%04X'",
                                  value);
                        goto out;
                    }
                }
                else if(0xDC00 <= value && value <= 0xDFFF) {
                    error_set(error, lex, "invalid Unicode '\\u%04X'", value);
                    goto out;
                }

                if(utf8_encode(value, t, &length))
                    assert(0);
                t += length;
            }
            else {
                switch(*p) {
                    case '"': case '\\': case '/':
                        *t = *p; break;
                    case 'b': *t = '\b'; break;
                    case 'f': *t = '\f'; break;
                    case 'n': *t = '\n'; break;
                    case 'r': *t = '\r'; break;
                    case 't': *t = '\t'; break;
                    default: assert(0);
                }
                t++;
                p++;
            }
        }
        else
            *(t++) = *(p++);
    }
    *t = '\0';
    lex->value.string.len = t - lex->value.string.val;
    lex->token = TOKEN_STRING;
    return;

out:
    lex_free_string(lex);
}

#ifndef JANSSON_USING_CMAKE /* disabled if using cmake */
#if JSON_INTEGER_IS_LONG_LONG
#ifdef _MSC_VER  /* Microsoft Visual Studio */
#define json_strtoint     _strtoi64
#else
#define json_strtoint     strtoll
#endif
#else
#define json_strtoint     strtol
#endif
#endif

static int lex_scan_number(lex_t *lex, int c, size_t flags, json_error_t *error)
{
    json_context_t *ctx;
    const char *saved_text;
    char *end;
    int significand_digits;

    ctx = jsonp_context();
    lex->token = TOKEN_INVALID;

    if(c == '-')
        c = lex_get_save(lex, error);

    if(c == '0') {
        c = lex_get_save(lex, error);
        if(l_isdigit(c)) {
            lex_unget_unsave(lex, c);
	    error_set(error, lex,
                      "numbers may not have unnecessary leading zeros");
            goto out;
        }
    }
    else if(l_isdigit(c)) {
        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }
    else {
        lex_unget_unsave(lex, c);
        goto out;
    }

    if(c != '.' && c != 'E' && c != 'e') {
        lex_unget_unsave(lex, c);

        saved_text = strbuffer_value(&lex->saved_text);

        errno = 0;

	if(flags & JSON_USE_BIGINT_ALWAYS) {
	    json_bigz_t bigvalue;
	    bigvalue = ctx->bigint.from_string_fn(saved_text, &ctx->memfuncs);
	    lex->token = TOKEN_BIGINTEGER;
	    lex->value.bigz = bigvalue;
	}
	else {
	    json_int_t value;

	    value = json_strtoint(saved_text, &end, 10);
	    if(errno == ERANGE) {
		if(flags & JSON_USE_BIGINT) {
		    json_bigz_t bigvalue;
		    bigvalue = ctx->bigint.from_string_fn(saved_text, &ctx->memfuncs);
		    lex->token = TOKEN_BIGINTEGER;
		    lex->value.bigz = bigvalue;
		}
		else {
		    if(value < 0)
			error_set(error, lex, "too big negative integer");
		    else
			error_set(error, lex, "too big integer");
		    goto out;
		}
	    }
	    else {
		assert(end == saved_text + lex->saved_text.length);
		lex->token = TOKEN_INTEGER;
		lex->value.integer = value;
	    }
	}

        return 0;
    }

    if(c == '.') {
        c = lex_get(lex, error);
        if(!l_isdigit(c)) {
            lex_unget(lex, c);
            goto out;
        }
        lex_save(lex, c);

        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }

    if((flags & JSON_USE_BIGREAL) && ! (flags & JSON_USE_BIGREAL_ALWAYS)) {
	/* Determine digits of precision needed to store number before
	 * a partial loss of precision occurs.
	 */
	significand_digits = jsonp_count_significand_digits(&lex->saved_text);
    }

    if(c == 'E' || c == 'e') {
        c = lex_get_save(lex, error);
        if(c == '+' || c == '-')
            c = lex_get_save(lex, error);

        if(!l_isdigit(c)) {
            lex_unget_unsave(lex, c);
            goto out;
        }

        c = lex_get_save(lex, error);
        while(l_isdigit(c))
            c = lex_get_save(lex, error);
    }

    lex_unget_unsave(lex, c);

    if((flags & JSON_USE_BIGREAL_ALWAYS) ||
	((flags & JSON_USE_BIGREAL) && significand_digits+1 >= DBL_DIG)) {
	json_bigr_t bigvalue = NULL;
	saved_text = strbuffer_value(&lex->saved_text);
	bigvalue = ctx->bigreal.from_string_fn(saved_text, &ctx->memfuncs);
	lex->token = TOKEN_BIGREAL;
	lex->value.bigr = bigvalue;
    }
    else {
	double value;
	int rc;

	rc = jsonp_strtod(&lex->saved_text, &value);
	if(errno == ERANGE && (flags & JSON_USE_BIGREAL)) {
	    /* overflow or underflow */
	    json_bigr_t bigvalue;
	    saved_text = strbuffer_value(&lex->saved_text);
	    bigvalue = ctx->bigreal.from_string_fn(saved_text, &ctx->memfuncs);
	    lex->token = TOKEN_BIGREAL;
	    lex->value.bigr = bigvalue;
	}
	else if(rc != 0) {
	    error_set(error, lex, "real number overflow");
	    goto out;
	}
	else {
	    lex->token = TOKEN_REAL;
	    lex->value.real = value;
	}
    }

    return 0;

out:
    return -1;
}

static int lex_scan(lex_t *lex, size_t flags, json_error_t *error)
{
    int c;

    lex_clear(lex);

    c = lex_get(lex, error);
    while(c == ' ' || c == '\t' || c == '\n' || c == '\r')
        c = lex_get(lex, error);

    if(c == STREAM_STATE_EOF) {
        lex->token = TOKEN_EOF;
        goto out;
    }

    if(c == STREAM_STATE_ERROR) {
        lex->token = TOKEN_INVALID;
        goto out;
    }

    lex_save(lex, c);

    if(c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
        lex->token = c;

    else if(c == '"')
        lex_scan_string(lex, error);

    else if(l_isdigit(c) || c == '-') {
        if(lex_scan_number(lex, c, flags, error))
            goto out;
    }

    else if(l_isalpha(c)) {
        /* eat up the whole identifier for clearer error messages */
        const char *saved_text;

        c = lex_get_save(lex, error);
        while(l_isalpha(c))
            c = lex_get_save(lex, error);
        lex_unget_unsave(lex, c);

        saved_text = strbuffer_value(&lex->saved_text);

        if(strcmp(saved_text, "true") == 0)
            lex->token = TOKEN_TRUE;
        else if(strcmp(saved_text, "false") == 0)
            lex->token = TOKEN_FALSE;
        else if(strcmp(saved_text, "null") == 0)
            lex->token = TOKEN_NULL;
        else
            lex->token = TOKEN_INVALID;
    }

    else {
        /* save the rest of the input UTF-8 sequence to get an error
           message of valid UTF-8 */
        lex_save_cached(lex);
        lex->token = TOKEN_INVALID;
    }

out:
    return lex->token;
}

static char *lex_steal_string(lex_t *lex, size_t *out_len)
{
    char *result = NULL;
    if(lex->token == TOKEN_STRING) {
        result = lex->value.string.val;
        *out_len = lex->value.string.len;
        lex->value.string.val = NULL;
        lex->value.string.len = 0;
    }
    return result;
}

static int lex_init(lex_t *lex, get_func get, void *data)
{
    stream_init(&lex->stream, get, data);
    if(strbuffer_init(&lex->saved_text))
        return -1;

    lex->token = TOKEN_INVALID;
    return 0;
}

static void lex_clear(lex_t *lex)
{
    if(lex->token == TOKEN_STRING) {
        lex_free_string(lex);
    }
    else if(lex->token == TOKEN_BIGINTEGER) {
	json_context_t *ctx = jsonp_context();
	if(ctx->have_bigint)
	    ctx->bigint.delete_fn(lex->value.bigz, &ctx->memfuncs);
	lex->value.bigz = NULL;
    }
    else if(lex->token == TOKEN_BIGREAL) {
	json_context_t *ctx = jsonp_context();
	if(ctx->have_bigreal)
	    ctx->bigreal.delete_fn(lex->value.bigr, &ctx->memfuncs);
	lex->value.bigr = NULL;
    }
    strbuffer_clear(&lex->saved_text);
}

static void lex_close(lex_t *lex)
{
    lex_clear(lex);
    strbuffer_close(&lex->saved_text);
}


/*** parser ***/

static json_t *parse_value(lex_t *lex, size_t flags, json_error_t *error);

static json_t *parse_object(lex_t *lex, size_t flags, json_error_t *error)
{
    json_t *object = json_object();
    if(!object)
        return NULL;

    lex_scan(lex, flags, error);
    if(lex->token == '}')
        return object;

    while(1) {
        char *key;
        size_t len;
        json_t *value;

        if(lex->token != TOKEN_STRING) {
            error_set(error, lex, "string or '}' expected");
            goto error;
        }

        key = lex_steal_string(lex, &len);
        if(!key)
            return NULL;
        if (memchr(key, '\0', len)) {
            jsonp_free(key);
            error_set(error, lex, "NUL byte in object key not supported");
            goto error;
        }

        if(flags & JSON_REJECT_DUPLICATES) {
            if(json_object_get(object, key)) {
                jsonp_free(key);
                error_set(error, lex, "duplicate object key");
                goto error;
            }
        }

        lex_scan(lex, flags, error);
        if(lex->token != ':') {
            jsonp_free(key);
            error_set(error, lex, "':' expected");
            goto error;
        }

        lex_scan(lex, flags, error);
        value = parse_value(lex, flags, error);
        if(!value) {
            jsonp_free(key);
            goto error;
        }

        if(json_object_set_nocheck(object, key, value)) {
            jsonp_free(key);
            json_decref(value);
            goto error;
        }

        json_decref(value);
        jsonp_free(key);

        lex_scan(lex, flags, error);
        if(lex->token != ',')
            break;

        lex_scan(lex, flags, error);
    }

    if(lex->token != '}') {
        error_set(error, lex, "'}' expected");
        goto error;
    }

    return object;

error:
    json_decref(object);
    return NULL;
}

static json_t *parse_array(lex_t *lex, size_t flags, json_error_t *error)
{
    json_t *array = json_array();
    if(!array)
        return NULL;

    lex_scan(lex, flags, error);
    if(lex->token == ']')
        return array;

    while(lex->token) {
        json_t *elem = parse_value(lex, flags, error);
        if(!elem)
            goto error;

        if(json_array_append(array, elem)) {
            json_decref(elem);
            goto error;
        }
        json_decref(elem);

        lex_scan(lex, flags, error);
        if(lex->token != ',')
            break;

        lex_scan(lex, flags, error);
    }

    if(lex->token != ']') {
        error_set(error, lex, "']' expected");
        goto error;
    }

    return array;

error:
    json_decref(array);
    return NULL;
}

static json_t *parse_value(lex_t *lex, size_t flags, json_error_t *error)
{
    json_t *json;
    double value;

    switch(lex->token) {
        case TOKEN_STRING: {
            const char *value = lex->value.string.val;
            size_t len = lex->value.string.len;

            if(!(flags & JSON_ALLOW_NUL)) {
                if(memchr(value, '\0', len)) {
                    error_set(error, lex, "\\u0000 is not allowed without JSON_ALLOW_NUL");
                    return NULL;
                }
            }

            json = jsonp_stringn_nocheck_own(value, len);
            if(json) {
                lex->value.string.val = NULL;
                lex->value.string.len = 0;
            }
            break;
        }

        case TOKEN_INTEGER: {
            if (flags & JSON_DECODE_INT_AS_REAL) {
                if(jsonp_strtod(&lex->saved_text, &value)) {
                    error_set(error, lex, "real number overflow");
                    return NULL;
                }
                json = json_real(value);
            } else {
                json = json_integer(lex->value.integer);
            }
            break;
        }

        case TOKEN_BIGINTEGER: {
	    json = json_biginteger(lex->value.bigz);
	    break;
	}

        case TOKEN_BIGREAL: {
	    json = json_bigreal(lex->value.bigr);
	    break;
	}

        case TOKEN_REAL: {
            json = json_real(lex->value.real);
            break;
        }

        case TOKEN_TRUE:
            json = json_true();
            break;

        case TOKEN_FALSE:
            json = json_false();
            break;

        case TOKEN_NULL:
            json = json_null();
            break;

        case '{':
            json = parse_object(lex, flags, error);
            break;

        case '[':
            json = parse_array(lex, flags, error);
            break;

        case TOKEN_INVALID:
            error_set(error, lex, "invalid token");
            return NULL;

        default:
            error_set(error, lex, "unexpected token");
            return NULL;
    }

    if(!json)
        return NULL;

    return json;
}

static json_t *parse_json(lex_t *lex, size_t flags, json_error_t *error)
{
    json_context_t *ctx = jsonp_context();
    json_t *result;

    if(flags & JSON_USE_BIGINT_ALWAYS)
	flags |= JSON_USE_BIGINT;
    if(flags & JSON_USE_BIGREAL_ALWAYS)
	flags |= JSON_USE_BIGREAL;

    if((flags & JSON_USE_BIGINT) && !ctx->have_bigint) {
        error_set(error, lex,
                  "Programming error: Not prepared to decode big integers");
	return NULL;
    }
    if((flags & JSON_USE_BIGREAL) && !ctx->have_bigreal) {
        error_set(error, lex,
                  "Programming error: Not prepared to decode big reals");
	return NULL;
    }

    lex_scan(lex, flags, error);
    if(!(flags & JSON_DECODE_ANY)) {
        if(lex->token != '[' && lex->token != '{') {
            error_set(error, lex, "'[' or '{' expected");
            return NULL;
        }
    }

    result = parse_value(lex, flags, error);
    if(!result)
        return NULL;

    if(!(flags & JSON_DISABLE_EOF_CHECK)) {
        lex_scan(lex, flags, error);
        if(lex->token != TOKEN_EOF) {
            error_set(error, lex, "end of file expected");
            json_decref(result);
            return NULL;
        }
    }

    if(error) {
        /* Save the position even though there was no error */
        error->position = lex->stream.position;
    }

    return result;
}

typedef struct
{
    const char *data;
    int pos;
} string_data_t;

static int string_get(void *data)
{
    char c;
    string_data_t *stream = (string_data_t *)data;
    c = stream->data[stream->pos];
    if(c == '\0')
        return EOF;
    else
    {
        stream->pos++;
        return (unsigned char)c;
    }
}

json_t *json_loads(const char *string, size_t flags, json_error_t *error)
{
    lex_t lex;
    json_t *result;
    string_data_t stream_data;

    jsonp_error_init(error, "<string>");

    if (string == NULL) {
        error_set(error, NULL, "wrong arguments");
        return NULL;
    }

    stream_data.data = string;
    stream_data.pos = 0;

    if(lex_init(&lex, string_get, (void *)&stream_data))
        return NULL;

    result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

typedef struct
{
    const char *data;
    size_t len;
    size_t pos;
} buffer_data_t;

static int buffer_get(void *data)
{
    char c;
    buffer_data_t *stream = data;
    if(stream->pos >= stream->len)
      return EOF;

    c = stream->data[stream->pos];
    stream->pos++;
    return (unsigned char)c;
}

json_t *json_loadb(const char *buffer, size_t buflen, size_t flags, json_error_t *error)
{
    lex_t lex;
    json_t *result;
    buffer_data_t stream_data;

    jsonp_error_init(error, "<buffer>");

    if (buffer == NULL) {
        error_set(error, NULL, "wrong arguments");
        return NULL;
    }

    stream_data.data = buffer;
    stream_data.pos = 0;
    stream_data.len = buflen;

    if(lex_init(&lex, buffer_get, (void *)&stream_data))
        return NULL;

    result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

json_t *json_loadf(FILE *input, size_t flags, json_error_t *error)
{
    lex_t lex;
    const char *source;
    json_t *result;

    if(input == stdin)
        source = "<stdin>";
    else
        source = "<stream>";

    jsonp_error_init(error, source);

    if (input == NULL) {
        error_set(error, NULL, "wrong arguments");
        return NULL;
    }

    if(lex_init(&lex, (get_func)fgetc, input))
        return NULL;

    result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}

json_t *json_load_file(const char *path, size_t flags, json_error_t *error)
{
    json_t *result;
    FILE *fp;

    jsonp_error_init(error, path);

    if (path == NULL) {
        error_set(error, NULL, "wrong arguments");
        return NULL;
    }

    fp = fopen(path, "rb");
    if(!fp)
    {
        error_set(error, NULL, "unable to open %s: %s",
                  path, strerror(errno));
        return NULL;
    }

    result = json_loadf(fp, flags, error);

    fclose(fp);
    return result;
}

#define MAX_BUF_LEN 1024

typedef struct
{
    char data[MAX_BUF_LEN];
    size_t len;
    size_t pos;
    json_load_callback_t callback;
    void *arg;
} callback_data_t;

static int callback_get(void *data)
{
    char c;
    callback_data_t *stream = data;

    if(stream->pos >= stream->len) {
        stream->pos = 0;
        stream->len = stream->callback(stream->data, MAX_BUF_LEN, stream->arg);
        if(stream->len == 0 || stream->len == (size_t)-1)
            return EOF;
    }

    c = stream->data[stream->pos];
    stream->pos++;
    return (unsigned char)c;
}

json_t *json_load_callback(json_load_callback_t callback, void *arg, size_t flags, json_error_t *error)
{
    lex_t lex;
    json_t *result;

    callback_data_t stream_data;

    memset(&stream_data, 0, sizeof(stream_data));
    stream_data.callback = callback;
    stream_data.arg = arg;

    jsonp_error_init(error, "<callback>");

    if (callback == NULL) {
        error_set(error, NULL, "wrong arguments");
        return NULL;
    }

    if(lex_init(&lex, (get_func)callback_get, &stream_data))
        return NULL;

    result = parse_json(&lex, flags, error);

    lex_close(&lex);
    return result;
}
