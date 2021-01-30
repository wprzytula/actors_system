#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <assert.h>

#include "err.h"
#include "cacti.h"
#include "message_queue.h"
#include "actors_queue.h"

#ifdef DEBUG
#include <stdio.h>
#endif

/* Defines the maximal number of messages that one thread may process on an actor
 * before returning to actors queue. */
#define MAX_MESSAGES_PROCESSED_IN_ONE_ITERATION 1

#define INITIAL_ACTOR_ARR_CAPACITY 8

/* Actor state struct & operations */
typedef struct {
    int gone_die;
    bool worked_at;
    void *state;
    actor_id_t id;
    role_t role;
    pthread_mutex_t mutex;
    message_queue_t queue;
} act_state_t;

static int act_state_init(act_state_t *const state, role_t *const role, actor_id_t new_id) {
    assert(role);
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        return -1;
    if (message_queue_init(&state->queue, ACTOR_QUEUE_LIMIT) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->gone_die = false;
    state->worked_at = false;
    state->id = new_id;
    state->role = *role;
    state->state = NULL;
    return 0;
}

static void act_state_destroy(act_state_t *const state) {
    int err;
    verify(pthread_mutex_destroy(&state->mutex),"mutex destruction failed.");
    message_queue_destroy(&state->queue);
}

/* Actor states array struct & operations */
typedef struct {
    size_t capacity;
    size_t size;
    act_state_t **arr; // array of malloc'd pointers

    // provides safety for realloc operation
    pthread_rwlock_t rwlock;
} act_state_arr;

static int act_state_arr_init(act_state_arr *const arr) {
    assert(arr);
    arr->capacity = INITIAL_ACTOR_ARR_CAPACITY;
    arr->size = 0;
    arr->arr = malloc(arr->capacity * sizeof(act_state_t*));
    if (arr->arr == NULL)
        return 1;

    if (pthread_rwlock_init(&arr->rwlock, NULL) != 0) {
        free(arr->arr);
        return -1;
    }

    return 0;
}

static int act_state_arr_emplace(act_state_arr *const arr, role_t *const role,
        actor_id_t new_id) {
    int err;
    assert(arr && role);
    assert(arr->size < CAST_LIMIT);

    rwlock_wrlock(&arr->rwlock);

    if (arr->capacity == arr->size) {
        arr->capacity *= 2;
        if ((arr->arr = realloc(arr->arr, sizeof(act_state_t*) * arr->capacity)) == NULL)
            fatal("realloc failed");
    }

    arr->arr[arr->size] = malloc(sizeof(act_state_t));
    if (arr->arr[arr->size] == NULL)
        fatal("malloc failed");
    int res = act_state_init(arr->arr[arr->size++], role, new_id);

    rwlock_unlock(&arr->rwlock);
    return res;
}

static void act_state_arr_destroy(act_state_arr *const arr) {
    int err;
    assert(arr);
    for (size_t i = 0; i < arr->size; ++i) {
        act_state_destroy(arr->arr[i]);
        free(arr->arr[i]);
    }
    rwlock_destroy(&arr->rwlock);
    free(arr->arr);
}

/* Actor system structure & operations */
struct actor_system {
    struct sigaction old_sigact;
    size_t alive_threads;
    pthread_t pool[POOL_SIZE];
    pthread_mutex_t mutex;
    pthread_cond_t new_request;
    act_state_arr actors;
    size_t alive_actors;
    actors_queue_t act_queue;
    bool interrupted;
};

/* Global actor system - one at a time */
struct actor_system *act_system = NULL;

/* Used to support actor_id_self() */
_Thread_local actor_id_t curr_actor;

/* Must be called with actor system mutex locked */
static void spawn_actor(actor_id_t *const new_actor, role_t *const role) {
    int err;
    if (act_system->actors.size == CAST_LIMIT)
        fatal("Maximum number of actors exceeded");

    *new_actor = act_system->actors.size;
    ++act_system->alive_actors;
    mutex_unlock(&act_system->mutex);

    verify(act_state_arr_emplace(&act_system->actors, role, *new_actor), "malloc failed");

    debug(printf("Spawned new actor %li.\n", *new_actor));
}

static void process_message(actor_id_t actor, message_t msg) {
    int err;
    switch (msg.message_type) {

        case MSG_SPAWN: {
            actor_id_t new_actor;
            if (act_system->interrupted)
                break;
            mutex_lock(&act_system->mutex);
            if (act_system->actors.size == CAST_LIMIT) {
                mutex_unlock(&act_system->mutex);
            } else {
                spawn_actor(&new_actor, (role_t *) msg.data);
                send_message(new_actor, (message_t) {.message_type = MSG_HELLO,
                        .nbytes = sizeof(actor_id_t),
                        .data = (void *) actor});
            }
        }
            break;

        case MSG_GODIE: {
            rwlock_rdlock(&act_system->actors.rwlock);
            act_state_t *target = act_system->actors.arr[actor];
            rwlock_unlock(&act_system->actors.rwlock);

            if (!target->gone_die) {
                target->gone_die = true;
                mutex_lock(&act_system->mutex);
                --act_system->alive_actors;
                mutex_unlock(&act_system->mutex);
            }
        }
            break;

        default: {
            rwlock_rdlock(&act_system->actors.rwlock);
            act_state_t *target = act_system->actors.arr[actor];
            rwlock_unlock(&act_system->actors.rwlock);

            if (msg.message_type >= (message_type_t)(target->role.nprompts))
                fatal("Requested message number not present in actor's control array.");

            target->role.prompts[msg.message_type]
                (&target->state, msg.nbytes, msg.data);

        }
    }
}

static void actor_system_destroy() {
    int err;
    if (act_system == NULL)
        return;

    cond_destroy(&act_system->new_request);
    mutex_destroy(&act_system->mutex);
    actors_queue_destroy(&act_system->act_queue);
    act_state_arr_destroy(&act_system->actors);

    // bring the previous handling method back
    sigaction(SIGINT, &act_system->old_sigact, NULL);

    free(act_system);
    debug(puts("System destroyed!"));
}

/* Worker threads behaviour */
static void* worker(__attribute__((unused)) void *data) {
    int err;
    message_t message;
    act_state_t *curr_act_config;

    mutex_lock(&act_system->mutex);
    ++act_system->alive_threads;
    mutex_unlock(&act_system->mutex);
    debug(printf("Thread %lu started!\n", pthread_self() % 100));

    while (true) {
        // determine job
        debug(printf("Thread %lu applies for a new job!\n", pthread_self() % 100));
        mutex_lock(&act_system->mutex);

        while (actors_queue_is_empty(&act_system->act_queue) && act_system->alive_actors > 0) {
            debug(printf("Thread %lu went asleep.\n", pthread_self() % 100));
            cond_wait(&act_system->new_request, &act_system->mutex);
            debug(printf("Thread %lu woke up!\n", pthread_self() % 100));
        }
        if (actors_queue_is_empty(&act_system->act_queue) && act_system->alive_actors == 0) {
            cond_signal(&act_system->new_request);
            mutex_unlock(&act_system->mutex);
            break;
        }
        assert(!actors_queue_is_empty(&act_system->act_queue));
        curr_actor = actors_queue_pop(&act_system->act_queue);
        mutex_unlock(&act_system->mutex);

        // work on actor begins
        debug(printf("Thread %lu began working on actor %ld!\n",
                pthread_self() % 100, curr_actor));
        rwlock_rdlock(&act_system->actors.rwlock);
        curr_act_config = act_system->actors.arr[curr_actor];
        rwlock_unlock(&act_system->actors.rwlock);

        mutex_lock(&curr_act_config->mutex);
        curr_act_config->worked_at = true;

        // loop in order to reduce resource waste on context switch
        for (size_t i = 0; i < MAX_MESSAGES_PROCESSED_IN_ONE_ITERATION; ++i) {
            assert(!message_queue_is_empty(&curr_act_config->queue));
            message = message_queue_pop(&curr_act_config->queue);
            mutex_unlock(&curr_act_config->mutex);

            debug(printf("Thread %lu has started processing message of type %ld on actor %ld!\n",
                         pthread_self() % 100, message.message_type,  curr_actor));
            process_message(curr_actor, message);

            debug(printf("Thread %lu has processed message of type %ld on actor %ld!\n",
                    pthread_self() % 100, message.message_type,  curr_actor));
            mutex_lock(&curr_act_config->mutex);
            if (message_queue_is_empty(&curr_act_config->queue))
                break;
        }
        if (!message_queue_is_empty(&curr_act_config->queue)) {
            mutex_lock(&act_system->mutex);
            assert(!actors_queue_is_full(&act_system->act_queue));
            actors_queue_push(&act_system->act_queue, curr_actor);
            mutex_unlock(&act_system->mutex);
        }
        curr_act_config->worked_at = false;
        mutex_unlock(&curr_act_config->mutex);
    }
    mutex_lock(&act_system->mutex);
    --act_system->alive_threads;
    if (act_system->alive_threads == 0) {
        bool interrupted = act_system->interrupted;
        mutex_unlock(&act_system->mutex);
        actor_system_destroy();
        if (interrupted)
            raise(SIGINT);
    } else
        mutex_unlock(&act_system->mutex);

    debug(printf("Thread %lu finished!\n", pthread_self() % 100));
    return NULL;
}

/* SIGINT handler */
static void interrupt() {
    int err;
    debug(fputs("Interrupted!", stderr));
    act_system->interrupted = true;
    rwlock_wrlock(&act_system->actors.rwlock);
    for (size_t i = 0; i < act_system->actors.size; ++i) {
        act_system->actors.arr[i]->gone_die = true;
    }
    rwlock_unlock(&act_system->actors.rwlock);
    mutex_lock(&act_system->mutex);
    act_system->alive_actors = 0;
    cond_broadcast(&act_system->new_request);
    mutex_unlock(&act_system->mutex);
}

int actor_system_create(actor_id_t *actor, role_t *const role) {
    int err;
    if (act_system != NULL)
        return -1;
    if ((act_system = malloc(sizeof(struct actor_system))) == NULL)
        goto MAIN_MALLOC_FAILED;
    if (act_state_arr_init(&act_system->actors) != 0)
        goto ACT_STATE_ARR_INIT_FAILED;
    if (act_state_arr_emplace(&act_system->actors, role, 0) != 0)
        goto ACT_STATE_INIT_FAILED;
    if (actors_queue_init(&act_system->act_queue, CAST_LIMIT) != 0)
        goto ACTOR_QUEUE_INIT_FAILED;
    if (pthread_mutex_init(&act_system->mutex, NULL) != 0)
        goto MUTEX_INIT_FAILED;
    if (pthread_cond_init(&act_system->new_request, NULL) != 0)
        goto NEW_REQUEST_INIT_FAILED;

    act_system->alive_actors = 1;
    act_system->interrupted = false;
    *actor = 0;
    debug(puts("System created!"));

    // Setting up signal handling
    struct sigaction sigact;
    sigset_t block_mask;
    sigemptyset(&block_mask);
    sigact.sa_handler = interrupt;
    sigact.sa_mask = block_mask;
    sigact.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sigact, &act_system->old_sigact);

    // Starting threads
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        pthread_create(&act_system->pool[i], &attr, worker, NULL);
    }

    debug(puts("All threads created!"));
    return 0;

    // Rollback in case of failure
    NEW_REQUEST_INIT_FAILED:
    mutex_destroy(&act_system->mutex);
    MUTEX_INIT_FAILED:
    actors_queue_destroy(&act_system->act_queue);
    ACTOR_QUEUE_INIT_FAILED:
    ACT_STATE_INIT_FAILED:
    act_state_arr_destroy(&act_system->actors);
    ACT_STATE_ARR_INIT_FAILED:
    free(act_system);
    MAIN_MALLOC_FAILED:
    return -1;
}

actor_id_t actor_id_self() {
    return curr_actor;
}

void actor_system_join(actor_id_t actor) {
    int err;
    pthread_t pool_copy[POOL_SIZE];

    if (act_system == NULL)
        return;

    if (actor >= 0 && actor < (actor_id_t)act_system->actors.size) {
        // Copying pool array data in order to avoid segmentation fault in case of SIGINT.
        for (size_t i = 0; i < POOL_SIZE; ++i) {
            pool_copy[i] = act_system->pool[i];
        }

        // Waiting for each thread in pool to finish.
        for (size_t i = 0; i < POOL_SIZE; ++i)
            verify(pthread_join(pool_copy[i], NULL), "join failed");
    }
}

int send_message(actor_id_t actor, message_t message) {
    int err;
    if (actor >= (actor_id_t)act_system->actors.size)
        return -2; // no such target

    // Fetching pointer to target actor
    rwlock_rdlock(&act_system->actors.rwlock);
    act_state_t *target = act_system->actors.arr[actor];
    rwlock_unlock(&act_system->actors.rwlock);

    if (target->gone_die)
        return -1; // target does not accept new messages

    debug(printf("Sending message of type %li to actor %li...\n", message.message_type, actor));

    mutex_lock(&target->mutex);
    bool was_empty = !target->worked_at &&
            message_queue_is_empty(&target->queue);

    assert(!message_queue_is_full(&target->queue));
    message_queue_push(&target->queue, message);

    mutex_unlock(&target->mutex);

    debug(printf("Sent message to actor %li.\n", actor));

    // If the actor queue was empty, it is required to push the actor id to actors queue.
    if (was_empty) {
        mutex_lock(&act_system->mutex);
        assert(!actors_queue_is_full(&act_system->act_queue));
        actors_queue_push(&act_system->act_queue, actor);

        debug(printf("Pushed actor %li to actors queue.\n", actor));

        cond_signal(&act_system->new_request);
        mutex_unlock(&act_system->mutex);
    }
    return 0;
}
