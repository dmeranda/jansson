/* json_bignum_quad.c
**
** Adds support to jansson for libquadmath, which is part of GCC 4.6+ on some platforms.
**
** See http://gcc.gnu.org/onlinedocs/libquadmath/
*/
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>

#include <quadmath.h>

#define JSON_BIGR_TYPE __float128
#include <jansson.h>

/* REALS */

static int json_bigreal_quad_compare(json_bigr_const_t r1, json_bigr_const_t r2,
				     const json_memory_funcs_t *memfuncs)
{
    const __float128 * f1 = (const __float128*) r1;
    const __float128 * f2 = (const __float128*) r2;

    if( *f1 == *f2 )
	return 0;
    if( *f1 < *f2 )
	return -1;
    return 1;
}

static json_bigr_t json_bigreal_quad_copy(json_bigr_const_t r,
					  const json_memory_funcs_t *memfuncs)
{
    const __float128 * f1 = (const __float128*) r;
    __float128 * f2;

    f2 = memfuncs->malloc_fn( sizeof(__float128) );
    if(!f2)
	return NULL;
    *f2 = *f1;
    return f2;
}

static void json_bigreal_quad_delete(json_bigr_t r,
				     const json_memory_funcs_t *memfuncs)
{
    __float128 * f = r;

    memfuncs->free_fn( f );
    return;
}

static json_bigr_t json_bigreal_quad_from_real(double value,
					       const json_memory_funcs_t *memfuncs)
{
    __float128 * f;

    f = memfuncs->malloc_fn( sizeof(__float128) );
    if(!f)
	return NULL;
    *f = value;
    return f;
}

static json_bigr_t json_bigreal_quad_from_str(const char *value,
					      const json_memory_funcs_t *memfuncs)
{
    __float128 f0;
    __float128 *f1;
    char* end;

    errno = 0;
    f0 = strtoflt128( value, &end );

    f1 = json_bigreal_quad_copy( (json_bigr_const_t)&f0, memfuncs );
    return f1;
}

static int json_bigreal_quad_to_str(json_bigr_const_t r, char *buffer, size_t size,
				    const json_memory_funcs_t *memfuncs)
{
    const __float128 *f = (const __float128*) r;
    int outsize;

    outsize = quadmath_snprintf( buffer, size, "%.*Qg", FLT128_DIG, *f );
    return outsize;
}

int json_use_quad_for_bigreals()
{
    static json_bigreal_funcs_t funcs;
    funcs.copy_fn        = json_bigreal_quad_copy;
    funcs.delete_fn      = json_bigreal_quad_delete;
    funcs.compare_fn     = json_bigreal_quad_compare;
    funcs.to_string_fn   = json_bigreal_quad_to_str;
    funcs.from_string_fn = json_bigreal_quad_from_str;
    funcs.from_real_fn   = json_bigreal_quad_from_real;

    json_set_bigreal_funcs( &funcs );
    return 0;
}
