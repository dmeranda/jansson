/* json_bignum_gmp.c
**
** Adds support to jansson for GNU Multiprecision Library (gmp)
**
** See http://gmplib.org/
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <gmp.h>

#define JSON_BIGZ_TYPE mpz_t
#define JSON_BIGR_TYPE mpf_t
#include <jansson.h>



/* INTEGERS */

static int json_bigint_gmp_compare(json_bigz_const_t i1, json_bigz_const_t i2,
				   const json_memory_funcs_t *memfuncs)
{
    const mpz_t * z1 = (const mpz_t*) i1;
    const mpz_t * z2 = (const mpz_t*) i2;

    return mpz_cmp( *z1, *z2 );
}

static json_bigz_t json_bigint_gmp_copy(json_bigz_const_t i,
					const json_memory_funcs_t *memfuncs)
{
    const mpz_t * z1 = (const mpz_t*) i;
    mpz_t * z2;

    z2 = memfuncs->malloc_fn( sizeof(mpz_t) );
    if(!z2)
	return NULL;

    mpz_init_set( *z2, *z1 );
    return z2;
}

static void json_bigint_gmp_delete(json_bigz_t i,
				   const json_memory_funcs_t *memfuncs)
{
    mpz_t * z = (mpz_t*) i;

    mpz_clear( *z );
    memfuncs->free_fn( z );
    return;
}

static json_bigz_t json_bigint_gmp_from_str(const char *value,
					    const json_memory_funcs_t *memfuncs)
{
    mpz_t z0;
    mpz_t * z1;
    int rc;

    /* Parse into local variable first, then copy, so on an error we
     * won't leak memory by not having the free_fn function available.
     */
    rc = mpz_init_set_str( z0, value, 10 );
    if( rc != 0 ) {
	mpz_clear( z0 );
	return NULL;
    }

    z1 = json_bigint_gmp_copy( (json_bigz_const_t)&z0, memfuncs );
    mpz_clear( z0 );
    memfuncs->overwrite_fn( &z0, sizeof(z0) );

    return z1;
}

static int json_bigint_gmp_to_str(json_bigz_const_t i, char *buffer, size_t size,
				  const json_memory_funcs_t *memfuncs)
{
    const mpz_t *z = (const mpz_t*) i;
    int outsize;

    outsize = gmp_snprintf( buffer, size, "%.Zd", *z );
    return outsize;
}

static json_bigz_t json_bigint_gmp_from_int(json_int_t value,
					    const json_memory_funcs_t *memfuncs)
{
    /* GMP supports direct 'long' conversion, but not 'long long' */
#if JSON_INTEGER_IS_LONG_LONG
    size_t size;
    char buffer[100];

    size = snprintf(buffer, sizeof(buffer),
		    "%" JSON_INTEGER_FORMAT,
		    value);
    if( size >= sizeof(buffer) ) {
	memfuncs->overwrite_fn( buffer, sizeof(buffer) );
	return NULL;
    }
    return json_bigint_gmp_from_str(buffer, memfuncs);
#else
    mpz_t * z;
    z = memfuncs->malloc_fn( sizeof(mpz_t) );
    if(!z)
	return NULL;
    mpz_init_set_si( *z, (signed long) value );
    return z;
#endif
}

int json_use_gmp_for_bigintegers()
{
    static json_bigint_funcs_t funcs;
    funcs.copy_fn        = json_bigint_gmp_copy;
    funcs.delete_fn      = json_bigint_gmp_delete;
    funcs.compare_fn     = json_bigint_gmp_compare;
    funcs.to_string_fn   = json_bigint_gmp_to_str;
    funcs.from_string_fn = json_bigint_gmp_from_str;
    funcs.from_int_fn    = json_bigint_gmp_from_int;

    json_set_biginteger_funcs( &funcs );
    return 0;
}


/* REALS */

static int json_bigreal_gmp_compare(json_bigr_const_t r1, json_bigr_const_t r2,
				    const json_memory_funcs_t *memfuncs)
{
    const mpf_t * f1 = (const mpf_t*) r1;
    const mpf_t * f2 = (const mpf_t*) r2;

    return mpf_cmp( *f1, *f2 );
}

static json_bigr_t json_bigreal_gmp_copy(json_bigr_const_t r,
					 const json_memory_funcs_t *memfuncs)
{
    const mpf_t * f1 = (const mpf_t*) r;
    mpf_t * f2;

    f2 = memfuncs->malloc_fn( sizeof(mpf_t) );
    if(!f2)
	return NULL;

    mpf_init_set( *f2, *f1 );
    return f2;
}

static void json_bigreal_gmp_delete(json_bigr_t r,
				    const json_memory_funcs_t *memfuncs)
{
    mpf_t * f = r;

    mpf_clear( *f );
    memfuncs->free_fn( f );
    return;
}

static json_bigr_t json_bigreal_gmp_from_real(double value,
					     const json_memory_funcs_t *memfuncs)
{
    mpf_t f0;
    mpf_t * f1;
    int rc;

    mpf_init_set_d( f0, value );

    f1 = json_bigreal_gmp_copy( (json_bigr_const_t)&f0, memfuncs );
    mpf_clear( f0 );
    memfuncs->overwrite_fn( &f0, sizeof(f0) );
    return f1;
}

static json_bigr_t json_bigreal_gmp_from_str(const char *value,
					     const json_memory_funcs_t *memfuncs)
{
    mpf_t f0;
    mpf_t * f1;
    int rc;

    /* Parse into local variable first, then copy, so on an error we
     * won't leak memory by not having the free_fn function available.
     */
    rc = mpf_init_set_str( f0, value, 10 );
    if( rc != 0 ) {
	mpf_clear( f0 );
	return NULL;
    }

    f1 = json_bigreal_gmp_copy( (json_bigr_const_t)&f0, memfuncs );
    mpf_clear( f0 );
    memfuncs->overwrite_fn( &f0, sizeof(f0) );
    return f1;
}

static int json_bigreal_gmp_to_str(json_bigr_const_t r, char *buffer, size_t size,
				   const json_memory_funcs_t *memfuncs)
{
    const mpf_t *f = (const mpf_t*) r;
    int outsize;

    outsize = gmp_snprintf( buffer, size, "%.Fg", *f );
    return outsize;
}

int json_use_gmp_for_bigreals()
{
    static json_bigreal_funcs_t funcs;
    funcs.copy_fn        = json_bigreal_gmp_copy;
    funcs.delete_fn      = json_bigreal_gmp_delete;
    funcs.compare_fn     = json_bigreal_gmp_compare;
    funcs.to_string_fn   = json_bigreal_gmp_to_str;
    funcs.from_string_fn = json_bigreal_gmp_from_str;
    funcs.from_real_fn   = json_bigreal_gmp_from_real;

    json_set_bigreal_funcs( &funcs );
    return 0;
}
