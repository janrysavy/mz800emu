#ifndef APP_THREAD_H
#define APP_THREAD_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifndef USE_SDL_MUTEXES
#include <glib.h>

    typedef GMutex app_mutex_t;

static inline void app_mutex_create(app_mutex_t **mutex)
{
    *mutex = g_new(app_mutex_t, 1);
    if (*mutex == NULL)
    {
        fprintf(stderr, "%s(%d): Out of memory\n", __func__, __LINE__);
        return;
    };
    g_mutex_init(*mutex);
}

static inline void app_mutex_destroy(app_mutex_t **mutex)
{
    if (*mutex == NULL)
    {
        return;
    };
    g_mutex_clear(*mutex);
    g_free(*mutex);
    *mutex = NULL;
}


#define APP_MUTEX_CREATE(mutex) app_mutex_create(&mutex)
#define APP_MUTEX_DESTROY(mutex) app_mutex_destroy(&mutex)
#define APP_MUTEX_LOCK(mutex) g_mutex_lock(mutex)
#define APP_MUTEX_TRYLOCK(mutex) g_mutex_trylock(mutex)
#define APP_MUTEX_UNLOCK(mutex) g_mutex_unlock(mutex)

    typedef GRWLock app_rwlock_t;

static inline void app_rwlock_create(app_rwlock_t **rwlock)
{
    *rwlock = g_new(app_rwlock_t, 1);
    if (*rwlock == NULL)
    {
        fprintf(stderr, "%s(%d): Out of memory\n", __func__, __LINE__);
        return;
    };
    g_rw_lock_init(*rwlock);
}

static inline void app_rwlock_destroy(app_rwlock_t **rwlock)
{
    if (*rwlock == NULL)
    {
        return;
    };
    g_rw_lock_clear(*rwlock);
    g_free(*rwlock);
    *rwlock = NULL;
}

#define APP_RWLOCK_CREATE(rwlock) app_rwlock_create(&rwlock)
#define APP_RWLOCK_DESTROY(rwlock) app_rwlock_destroy(&rwlock)
#define APP_RWLOCK_RDLOCK(rwlock) g_rw_lock_reader_lock(rwlock)
#define APP_RWLOCK_TRYRDLOCK(rwlock) g_rw_lock_reader_trylock(rwlock)
#define APP_RWLOCK_RDUNLOCK(rwlock) g_rw_lock_reader_unlock(rwlock)
#define APP_RWLOCK_WRLOCK(rwlock) g_rw_lock_writer_lock(rwlock)
#define APP_RWLOCK_TRYWRLOCK(rwlock) g_rw_lock_writer_trylock(rwlock)
#define APP_RWLOCK_WRUNLOCK(rwlock) g_rw_lock_writer_unlock(rwlock)

    typedef GCond app_cond_t;

static inline void app_cond_create(app_cond_t **cond)
{
    *cond = g_new(app_cond_t, 1);
    if (*cond == NULL)
    {
        fprintf(stderr, "%s(%d): Out of memory\n", __func__, __LINE__);
        return;
    };
    g_cond_init(*cond);
}

static inline void app_cond_destroy(app_cond_t **cond)
{
    if (*cond == NULL)
    {
        return;
    };
    g_cond_clear(*cond);
    g_free(*cond);
    *cond = NULL;
}

#define APP_COND_CREATE(cond) app_cond_create(&cond)
#define APP_COND_DESTROY(cond) app_cond_destroy(&cond)
#define APP_COND_BROADCAST(cond) g_cond_broadcast(cond)
#define APP_COND_SIGNAL(cond) g_cond_signal(cond)
#define APP_COND_WAIT(cond, mutex) g_cond_wait(cond, mutex)
#define APP_COND_WAIT_UNTIL(cond, mutex, end_time) g_cond_wait_until(cond, mutex, end_time)
#define APP_COND_WAIT_TIMEOUT_MS(cond, mutex, timeout_ms) app_cond_wait_ms(cond, mutex, timeout_ms)

    static inline gboolean app_cond_wait_ms(app_cond_t *cond, app_mutex_t *mutex, gint32 timeout_ms)
    {
        gint64 end_time = g_get_monotonic_time() + timeout_ms * G_TIME_SPAN_MILLISECOND;
        return g_cond_wait_until(cond, mutex, end_time);
    }
#else

#ifdef USE_SDL2
#include <SDL.h>
#define SDL_Mutex SDL_mutex
#define SDL_RWLock SDL_mutex
#define SDL_CreateRWLock SDL_CreateMutex
#define SDL_DestroyRWLock SDL_DestroyMutex
#define SDL_LockRWLockForReading SDL_LockMutex
#define SDL_LockRWLockForWriting SDL_LockMutex
#define SDL_UnlockRWLock SDL_UnlockMutex
#define SDL_Condition SDL_cond
#define SDL_CreateCondition SDL_CreateCond
#define SDL_DestroyCondition SDL_DestroyCond
#define SDL_SignalCondition SDL_CondSignal
#define SDL_WaitCondition SDL_CondWait
#define SDL_WaitConditionTimeout SDL_CondWaitTimeout
#else
#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#error "Unsupported SDL version"
#endif // USE_SDL3
#endif // USE_SDL2

    typedef SDL_Mutex app_mutex_t;

static inline void app_mutex_create(app_mutex_t **mutex)
{
    *mutex = SDL_CreateMutex();
    if (*mutex == NULL)
    {
        fprintf(stderr, "%s(%d): Could not create mutex: %s\n", __func__, __LINE__, SDL_GetError());
        return;
    };
}

static inline void app_mutex_destroy(app_mutex_t **mutex)
{
    if (*mutex == NULL)
    {
        return;
    };
    SDL_DestroyMutex(*mutex);
    *mutex = NULL;
}

#define APP_MUTEX_CREATE(mutex) app_mutex_create(&mutex)
#define APP_MUTEX_DESTROY(mutex) app_mutex_destroy(&mutex)
#define APP_MUTEX_LOCK(mutex) SDL_LockMutex(mutex)
#define APP_MUTEX_TRYLOCK(mutex) SDL_TryLockMutex(mutex)
#define APP_MUTEX_UNLOCK(mutex) SDL_UnlockMutex(mutex)

    typedef SDL_RWLock app_rwlock_t;

static inline void app_rwlock_create(app_rwlock_t **rwlock)
{
    *rwlock = SDL_CreateRWLock();
    if (*rwlock == NULL)
    {
        fprintf(stderr, "%s(%d): Could not create rwlock: %s\n", __func__, __LINE__, SDL_GetError());
        return;
    };
}

static inline void app_rwlock_destroy(app_rwlock_t **rwlock)
{
    if (*rwlock == NULL)
    {
        return;
    };
    SDL_DestroyRWLock(*rwlock);
    *rwlock = NULL;
}

#define APP_RWLOCK_CREATE(rwlock) app_rwlock_create(&rwlock)
#define APP_RWLOCK_DESTROY(rwlock) app_rwlock_destroy(&rwlock)
#define APP_RWLOCK_RDLOCK(rwlock) SDL_LockRWLockForReading(rwlock)
#define APP_RWLOCK_TRYRDLOCK(rwlock) SDL_TryLockRWLockForReading(rwlock)
#define APP_RWLOCK_RDUNLOCK(rwlock) SDL_UnlockRWLock(rwlock)
#define APP_RWLOCK_WRLOCK(rwlock) SDL_LockRWLockForWriting(rwlock)
#define APP_RWLOCK_TRYWRLOCK(rwlock) SDL_TryLockRWLockForWriting(rwlock)
#define APP_RWLOCK_WRUNLOCK(rwlock) SDL_UnlockRWLock(rwlock)

    typedef SDL_Condition app_cond_t;

static inline void app_cond_create(app_cond_t **cond)
{
    *cond = SDL_CreateCondition();
    if (*cond == NULL)
    {
        fprintf(stderr, "%s(%d): Could not create cond: %s\n", __func__, __LINE__, SDL_GetError());
        return;
    };
}

static inline void app_cond_destroy(app_cond_t **cond)
{
    if (*cond == NULL)
    {
        return;
    };
    SDL_DestroyCondition(*cond);
    *cond = NULL;
}

#define APP_COND_CREATE(cond) app_cond_create(&cond)
#define APP_COND_DESTROY(cond) app_cond_destroy(&cond)
#define APP_COND_BROADCAST(cond) SDL_BroadcastCondition(cond)
#define APP_COND_SIGNAL(cond) SDL_SignalCondition(cond)
#define APP_COND_WAIT(cond, mutex) SDL_WaitCondition(cond, mutex)
//#define APP_COND_WAIT_UNTIL(cond, mutex, end_time) SDL_CondWaitTimeout(cond, mutex, end_time)
#define APP_COND_WAIT_TIMEOUT_MS(cond, mutex, timeout_ms) SDL_WaitConditionTimeout(cond, mutex, timeout_ms)

    
#endif



    /****************************************************************************************
     * MACRO DEFINITIONS
     ***************************************************************************************/

#define MUTEX_READ_VAR(mutex, var) ({ \
    APP_MUTEX_LOCK(mutex);            \
    __typeof__(var) _val = var;       \
    APP_MUTEX_UNLOCK(mutex);          \
    _val;                             \
})

#define MUTEX_WRITE_VAR(mutex, var, value) \
    do                                     \
    {                                      \
        APP_MUTEX_LOCK(mutex);             \
        var = value;                       \
        APP_MUTEX_UNLOCK(mutex);           \
    } while (0)

#define RWLOCK_READ_VAR(rwlock, var) ({ \
    APP_RWLOCK_RDLOCK(rwlock);          \
    __typeof__(var) _val = var;         \
    APP_RWLOCK_RDUNLOCK(rwlock);        \
    _val;                               \
})

#define RWLOCK_WRITE_VAR(rwlock, var, value) \
    do                                       \
    {                                        \
        APP_RWLOCK_WRLOCK(rwlock);           \
        var = value;                         \
        APP_RWLOCK_WRUNLOCK(rwlock);         \
    } while (0)


#ifdef __cplusplus
extern "C"
{
#endif


#ifdef __cplusplus
}
#endif
#endif // APP_THREAD_H
