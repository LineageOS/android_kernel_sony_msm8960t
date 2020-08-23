/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/crypto.h>

#include "zcomp.h"

/*
 * single zcomp_strm backend
 */
struct zcomp_strm_single {
	struct mutex strm_lock;
	struct zcomp_strm *zstrm;
};

/*
 * multi zcomp_strm backend
 */
struct zcomp_strm_multi {
	/* protect strm list */
	spinlock_t strm_lock;
	/* max possible number of zstrm streams */
	int max_strm;
	/* number of available zstrm streams */
	int avail_strm;
	/* list of available strms */
	struct list_head idle_strm;
	wait_queue_head_t strm_wait;
};

static const char * const backends[] = {
	"lzo",
#ifdef CONFIG_ZRAM_LZ4_COMPRESS
	"lz4",
#endif
	NULL
};

static const char *find_backend(const char *compress)
{
	int i = 0;
	while (backends[i]) {
		if (sysfs_streq(compress, backends[i]))
			break;
		i++;
	}
	return backends[i];
}

static void zcomp_strm_free(struct zcomp_strm *zstrm)
{
	if (!IS_ERR_OR_NULL(zstrm->tfm))
		crypto_free_comp(zstrm->tfm);
	free_pages((unsigned long)zstrm->buffer, 1);
	kfree(zstrm);
}

/*
 * allocate new zcomp_strm structure with ->tfm initialized by
 * backend, return NULL on error
 */
static struct zcomp_strm *zcomp_strm_alloc(struct zcomp *comp, gfp_t flags)
{
	struct zcomp_strm *zstrm = kmalloc(sizeof(*zstrm), flags);
	if (!zstrm)
		return NULL;

	zstrm->tfm = crypto_alloc_comp(comp->name, 0, 0);
	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = (void *)__get_free_pages(flags | __GFP_ZERO, 1);
	if (IS_ERR_OR_NULL(zstrm->tfm) || !zstrm->buffer) {
		zcomp_strm_free(zstrm);
		zstrm = NULL;
	}
	return zstrm;
}

/*
 * get idle zcomp_strm or wait until other process release
 * (zcomp_strm_release()) one for us
 */
static struct zcomp_strm *zcomp_strm_multi_find(struct zcomp *comp)
{
	struct zcomp_strm_multi *zs = comp->stream;
	struct zcomp_strm *zstrm;

	while (1) {
		spin_lock(&zs->strm_lock);
		if (!list_empty(&zs->idle_strm)) {
			zstrm = list_entry(zs->idle_strm.next,
					struct zcomp_strm, list);
			list_del(&zstrm->list);
			spin_unlock(&zs->strm_lock);
			return zstrm;
		}
		/* zstrm streams limit reached, wait for idle stream */
		if (zs->avail_strm >= zs->max_strm) {
			spin_unlock(&zs->strm_lock);
			wait_event(zs->strm_wait, !list_empty(&zs->idle_strm));
			continue;
		}
		/* allocate new zstrm stream */
		zs->avail_strm++;
		spin_unlock(&zs->strm_lock);
		/*
		 * This function can be called in swapout/fs write path
		 * so we can't use GFP_FS|IO. And it assumes we already
		 * have at least one stream in zram initialization so we
		 * don't do best effort to allocate more stream in here.
		 * A default stream will work well without further multiple
		 * streams. That's why we use NORETRY | NOWARN.
		 */
		zstrm = zcomp_strm_alloc(comp, GFP_NOIO | __GFP_NORETRY |
					__GFP_NOWARN);
		if (!zstrm) {
			spin_lock(&zs->strm_lock);
			zs->avail_strm--;
			spin_unlock(&zs->strm_lock);
			wait_event(zs->strm_wait, !list_empty(&zs->idle_strm));
			continue;
		}
		break;
	}
	return zstrm;
}

/* add stream back to idle list and wake up waiter or free the stream */
static void zcomp_strm_multi_release(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	struct zcomp_strm_multi *zs = comp->stream;

	spin_lock(&zs->strm_lock);
	if (zs->avail_strm <= zs->max_strm) {
		list_add(&zstrm->list, &zs->idle_strm);
		spin_unlock(&zs->strm_lock);
		wake_up(&zs->strm_wait);
		return;
	}

	zs->avail_strm--;
	spin_unlock(&zs->strm_lock);
	zcomp_strm_free(zstrm);
}

/* change max_strm limit */
static bool zcomp_strm_multi_set_max_streams(struct zcomp *comp, int num_strm)
{
	struct zcomp_strm_multi *zs = comp->stream;
	struct zcomp_strm *zstrm;

	spin_lock(&zs->strm_lock);
	zs->max_strm = num_strm;
	/*
	 * if user has lowered the limit and there are idle streams,
	 * immediately free as much streams (and memory) as we can.
	 */
	while (zs->avail_strm > num_strm && !list_empty(&zs->idle_strm)) {
		zstrm = list_entry(zs->idle_strm.next,
				struct zcomp_strm, list);
		list_del(&zstrm->list);
		zcomp_strm_free(zstrm);
		zs->avail_strm--;
	}
	spin_unlock(&zs->strm_lock);
	return true;
}

static void zcomp_strm_multi_destroy(struct zcomp *comp)
{
	struct zcomp_strm_multi *zs = comp->stream;
	struct zcomp_strm *zstrm;

	while (!list_empty(&zs->idle_strm)) {
		zstrm = list_entry(zs->idle_strm.next,
				struct zcomp_strm, list);
		list_del(&zstrm->list);
		zcomp_strm_free(zstrm);
	}
	kfree(zs);
}

static int zcomp_strm_multi_create(struct zcomp *comp, int max_strm)
{
	struct zcomp_strm *zstrm;
	struct zcomp_strm_multi *zs;

	comp->destroy = zcomp_strm_multi_destroy;
	comp->strm_find = zcomp_strm_multi_find;
	comp->strm_release = zcomp_strm_multi_release;
	comp->set_max_streams = zcomp_strm_multi_set_max_streams;
	zs = kmalloc(sizeof(struct zcomp_strm_multi), GFP_KERNEL);
	if (!zs)
		return -ENOMEM;

	comp->stream = zs;
	spin_lock_init(&zs->strm_lock);
	INIT_LIST_HEAD(&zs->idle_strm);
	init_waitqueue_head(&zs->strm_wait);
	zs->max_strm = max_strm;
	zs->avail_strm = 1;

	zstrm = zcomp_strm_alloc(comp, GFP_KERNEL);
	if (!zstrm) {
		kfree(zs);
		return -ENOMEM;
	}
	list_add(&zstrm->list, &zs->idle_strm);
	return 0;
}

static struct zcomp_strm *zcomp_strm_single_find(struct zcomp *comp)
{
	struct zcomp_strm_single *zs = comp->stream;
	mutex_lock(&zs->strm_lock);
	return zs->zstrm;
}

static void zcomp_strm_single_release(struct zcomp *comp,
		struct zcomp_strm *zstrm)
{
	struct zcomp_strm_single *zs = comp->stream;
	mutex_unlock(&zs->strm_lock);
}

static bool zcomp_strm_single_set_max_streams(struct zcomp *comp, int num_strm)
{
	/* zcomp_strm_single support only max_comp_streams == 1 */
	return false;
}

static void zcomp_strm_single_destroy(struct zcomp *comp)
{
	struct zcomp_strm_single *zs = comp->stream;
	zcomp_strm_free(zs->zstrm);
	kfree(zs);
}

static int zcomp_strm_single_create(struct zcomp *comp)
{
	struct zcomp_strm_single *zs;

	comp->destroy = zcomp_strm_single_destroy;
	comp->strm_find = zcomp_strm_single_find;
	comp->strm_release = zcomp_strm_single_release;
	comp->set_max_streams = zcomp_strm_single_set_max_streams;
	zs = kmalloc(sizeof(struct zcomp_strm_single), GFP_KERNEL);
	if (!zs)
		return -ENOMEM;

	comp->stream = zs;
	mutex_init(&zs->strm_lock);
	zs->zstrm = zcomp_strm_alloc(comp, GFP_KERNEL);
	if (!zs->zstrm) {
		kfree(zs);
		return -ENOMEM;
	}
	return 0;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	ssize_t sz = 0;
	int i = 0;

	while (backends[i]) {
		if (!strcmp(comp, backends[i]))
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]);
		else
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]);
		i++;
	}
	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

bool zcomp_available_algorithm(const char *comp)
{
	return find_backend(comp) != NULL;
}

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm)
{
	return comp->set_max_streams(comp, num_strm);
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	return comp->strm_find(comp);
}

void zcomp_stream_put(struct zcomp *comp, struct zcomp_strm *zstrm)
{
	comp->strm_release(comp, zstrm);
}

int zcomp_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int *dst_len)
{
	/*
	 * Our dst memory (zstrm->buffer) is always `2 * PAGE_SIZE' sized
	 * because sometimes we can endup having a bigger compressed data
	 * due to various reasons: for example compression algorithms tend
	 * to add some padding to the compressed buffer. Speaking of padding,
	 * comp algorithm `842' pads the compressed length to multiple of 8
	 * and returns -ENOSP when the dst memory is not big enough, which
	 * is not something that ZRAM wants to see. We can handle the
	 * `compressed_size > PAGE_SIZE' case easily in ZRAM, but when we
	 * receive -ERRNO from the compressing backend we can't help it
	 * anymore. To make `842' happy we need to tell the exact size of
	 * the dst buffer, zram_drv will take care of the fact that
	 * compressed buffer is too big.
	 */
	*dst_len = PAGE_SIZE * 2;

	return crypto_comp_compress(zstrm->tfm,
			src, PAGE_SIZE,
			zstrm->buffer, dst_len);
}

int zcomp_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst)
{
	unsigned int dst_len = PAGE_SIZE;

	return crypto_comp_decompress(zstrm->tfm,
			src, src_len,
			dst, &dst_len);
}

void zcomp_destroy(struct zcomp *comp)
{
	comp->destroy(comp);
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by functions zcomp_strm_{multi,single}_create.
 */
struct zcomp *zcomp_create(const char *compress, int max_strm)
{
	struct zcomp *comp;
	const char *backend;
	int error;

	backend = find_backend(compress);
	if (!backend)
		return ERR_PTR(-EINVAL);

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->name = backend;
	if (max_strm > 1)
		error = zcomp_strm_multi_create(comp, max_strm);
	else
		error = zcomp_strm_single_create(comp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
