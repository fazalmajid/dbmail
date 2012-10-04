/*
  
 Copyright (c) 2004-2012 NFG Net Facilities Group BV support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "dbmail.h"
#include "dm_cache.h"
#include <assert.h>
#include <pthread.h>

#define THIS_MODULE "Cache"
/*
 * cached raw message data
 *
 * implement a global message cache as a LIST of Stream_T objects
 * with reference bookkeeping and TTL
 *
 */
#define T Cache_T

#define TTL_SECONDS 30
#define GC_INTERVAL 10

#define CACHE_LOCK(a) if (pthread_mutex_lock(&(a))) { perror("pthread_mutex_lock failed"); }
#define CACHE_UNLOCK(a) if (pthread_mutex_unlock(&(a))) { perror("pthread_mutex_unlock failed"); }

struct element {
	uint64_t id;
	uint64_t size;
	unsigned ttl;
	unsigned ref;
	Stream_T mem;
};

struct T {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	GList *elements;
	uint64_t size;
	volatile int done; // exit flag
};

pthread_t gc_thread_id;

static void Cache_gc(T C);

static void * _gc_callback(void *data)
{
	TRACE(TRACE_DEBUG, "start Cache GC thread. sweep interval [%d]",
			GC_INTERVAL);
	T C = (T)data;
	CACHE_LOCK(C->lock);
	while (! C->done) {
		struct timespec wait = {0,0};
		wait.tv_sec = time(NULL) + GC_INTERVAL;
		pthread_cond_timedwait(&C->cond, &C->lock, &wait);
		if (C->done) break;
		Cache_gc(C);
	}
	CACHE_UNLOCK(C->lock);
	return NULL;
}

/* */
T Cache_new(void)
{
	T C;
	
	C = (T)g_malloc0(sizeof(*C));

	if (pthread_mutex_init(&C->lock, NULL)) {
		perror("pthread_mutex_init failed");
		g_free(C);
		return NULL;
	}
	if (pthread_cond_init(&C->cond, NULL)) {
		perror("pthread_cond_init failed");
		g_free(C);
		return NULL;
	}

	C->elements = NULL;

	if (pthread_create(&gc_thread_id, NULL, _gc_callback, C)) {
		perror("GC thread create failed");
		g_free(C);
		return NULL;
	}

	return C;
}

static struct element * Cache_find(T C, uint64_t id)
{
	assert(C);
	struct element *E = NULL;
	GList *elements = C->elements;
	elements = g_list_first(elements);
	while (elements) {
		E = elements->data;
		if (E->id == id) return E;
		if (! g_list_next(elements)) break;
		elements = g_list_next(elements);
	}
	return NULL;
}

static void Cache_remove(T C, struct element *E)
{
	C->elements = g_list_remove(C->elements, E);
	Stream_close(&E->mem);
	C->size -= (E->size + sizeof(*E));
	g_free(E);
}

void Cache_clear(T C, uint64_t id)
{
	struct element *E;
	CACHE_LOCK(C->lock);
	if (! (E = Cache_find(C, id))) {
		CACHE_UNLOCK(C->lock);
		return;
	}

	Cache_remove(C, E);
	CACHE_UNLOCK(C->lock);
}

uint64_t Cache_update(T C, DbmailMessage *message)
{
	uint64_t outcnt = 0;
	char *crlf = NULL;
	struct element *E;
	time_t now = time(NULL);

	TRACE(TRACE_DEBUG, "message [%lu]", message->id);
	CACHE_LOCK(C->lock);
	E = Cache_find(C, message->id);
	if (E) {
		uint64_t size = E->size;
		E->ttl = now + TTL_SECONDS;
		CACHE_UNLOCK(C->lock);
		return size;
	}

	crlf = get_crlf_encoded(message->raw_content);
	E = (struct element *)g_malloc0(sizeof(struct element));
	assert(E);

	outcnt = strlen(crlf);

	E->ref = 0;
	E->id = message->id;
	E->size = outcnt;
	E->ttl = now + TTL_SECONDS;
	E->mem = Stream_open();

	Stream_rewind(E->mem);
	Stream_write(E->mem, crlf, outcnt);
	Stream_rewind(E->mem);

	C->size += outcnt + sizeof(*E);

	C->elements = g_list_append(C->elements, E);
	CACHE_UNLOCK(C->lock);

	g_free(crlf);
	return outcnt;
}

uint64_t Cache_get_size(T C, uint64_t id)
{
	uint64_t size;
	struct element *E;
	assert(C);
	CACHE_LOCK(C->lock);
	E = Cache_find(C, id);
	if (! E) {
		CACHE_UNLOCK(C->lock);
		return 0;
	}
	size = E->size;
	CACHE_UNLOCK(C->lock);
	return size;
}

void Cache_get_mem(T C, uint64_t id, Stream_T M)
{
	time_t now = time(NULL);
	struct element *E;
	assert(C);
	CACHE_LOCK(C->lock);
	E = Cache_find(C, id);
	assert(E);
	E->ref++;
	E->ttl = now + TTL_SECONDS;
	Stream_ref(E->mem, M);
	CACHE_UNLOCK(C->lock);
}

void Cache_unref_mem(T C, uint64_t id, Stream_T *M)
{
	time_t now = time(NULL);
	Stream_T m = *M;
	struct element *E;
	assert(C);
	assert(m);
	CACHE_LOCK(C->lock);
	E = Cache_find(C, id);
	assert(E);
	E->ref--;
	if (E->ttl < now && E->ref <= 0)
		Cache_remove(C, E);
	CACHE_UNLOCK(C->lock);
	g_free(m);
	m = NULL;
}

/*
 * garbage collection: sweep the cache for unref-ed 
 * or expired objects
 */

void Cache_gc(T C)
{
	assert(C);
	struct element *E = NULL;
	time_t now = time(NULL);
	GList *elements;
	if (C->done) return;
	elements = g_list_first(C->elements);
	TRACE(TRACE_DEBUG, "running GC ...");
	while (elements) {
		E = elements->data;
		if (E->ttl < now && E->ref <= 0)
			Cache_remove(C, E);
		if (! g_list_next(elements)) break;
		elements = g_list_next(elements);
	}
	TRACE(TRACE_DEBUG, "running GC finished.");
}
/*
 * closes the msg cache
 */
void Cache_free(T *C)
{
	struct element *E = NULL;
	GList *elements;
	T c = *C;
	CACHE_LOCK(c->lock);
	c->done = true;
	elements = g_list_first(c->elements);
	while (elements) {
		E = elements->data;
		Cache_remove(c, E);
		if (! g_list_next(elements)) break;
		elements = g_list_next(elements);
	}
	CACHE_UNLOCK(c->lock);
	pthread_cond_signal(&c->cond);
	pthread_join(gc_thread_id, NULL);

	pthread_cond_destroy(&c->cond);
	pthread_mutex_destroy(&c->lock);
	g_free(c);
	c = NULL;
	
}

#undef T
#undef CACHE_LOCK
#undef CACHE_UNLOCK

