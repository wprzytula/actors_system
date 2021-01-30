/*
 * Based on MIM UW code (Concurrent Programming lab sample code)
 * */

#ifndef _ERR_
#define _ERR_

/* print system call error message and terminate */
extern void syserr(int bl, const char *fmt, ...);

/* print error message and terminate */
extern void fatal(const char *fmt, ...);

/* Prettifying macro */
#define verify(action, message) \
do { \
    if ((err = action) != 0) \
        syserr(err, message); \
} while (0)

#ifdef DEBUG
#define debug(action) \
do { \
    action; \
} while (0)
#else
#define debug(action)
#endif

#define mutex_lock(mutex) verify(pthread_mutex_lock(mutex), "mutex lock failed")
#define mutex_unlock(mutex) verify(pthread_mutex_unlock(mutex), "mutex unlock failed")
#define cond_wait(cond, mutex) verify(pthread_cond_wait(cond, mutex), "cond wait failed")
#define cond_signal(cond) verify(pthread_cond_signal(cond), "cond signal failed")
#define cond_broadcast(cond) verify(pthread_cond_broadcast(cond), "cond broadcast failed")
#define mutex_destroy(mutex) verify(pthread_mutex_destroy(mutex), "mutex destroy failed")
#define cond_destroy(mutex) verify(pthread_cond_destroy(mutex), "cond destroy failed")

#endif
