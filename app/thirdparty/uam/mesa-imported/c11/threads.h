#pragma once

// Real mutexes (SwitchWakerHD patch 1, see PATCHES.md). uam's own header was a single-threaded stub
// (every lock a no-op), which let two threads build Mesa's built-in GLSL function library at the
// same time and damage the ralloc heap. uam stays NOT reentrant (static gl_context in
// glsl_frontend.cpp): this only keeps Mesa's global tables consistent; compile on one thread.
#if defined(_MSC_VER)
// tw2de (2026-11-07): an MSVC-ABI Windows host (clang-cl: the uam PC tool, build_switch.py build_host_uam) has no
// pthreads: an SRW lock, made recursive by its owner and depth (mtx_recursive), with a static initializer as
// PTHREAD_MUTEX_INITIALIZER.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef struct {
	SRWLOCK lock;
	DWORD owner;
	unsigned depth;
	int recursive;
} mtx_t;
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };
enum { thrd_success = 0, thrd_error = 2 };

static inline int mtx_init(mtx_t *m, int type)
{
	InitializeSRWLock(&m->lock);
	m->owner = 0;
	m->depth = 0;
	m->recursive = (type & mtx_recursive) != 0;
	return thrd_success;
}
static inline void mtx_destroy(mtx_t *m) { (void)m; }
static inline int mtx_lock(mtx_t *m)
{
	const DWORD self = GetCurrentThreadId();
	if (m->recursive && m->owner == self) {
		m->depth++;
		return thrd_success;
	}
	AcquireSRWLockExclusive(&m->lock);
	m->owner = self;
	m->depth = 1;
	return thrd_success;
}
static inline int mtx_unlock(mtx_t *m)
{
	if (--m->depth == 0) {
		m->owner = 0;
		ReleaseSRWLockExclusive(&m->lock);
	}
	return thrd_success;
}

#define _MTX_INITIALIZER_NP { SRWLOCK_INIT, 0, 0, 0 }
#else
#include <pthread.h>

typedef pthread_mutex_t mtx_t;
enum { mtx_plain = 0, mtx_recursive = 1, mtx_timed = 2 };

static inline int mtx_init(mtx_t *m, int type)
{
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, (type & mtx_recursive) ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_NORMAL);
	int r = pthread_mutex_init(m, &a);
	pthread_mutexattr_destroy(&a);
	return r;
}
static inline void mtx_destroy(mtx_t *m) { pthread_mutex_destroy(m); }
static inline int mtx_lock(mtx_t *m) { return pthread_mutex_lock(m); }
static inline int mtx_unlock(mtx_t *m) { return pthread_mutex_unlock(m); }

#define _MTX_INITIALIZER_NP PTHREAD_MUTEX_INITIALIZER
#endif
