/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared crypto simd helpers
 */

#ifndef _CRYPTO_INTERNAL_SIMD_H
#define _CRYPTO_INTERNAL_SIMD_H

#include <asm/simd.h>
#include <linux/percpu.h>
#include <linux/types.h>

/* skcipher support */

struct simd_skcipher_alg;
struct skcipher_alg;

struct simd_skcipher_alg *simd_skcipher_create_compat(struct skcipher_alg *ialg,
						      const char *algname,
						      const char *drvname,
						      const char *basename);
void simd_skcipher_free(struct simd_skcipher_alg *alg);

int simd_register_skciphers_compat(struct skcipher_alg *algs, int count,
				   struct simd_skcipher_alg **simd_algs);

void simd_unregister_skciphers(struct skcipher_alg *algs, int count,
			       struct simd_skcipher_alg **simd_algs);

/* AEAD support */

struct simd_aead_alg;
struct aead_alg;

int simd_register_aeads_compat(struct aead_alg *algs, int count,
			       struct simd_aead_alg **simd_algs);

void simd_unregister_aeads(struct aead_alg *algs, int count,
			   struct simd_aead_alg **simd_algs);

/*
 * crypto_simd_usable() - is it allowed at this time to use SIMD instructions or
 *			  access the SIMD register file?
 *
 * This delegates to may_use_simd(), except that this also returns false if SIMD
 * in crypto code has been temporarily disabled on this CPU by the crypto
 * self-tests, in order to test the no-SIMD fallback code.
 */
#ifdef CONFIG_CRYPTO_SELFTESTS
DECLARE_PER_CPU(bool, crypto_simd_disabled_for_test);
#define crypto_simd_usable() \
	(may_use_simd() && !this_cpu_read(crypto_simd_disabled_for_test))
#else
#define crypto_simd_usable() may_use_simd()
#endif

#endif /* _CRYPTO_INTERNAL_SIMD_H */
