/* json_bignum_openssl.c
**
** Adds support to jansson for OpenSSL big numbers (BN)
**
** See http://www.openssl.org/docs/crypto/bn.html
*/

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/bn.h>

#define JSON_BIGZ_TYPE BIGNUM
#include <jansson.h>



/* INTEGERS */

static int json_bigint_openssl_compare(json_bigz_const_t i1, json_bigz_const_t i2,
				       const json_memory_funcs_t *memfuncs)
{
    const BIGNUM * z1 = (const BIGNUM*) i1;
    const BIGNUM * z2 = (const BIGNUM*) i2;

    return BN_cmp( z1, z2 );
}

static json_bigz_t json_bigint_openssl_copy(json_bigz_const_t i,
					    const json_memory_funcs_t *memfuncs)
{
    const BIGNUM * z1 = (const BIGNUM*) i;
    BIGNUM * z2;

    z2 = BN_dup( z1 );
    return z2;
}

static void json_bigint_openssl_delete(json_bigz_t i,
				       const json_memory_funcs_t *memfuncs)
{
    BIGNUM * z = (BIGNUM*) i;

    BN_clear_free( z );
    return;
}

static json_bigz_t json_bigint_openssl_from_int(json_int_t value,
						const json_memory_funcs_t *memfuncs)
{
    BIGNUM * z;
    size_t size;
    char buffer[100];

    size = snprintf(buffer, sizeof(buffer),
		    "%" JSON_INTEGER_FORMAT,
		    value );
    if( size >= sizeof(buffer) )
	return NULL;

    z = BN_new();
    BN_init(z);
    if( BN_dec2bn( &z, buffer ) == 0 ) {
	BN_clear_free(z);
	return NULL;
    }
    return z;
}

static json_bigz_t json_bigint_openssl_from_str(const char *value,
						const json_memory_funcs_t *memfuncs)
{
    BIGNUM * z;

    z = BN_new();
    BN_init(z);
    if( BN_dec2bn( &z, value ) == 0 ) {
	BN_clear_free(z);
	return NULL;
    }
    return z;
}

static int json_bigint_openssl_to_str(json_bigz_const_t i,
				      char *buffer, size_t size,
				      const json_memory_funcs_t *memfuncs)
{
    const BIGNUM *z = (const BIGNUM*) i;
    char *buf;
    size_t outsize;
    size_t copysize;
 
    buf = BN_bn2dec(z);
    outsize = strlen(buf);
    if( outsize <= size )
	copysize = outsize;
    else
	copysize = size - 1;

    memcpy( buffer, buf, copysize );
    buffer[copysize] = '\0';

    OPENSSL_free(buf);
    return outsize;
}

int json_use_openssl_for_bigintegers()
{
    static json_bigint_funcs_t funcs;
    funcs.copy_fn        = json_bigint_openssl_copy;
    funcs.compare_fn     = json_bigint_openssl_compare;
    funcs.delete_fn      = json_bigint_openssl_delete;
    funcs.to_string_fn   = json_bigint_openssl_to_str;
    funcs.from_string_fn = json_bigint_openssl_from_str;
    funcs.from_int_fn    = json_bigint_openssl_from_int;

    json_set_biginteger_funcs( &funcs );
    return 0;
}
