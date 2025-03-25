// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Symmetric key cipher operations.
 *
 * Generic encrypt/decrypt wrapper for ciphers, handles operations across
 * multiple page boundaries by using temporary blocks.  In user context,
 * the kernel is given a chance to schedule us once per page.
 *
 * Copyright (c) 2015 Herbert Xu <herbert@gondor.apana.org.au>
 */

#include <crypto/internal/aead.h>
#include <crypto/internal/cipher.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/bug.h>
#include <linux/cryptouser.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <net/netlink.h>
#include "skcipher.h"

#define CRYPTO_ALG_TYPE_SKCIPHER_MASK	0x0000000e

enum {
	SKCIPHER_WALK_SLOW = 1 << 0,
	SKCIPHER_WALK_COPY = 1 << 1,
	SKCIPHER_WALK_DIFF = 1 << 2,
	SKCIPHER_WALK_SLEEP = 1 << 3,
};

static const struct crypto_type crypto_skcipher_type;

static int skcipher_walk_next(struct skcipher_walk *walk);

static inline void skcipher_map_src(struct skcipher_walk *walk)
{
	walk->src.virt.addr = scatterwalk_map(&walk->in);
}

static inline void skcipher_map_dst(struct skcipher_walk *walk)
{
	walk->dst.virt.addr = scatterwalk_map(&walk->out);
}

static inline void skcipher_unmap_src(struct skcipher_walk *walk)
{
	scatterwalk_unmap(walk->src.virt.addr);
}

static inline void skcipher_unmap_dst(struct skcipher_walk *walk)
{
	scatterwalk_unmap(walk->dst.virt.addr);
}

static inline gfp_t skcipher_walk_gfp(struct skcipher_walk *walk)
{
	return walk->flags & SKCIPHER_WALK_SLEEP ? GFP_KERNEL : GFP_ATOMIC;
}

static inline struct skcipher_alg *__crypto_skcipher_alg(
	struct crypto_alg *alg)
{
	return container_of(alg, struct skcipher_alg, base);
}

static int skcipher_done_slow(struct skcipher_walk *walk, unsigned int bsize)
{
	u8 *addr = PTR_ALIGN(walk->buffer, walk->alignmask + 1);

	scatterwalk_copychunks(addr, &walk->out, bsize, 1);
	return 0;
}

/**
 * skcipher_walk_done() - finish one step of a skcipher_walk
 * @walk: the skcipher_walk
 * @res: number of bytes *not* processed (>= 0) from walk->nbytes,
 *	 or a -errno value to terminate the walk due to an error
 *
 * This function cleans up after one step of walking through the source and
 * destination scatterlists, and advances to the next step if applicable.
 * walk->nbytes is set to the number of bytes available in the next step,
 * walk->total is set to the new total number of bytes remaining, and
 * walk->{src,dst}.virt.addr is set to the next pair of data pointers.  If there
 * is no more data, or if an error occurred (i.e. -errno return), then
 * walk->nbytes and walk->total are set to 0 and all resources owned by the
 * skcipher_walk are freed.
 *
 * Return: 0 or a -errno value.  If @res was a -errno value then it will be
 *	   returned, but other errors may occur too.
 */
int skcipher_walk_done(struct skcipher_walk *walk, int res)
{
	unsigned int n = walk->nbytes; /* num bytes processed this step */
	unsigned int total = 0; /* new total remaining */

	if (!n)
		goto finish;

	if (likely(res >= 0)) {
		n -= res; /* subtract num bytes *not* processed */
		total = walk->total - n;
	}

	if (likely(!(walk->flags & (SKCIPHER_WALK_SLOW |
				    SKCIPHER_WALK_COPY |
				    SKCIPHER_WALK_DIFF)))) {
unmap_src:
		skcipher_unmap_src(walk);
	} else if (walk->flags & SKCIPHER_WALK_DIFF) {
		skcipher_unmap_dst(walk);
		goto unmap_src;
	} else if (walk->flags & SKCIPHER_WALK_COPY) {
		skcipher_map_dst(walk);
		memcpy(walk->dst.virt.addr, walk->page, n);
		skcipher_unmap_dst(walk);
	} else { /* SKCIPHER_WALK_SLOW */
		if (res > 0) {
			/*
			 * Didn't process all bytes.  Either the algorithm is
			 * broken, or this was the last step and it turned out
			 * the message wasn't evenly divisible into blocks but
			 * the algorithm requires it.
			 */
			res = -EINVAL;
			total = 0;
		} else
			n = skcipher_done_slow(walk, n);
	}

	if (res > 0)
		res = 0;

	walk->total = total;
	walk->nbytes = 0;

	scatterwalk_advance(&walk->in, n);
	scatterwalk_advance(&walk->out, n);
	scatterwalk_done(&walk->in, 0, total);
	scatterwalk_done(&walk->out, 1, total);

	if (total) {
		if (walk->flags & SKCIPHER_WALK_SLEEP)
			cond_resched();
		walk->flags &= ~(SKCIPHER_WALK_SLOW | SKCIPHER_WALK_COPY |
				 SKCIPHER_WALK_DIFF);
		return skcipher_walk_next(walk);
	}

finish:
	/* Short-circuit for the common/fast path. */
	if (!((unsigned long)walk->buffer | (unsigned long)walk->page))
		goto out;

	if (walk->iv != walk->oiv)
		memcpy(walk->oiv, walk->iv, walk->ivsize);
	if (walk->buffer != walk->page)
		kfree(walk->buffer);
	if (walk->page)
		free_page((unsigned long)walk->page);

out:
	return res;
}
EXPORT_SYMBOL_GPL(skcipher_walk_done);

static int skcipher_next_slow(struct skcipher_walk *walk, unsigned int bsize)
{
	unsigned alignmask = walk->alignmask;
	unsigned n;
	u8 *buffer;

	if (!walk->buffer)
		walk->buffer = walk->page;
	buffer = walk->buffer;
	if (!buffer) {
		/* Min size for a buffer of bsize bytes aligned to alignmask */
		n = bsize + (alignmask & ~(crypto_tfm_ctx_alignment() - 1));

		buffer = kzalloc(n, skcipher_walk_gfp(walk));
		if (!buffer)
			return skcipher_walk_done(walk, -ENOMEM);
		walk->buffer = buffer;
	}
	walk->dst.virt.addr = PTR_ALIGN(buffer, alignmask + 1);
	walk->src.virt.addr = walk->dst.virt.addr;

	scatterwalk_copychunks(walk->src.virt.addr, &walk->in, bsize, 0);

	walk->nbytes = bsize;
	walk->flags |= SKCIPHER_WALK_SLOW;

	return 0;
}

static int skcipher_next_copy(struct skcipher_walk *walk)
{
	u8 *tmp = walk->page;

	skcipher_map_src(walk);
	memcpy(tmp, walk->src.virt.addr, walk->nbytes);
	skcipher_unmap_src(walk);

	walk->src.virt.addr = tmp;
	walk->dst.virt.addr = tmp;
	return 0;
}

static int skcipher_next_fast(struct skcipher_walk *walk)
{
	unsigned long diff;

	diff = offset_in_page(walk->in.offset) -
	       offset_in_page(walk->out.offset);
	diff |= (u8 *)scatterwalk_page(&walk->in) -
		(u8 *)scatterwalk_page(&walk->out);

	skcipher_map_src(walk);
	walk->dst.virt.addr = walk->src.virt.addr;

	if (diff) {
		walk->flags |= SKCIPHER_WALK_DIFF;
		skcipher_map_dst(walk);
	}

	return 0;
}

static int skcipher_walk_next(struct skcipher_walk *walk)
{
	unsigned int bsize;
	unsigned int n;

	n = walk->total;
	bsize = min(walk->stride, max(n, walk->blocksize));
	n = scatterwalk_clamp(&walk->in, n);
	n = scatterwalk_clamp(&walk->out, n);

	if (unlikely(n < bsize)) {
		if (unlikely(walk->total < walk->blocksize))
			return skcipher_walk_done(walk, -EINVAL);

slow_path:
		return skcipher_next_slow(walk, bsize);
	}
	walk->nbytes = n;

	if (unlikely((walk->in.offset | walk->out.offset) & walk->alignmask)) {
		if (!walk->page) {
			gfp_t gfp = skcipher_walk_gfp(walk);

			walk->page = (void *)__get_free_page(gfp);
			if (!walk->page)
				goto slow_path;
		}
		walk->flags |= SKCIPHER_WALK_COPY;
		return skcipher_next_copy(walk);
	}

	return skcipher_next_fast(walk);
}

static int skcipher_copy_iv(struct skcipher_walk *walk)
{
	unsigned alignmask = walk->alignmask;
	unsigned ivsize = walk->ivsize;
	unsigned aligned_stride = ALIGN(walk->stride, alignmask + 1);
	unsigned size;
	u8 *iv;

	/* Min size for a buffer of stride + ivsize, aligned to alignmask */
	size = aligned_stride + ivsize +
	       (alignmask & ~(crypto_tfm_ctx_alignment() - 1));

	walk->buffer = kmalloc(size, skcipher_walk_gfp(walk));
	if (!walk->buffer)
		return -ENOMEM;

	iv = PTR_ALIGN(walk->buffer, alignmask + 1) + aligned_stride;

	walk->iv = memcpy(iv, walk->iv, walk->ivsize);
	return 0;
}

static int skcipher_walk_first(struct skcipher_walk *walk)
{
	if (WARN_ON_ONCE(in_hardirq()))
		return -EDEADLK;

	walk->buffer = NULL;
	if (unlikely(((unsigned long)walk->iv & walk->alignmask))) {
		int err = skcipher_copy_iv(walk);
		if (err)
			return err;
	}

	walk->page = NULL;

	return skcipher_walk_next(walk);
}

int skcipher_walk_virt(struct skcipher_walk *walk,
		       struct skcipher_request *req, bool atomic)
{
	const struct skcipher_alg *alg =
		crypto_skcipher_alg(crypto_skcipher_reqtfm(req));

	might_sleep_if(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP);

	walk->total = req->cryptlen;
	walk->nbytes = 0;
	walk->iv = req->iv;
	walk->oiv = req->iv;
	if ((req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) && !atomic)
		walk->flags = SKCIPHER_WALK_SLEEP;
	else
		walk->flags = 0;

	if (unlikely(!walk->total))
		return 0;

	scatterwalk_start(&walk->in, req->src);
	scatterwalk_start(&walk->out, req->dst);

	/*
	 * Accessing 'alg' directly generates better code than using the
	 * crypto_skcipher_blocksize() and similar helper functions here, as it
	 * prevents the algorithm pointer from being repeatedly reloaded.
	 */
	walk->blocksize = alg->base.cra_blocksize;
	walk->ivsize = alg->co.ivsize;
	walk->alignmask = alg->base.cra_alignmask;

	if (alg->co.base.cra_type != &crypto_skcipher_type)
		walk->stride = alg->co.chunksize;
	else
		walk->stride = alg->walksize;

	return skcipher_walk_first(walk);
}
EXPORT_SYMBOL_GPL(skcipher_walk_virt);

static int skcipher_walk_aead_common(struct skcipher_walk *walk,
				     struct aead_request *req, bool atomic)
{
	const struct aead_alg *alg = crypto_aead_alg(crypto_aead_reqtfm(req));

	walk->nbytes = 0;
	walk->iv = req->iv;
	walk->oiv = req->iv;
	if ((req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) && !atomic)
		walk->flags = SKCIPHER_WALK_SLEEP;
	else
		walk->flags = 0;

	if (unlikely(!walk->total))
		return 0;

	scatterwalk_start(&walk->in, req->src);
	scatterwalk_start(&walk->out, req->dst);

	scatterwalk_copychunks(NULL, &walk->in, req->assoclen, 2);
	scatterwalk_copychunks(NULL, &walk->out, req->assoclen, 2);

	scatterwalk_done(&walk->in, 0, walk->total);
	scatterwalk_done(&walk->out, 0, walk->total);

	/*
	 * Accessing 'alg' directly generates better code than using the
	 * crypto_aead_blocksize() and similar helper functions here, as it
	 * prevents the algorithm pointer from being repeatedly reloaded.
	 */
	walk->blocksize = alg->base.cra_blocksize;
	walk->stride = alg->chunksize;
	walk->ivsize = alg->ivsize;
	walk->alignmask = alg->base.cra_alignmask;

	return skcipher_walk_first(walk);
}

int skcipher_walk_aead_encrypt(struct skcipher_walk *walk,
			       struct aead_request *req, bool atomic)
{
	walk->total = req->cryptlen;

	return skcipher_walk_aead_common(walk, req, atomic);
}
EXPORT_SYMBOL_GPL(skcipher_walk_aead_encrypt);

int skcipher_walk_aead_decrypt(struct skcipher_walk *walk,
			       struct aead_request *req, bool atomic)
{
	struct crypto_aead *tfm = crypto_aead_reqtfm(req);

	walk->total = req->cryptlen - crypto_aead_authsize(tfm);

	return skcipher_walk_aead_common(walk, req, atomic);
}
EXPORT_SYMBOL_GPL(skcipher_walk_aead_decrypt);

static void skcipher_set_needkey(struct crypto_skcipher *tfm)
{
	if (crypto_skcipher_max_keysize(tfm) != 0)
		crypto_skcipher_set_flags(tfm, CRYPTO_TFM_NEED_KEY);
}

static int skcipher_setkey_unaligned(struct crypto_skcipher *tfm,
				     const u8 *key, unsigned int keylen)
{
	unsigned long alignmask = crypto_skcipher_alignmask(tfm);
	struct skcipher_alg *cipher = crypto_skcipher_alg(tfm);
	u8 *buffer, *alignbuffer;
	unsigned long absize;
	int ret;

	absize = keylen + alignmask;
	buffer = kmalloc(absize, GFP_ATOMIC);
	if (!buffer)
		return -ENOMEM;

	alignbuffer = (u8 *)ALIGN((unsigned long)buffer, alignmask + 1);
	memcpy(alignbuffer, key, keylen);
	ret = cipher->setkey(tfm, alignbuffer, keylen);
	kfree_sensitive(buffer);
	return ret;
}

int crypto_skcipher_setkey(struct crypto_skcipher *tfm, const u8 *key,
			   unsigned int keylen)
{
	struct skcipher_alg *cipher = crypto_skcipher_alg(tfm);
	unsigned long alignmask = crypto_skcipher_alignmask(tfm);
	int err;

	if (cipher->co.base.cra_type != &crypto_skcipher_type) {
		struct crypto_lskcipher **ctx = crypto_skcipher_ctx(tfm);

		crypto_lskcipher_clear_flags(*ctx, CRYPTO_TFM_REQ_MASK);
		crypto_lskcipher_set_flags(*ctx,
					   crypto_skcipher_get_flags(tfm) &
					   CRYPTO_TFM_REQ_MASK);
		err = crypto_lskcipher_setkey(*ctx, key, keylen);
		goto out;
	}

	if (keylen < cipher->min_keysize || keylen > cipher->max_keysize)
		return -EINVAL;

	if ((unsigned long)key & alignmask)
		err = skcipher_setkey_unaligned(tfm, key, keylen);
	else
		err = cipher->setkey(tfm, key, keylen);

out:
	if (unlikely(err)) {
		skcipher_set_needkey(tfm);
		return err;
	}

	crypto_skcipher_clear_flags(tfm, CRYPTO_TFM_NEED_KEY);
	return 0;
}
EXPORT_SYMBOL_GPL(crypto_skcipher_setkey);

int crypto_skcipher_encrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);

	if (crypto_skcipher_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		return -ENOKEY;
	if (alg->co.base.cra_type != &crypto_skcipher_type)
		return crypto_lskcipher_encrypt_sg(req);
	return alg->encrypt(req);
}
EXPORT_SYMBOL_GPL(crypto_skcipher_encrypt);

int crypto_skcipher_decrypt(struct skcipher_request *req)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);

	if (crypto_skcipher_get_flags(tfm) & CRYPTO_TFM_NEED_KEY)
		return -ENOKEY;
	if (alg->co.base.cra_type != &crypto_skcipher_type)
		return crypto_lskcipher_decrypt_sg(req);
	return alg->decrypt(req);
}
EXPORT_SYMBOL_GPL(crypto_skcipher_decrypt);

static int crypto_lskcipher_export(struct skcipher_request *req, void *out)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	u8 *ivs = skcipher_request_ctx(req);

	ivs = PTR_ALIGN(ivs, crypto_skcipher_alignmask(tfm) + 1);

	memcpy(out, ivs + crypto_skcipher_ivsize(tfm),
	       crypto_skcipher_statesize(tfm));

	return 0;
}

static int crypto_lskcipher_import(struct skcipher_request *req, const void *in)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	u8 *ivs = skcipher_request_ctx(req);

	ivs = PTR_ALIGN(ivs, crypto_skcipher_alignmask(tfm) + 1);

	memcpy(ivs + crypto_skcipher_ivsize(tfm), in,
	       crypto_skcipher_statesize(tfm));

	return 0;
}

static int skcipher_noexport(struct skcipher_request *req, void *out)
{
	return 0;
}

static int skcipher_noimport(struct skcipher_request *req, const void *in)
{
	return 0;
}

int crypto_skcipher_export(struct skcipher_request *req, void *out)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);

	if (alg->co.base.cra_type != &crypto_skcipher_type)
		return crypto_lskcipher_export(req, out);
	return alg->export(req, out);
}
EXPORT_SYMBOL_GPL(crypto_skcipher_export);

int crypto_skcipher_import(struct skcipher_request *req, const void *in)
{
	struct crypto_skcipher *tfm = crypto_skcipher_reqtfm(req);
	struct skcipher_alg *alg = crypto_skcipher_alg(tfm);

	if (alg->co.base.cra_type != &crypto_skcipher_type)
		return crypto_lskcipher_import(req, in);
	return alg->import(req, in);
}
EXPORT_SYMBOL_GPL(crypto_skcipher_import);

static void crypto_skcipher_exit_tfm(struct crypto_tfm *tfm)
{
	struct crypto_skcipher *skcipher = __crypto_skcipher_cast(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(skcipher);

	alg->exit(skcipher);
}

static int crypto_skcipher_init_tfm(struct crypto_tfm *tfm)
{
	struct crypto_skcipher *skcipher = __crypto_skcipher_cast(tfm);
	struct skcipher_alg *alg = crypto_skcipher_alg(skcipher);

	skcipher_set_needkey(skcipher);

	if (tfm->__crt_alg->cra_type != &crypto_skcipher_type) {
		unsigned am = crypto_skcipher_alignmask(skcipher);
		unsigned reqsize;

		reqsize = am & ~(crypto_tfm_ctx_alignment() - 1);
		reqsize += crypto_skcipher_ivsize(skcipher);
		reqsize += crypto_skcipher_statesize(skcipher);
		crypto_skcipher_set_reqsize(skcipher, reqsize);

		return crypto_init_lskcipher_ops_sg(tfm);
	}

	if (alg->exit)
		skcipher->base.exit = crypto_skcipher_exit_tfm;

	if (alg->init)
		return alg->init(skcipher);

	return 0;
}

static unsigned int crypto_skcipher_extsize(struct crypto_alg *alg)
{
	if (alg->cra_type != &crypto_skcipher_type)
		return sizeof(struct crypto_lskcipher *);

	return crypto_alg_extsize(alg);
}

static void crypto_skcipher_free_instance(struct crypto_instance *inst)
{
	struct skcipher_instance *skcipher =
		container_of(inst, struct skcipher_instance, s.base);

	skcipher->free(skcipher);
}

static void crypto_skcipher_show(struct seq_file *m, struct crypto_alg *alg)
	__maybe_unused;
static void crypto_skcipher_show(struct seq_file *m, struct crypto_alg *alg)
{
	struct skcipher_alg *skcipher = __crypto_skcipher_alg(alg);

	seq_printf(m, "type         : skcipher\n");
	seq_printf(m, "async        : %s\n",
		   alg->cra_flags & CRYPTO_ALG_ASYNC ?  "yes" : "no");
	seq_printf(m, "blocksize    : %u\n", alg->cra_blocksize);
	seq_printf(m, "min keysize  : %u\n", skcipher->min_keysize);
	seq_printf(m, "max keysize  : %u\n", skcipher->max_keysize);
	seq_printf(m, "ivsize       : %u\n", skcipher->ivsize);
	seq_printf(m, "chunksize    : %u\n", skcipher->chunksize);
	seq_printf(m, "walksize     : %u\n", skcipher->walksize);
	seq_printf(m, "statesize    : %u\n", skcipher->statesize);
}

static int __maybe_unused crypto_skcipher_report(
	struct sk_buff *skb, struct crypto_alg *alg)
{
	struct skcipher_alg *skcipher = __crypto_skcipher_alg(alg);
	struct crypto_report_blkcipher rblkcipher;

	memset(&rblkcipher, 0, sizeof(rblkcipher));

	strscpy(rblkcipher.type, "skcipher", sizeof(rblkcipher.type));
	strscpy(rblkcipher.geniv, "<none>", sizeof(rblkcipher.geniv));

	rblkcipher.blocksize = alg->cra_blocksize;
	rblkcipher.min_keysize = skcipher->min_keysize;
	rblkcipher.max_keysize = skcipher->max_keysize;
	rblkcipher.ivsize = skcipher->ivsize;

	return nla_put(skb, CRYPTOCFGA_REPORT_BLKCIPHER,
		       sizeof(rblkcipher), &rblkcipher);
}

static const struct crypto_type crypto_skcipher_type = {
	.extsize = crypto_skcipher_extsize,
	.init_tfm = crypto_skcipher_init_tfm,
	.free = crypto_skcipher_free_instance,
#ifdef CONFIG_PROC_FS
	.show = crypto_skcipher_show,
#endif
#if IS_ENABLED(CONFIG_CRYPTO_USER)
	.report = crypto_skcipher_report,
#endif
	.maskclear = ~CRYPTO_ALG_TYPE_MASK,
	.maskset = CRYPTO_ALG_TYPE_SKCIPHER_MASK,
	.type = CRYPTO_ALG_TYPE_SKCIPHER,
	.tfmsize = offsetof(struct crypto_skcipher, base),
};

int crypto_grab_skcipher(struct crypto_skcipher_spawn *spawn,
			 struct crypto_instance *inst,
			 const char *name, u32 type, u32 mask)
{
	spawn->base.frontend = &crypto_skcipher_type;
	return crypto_grab_spawn(&spawn->base, inst, name, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_grab_skcipher);

struct crypto_skcipher *crypto_alloc_skcipher(const char *alg_name,
					      u32 type, u32 mask)
{
	return crypto_alloc_tfm(alg_name, &crypto_skcipher_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_alloc_skcipher);

struct crypto_sync_skcipher *crypto_alloc_sync_skcipher(
				const char *alg_name, u32 type, u32 mask)
{
	struct crypto_skcipher *tfm;

	/* Only sync algorithms allowed. */
	mask |= CRYPTO_ALG_ASYNC | CRYPTO_ALG_SKCIPHER_REQSIZE_LARGE;

	tfm = crypto_alloc_tfm(alg_name, &crypto_skcipher_type, type, mask);

	/*
	 * Make sure we do not allocate something that might get used with
	 * an on-stack request: check the request size.
	 */
	if (!IS_ERR(tfm) && WARN_ON(crypto_skcipher_reqsize(tfm) >
				    MAX_SYNC_SKCIPHER_REQSIZE)) {
		crypto_free_skcipher(tfm);
		return ERR_PTR(-EINVAL);
	}

	return (struct crypto_sync_skcipher *)tfm;
}
EXPORT_SYMBOL_GPL(crypto_alloc_sync_skcipher);

int crypto_has_skcipher(const char *alg_name, u32 type, u32 mask)
{
	return crypto_type_has_alg(alg_name, &crypto_skcipher_type, type, mask);
}
EXPORT_SYMBOL_GPL(crypto_has_skcipher);

int skcipher_prepare_alg_common(struct skcipher_alg_common *alg)
{
	struct crypto_alg *base = &alg->base;

	if (alg->ivsize > PAGE_SIZE / 8 || alg->chunksize > PAGE_SIZE / 8 ||
	    alg->statesize > PAGE_SIZE / 2 ||
	    (alg->ivsize + alg->statesize) > PAGE_SIZE / 2)
		return -EINVAL;

	if (!alg->chunksize)
		alg->chunksize = base->cra_blocksize;

	base->cra_flags &= ~CRYPTO_ALG_TYPE_MASK;

	return 0;
}

static int skcipher_prepare_alg(struct skcipher_alg *alg)
{
	struct crypto_alg *base = &alg->base;
	int err;

	err = skcipher_prepare_alg_common(&alg->co);
	if (err)
		return err;

	if (alg->walksize > PAGE_SIZE / 8)
		return -EINVAL;

	if (!alg->walksize)
		alg->walksize = alg->chunksize;

	if (!alg->statesize) {
		alg->import = skcipher_noimport;
		alg->export = skcipher_noexport;
	} else if (!(alg->import && alg->export))
		return -EINVAL;

	base->cra_type = &crypto_skcipher_type;
	base->cra_flags |= CRYPTO_ALG_TYPE_SKCIPHER;

	return 0;
}

int crypto_register_skcipher(struct skcipher_alg *alg)
{
	struct crypto_alg *base = &alg->base;
	int err;

	err = skcipher_prepare_alg(alg);
	if (err)
		return err;

	return crypto_register_alg(base);
}
EXPORT_SYMBOL_GPL(crypto_register_skcipher);

void crypto_unregister_skcipher(struct skcipher_alg *alg)
{
	crypto_unregister_alg(&alg->base);
}
EXPORT_SYMBOL_GPL(crypto_unregister_skcipher);

int crypto_register_skciphers(struct skcipher_alg *algs, int count)
{
	int i, ret;

	for (i = 0; i < count; i++) {
		ret = crypto_register_skcipher(&algs[i]);
		if (ret)
			goto err;
	}

	return 0;

err:
	for (--i; i >= 0; --i)
		crypto_unregister_skcipher(&algs[i]);

	return ret;
}
EXPORT_SYMBOL_GPL(crypto_register_skciphers);

void crypto_unregister_skciphers(struct skcipher_alg *algs, int count)
{
	int i;

	for (i = count - 1; i >= 0; --i)
		crypto_unregister_skcipher(&algs[i]);
}
EXPORT_SYMBOL_GPL(crypto_unregister_skciphers);

int skcipher_register_instance(struct crypto_template *tmpl,
			   struct skcipher_instance *inst)
{
	int err;

	if (WARN_ON(!inst->free))
		return -EINVAL;

	err = skcipher_prepare_alg(&inst->alg);
	if (err)
		return err;

	return crypto_register_instance(tmpl, skcipher_crypto_instance(inst));
}
EXPORT_SYMBOL_GPL(skcipher_register_instance);

static int skcipher_setkey_simple(struct crypto_skcipher *tfm, const u8 *key,
				  unsigned int keylen)
{
	struct crypto_cipher *cipher = skcipher_cipher_simple(tfm);

	crypto_cipher_clear_flags(cipher, CRYPTO_TFM_REQ_MASK);
	crypto_cipher_set_flags(cipher, crypto_skcipher_get_flags(tfm) &
				CRYPTO_TFM_REQ_MASK);
	return crypto_cipher_setkey(cipher, key, keylen);
}

static int skcipher_init_tfm_simple(struct crypto_skcipher *tfm)
{
	struct skcipher_instance *inst = skcipher_alg_instance(tfm);
	struct crypto_cipher_spawn *spawn = skcipher_instance_ctx(inst);
	struct skcipher_ctx_simple *ctx = crypto_skcipher_ctx(tfm);
	struct crypto_cipher *cipher;

	cipher = crypto_spawn_cipher(spawn);
	if (IS_ERR(cipher))
		return PTR_ERR(cipher);

	ctx->cipher = cipher;
	return 0;
}

static void skcipher_exit_tfm_simple(struct crypto_skcipher *tfm)
{
	struct skcipher_ctx_simple *ctx = crypto_skcipher_ctx(tfm);

	crypto_free_cipher(ctx->cipher);
}

static void skcipher_free_instance_simple(struct skcipher_instance *inst)
{
	crypto_drop_cipher(skcipher_instance_ctx(inst));
	kfree(inst);
}

/**
 * skcipher_alloc_instance_simple - allocate instance of simple block cipher mode
 *
 * Allocate an skcipher_instance for a simple block cipher mode of operation,
 * e.g. cbc or ecb.  The instance context will have just a single crypto_spawn,
 * that for the underlying cipher.  The {min,max}_keysize, ivsize, blocksize,
 * alignmask, and priority are set from the underlying cipher but can be
 * overridden if needed.  The tfm context defaults to skcipher_ctx_simple, and
 * default ->setkey(), ->init(), and ->exit() methods are installed.
 *
 * @tmpl: the template being instantiated
 * @tb: the template parameters
 *
 * Return: a pointer to the new instance, or an ERR_PTR().  The caller still
 *	   needs to register the instance.
 */
struct skcipher_instance *skcipher_alloc_instance_simple(
	struct crypto_template *tmpl, struct rtattr **tb)
{
	u32 mask;
	struct skcipher_instance *inst;
	struct crypto_cipher_spawn *spawn;
	struct crypto_alg *cipher_alg;
	int err;

	err = crypto_check_attr_type(tb, CRYPTO_ALG_TYPE_SKCIPHER, &mask);
	if (err)
		return ERR_PTR(err);

	inst = kzalloc(sizeof(*inst) + sizeof(*spawn), GFP_KERNEL);
	if (!inst)
		return ERR_PTR(-ENOMEM);
	spawn = skcipher_instance_ctx(inst);

	err = crypto_grab_cipher(spawn, skcipher_crypto_instance(inst),
				 crypto_attr_alg_name(tb[1]), 0, mask);
	if (err)
		goto err_free_inst;
	cipher_alg = crypto_spawn_cipher_alg(spawn);

	err = crypto_inst_setname(skcipher_crypto_instance(inst), tmpl->name,
				  cipher_alg);
	if (err)
		goto err_free_inst;

	inst->free = skcipher_free_instance_simple;

	/* Default algorithm properties, can be overridden */
	inst->alg.base.cra_blocksize = cipher_alg->cra_blocksize;
	inst->alg.base.cra_alignmask = cipher_alg->cra_alignmask;
	inst->alg.base.cra_priority = cipher_alg->cra_priority;
	inst->alg.min_keysize = cipher_alg->cra_cipher.cia_min_keysize;
	inst->alg.max_keysize = cipher_alg->cra_cipher.cia_max_keysize;
	inst->alg.ivsize = cipher_alg->cra_blocksize;

	/* Use skcipher_ctx_simple by default, can be overridden */
	inst->alg.base.cra_ctxsize = sizeof(struct skcipher_ctx_simple);
	inst->alg.setkey = skcipher_setkey_simple;
	inst->alg.init = skcipher_init_tfm_simple;
	inst->alg.exit = skcipher_exit_tfm_simple;

	return inst;

err_free_inst:
	skcipher_free_instance_simple(inst);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(skcipher_alloc_instance_simple);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Symmetric key cipher type");
MODULE_IMPORT_NS("CRYPTO_INTERNAL");
