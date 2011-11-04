/* json_bignum_ldbl.c
**
** Adds support to jansson for 'long double' support.
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>

#define JSON_BIGR_TYPE long double
#include <jansson.h>


/* REALS */

static int json_bigreal_ldbl_compare(json_bigr_const_t r1, json_bigr_const_t r2,
				     const json_memory_funcs_t *memfuncs)
{
    const long double * f1 = (const long double*) r1;
    const long double * f2 = (const long double*) r2;

    if( *f1 == *f2 )
	return 0;
    if( *f1 < *f2 )
	return -1;
    return 1;
}

static json_bigr_t json_bigreal_ldbl_copy(json_bigr_const_t r,
					  const json_memory_funcs_t *memfuncs)
{
    const long double * f1 = (const long double*) r;
    long double * f2;

    f2 = memfuncs->malloc_fn( sizeof(long double) );
    if(!f2)
	return NULL;
    *f2 = *f1;
    return f2;
}

static void json_bigreal_ldbl_delete(json_bigr_t r,
				     const json_memory_funcs_t *memfuncs)
{
    long double * f = r;

    memfuncs->free_fn( f );
    return;
}

static json_bigr_t json_bigreal_ldbl_from_real(double value,
					       const json_memory_funcs_t *memfuncs)
{
    long double * f;

    f = memfuncs->malloc_fn( sizeof(long double) );
    if(!f)
	return NULL;
    *f = value;
    return f;
}

static json_bigr_t json_bigreal_ldbl_from_str(const char *value,
					      const json_memory_funcs_t *memfuncs)
{
    long double f0;
    long double *f1;
    char* end;

    errno = 0;
    f0 = strtold( value, &end );

    f1 = json_bigreal_ldbl_copy( (json_bigr_const_t)&f0, memfuncs );
    memfuncs->overwrite_fn( &f0, sizeof(long double) );
    return f1;
}

static int json_bigreal_ldbl_to_str(json_bigr_const_t r, char *buffer, size_t size,
				    const json_memory_funcs_t *memfuncs)
{
    const long double *f = (const long double*) r;
    int outsize;

    outsize = snprintf( buffer, size, "%.*Lg", LDBL_DIG, *f );
    return outsize;
}

int json_use_ldbl_for_bigreals()
{
    static json_bigreal_funcs_t funcs;
    funcs.copy_fn        = json_bigreal_ldbl_copy;
    funcs.delete_fn      = json_bigreal_ldbl_delete;
    funcs.compare_fn     = json_bigreal_ldbl_compare;
    funcs.to_string_fn   = json_bigreal_ldbl_to_str;
    funcs.from_string_fn = json_bigreal_ldbl_from_str;
    funcs.from_real_fn   = json_bigreal_ldbl_from_real;

    json_set_bigreal_funcs( &funcs );
    return 0;
}
