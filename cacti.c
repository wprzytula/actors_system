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
    act_state_t *arr;
} act_state_arr;

static int act_state_arr_init(act_state_arr *const arr) {
    assert(arr);
    arr->capacity = INITIAL_ACTOR_ARR_CAPACITY;
    arr->arr = malloc(arr->capacity * sizeof(act_state_t));
    arr->size = 0;
    if (arr->arr == NULL)
        return 1;
    return 0;
}

static int act_state_arr_emplace(act_state_arr *const arr, role_t *const role, actor_id_t new_id) {
    assert(arr && role);
    assert(arr->size < CAST_LIMIT);
    if (arr->capacity == arr->size) {
        arr->capacity *= 2;
        // Here we act as a privileged Writer in Readers & Writers model
        if ((arr->arr = realloc(arr->arr, sizeof(act_state_t) * arr->capacity)) == NULL)
            fatal("realloc failed");
    }
    return act_state_init(arr->arr + arr->size++, role, new_id);
}

static void act_state_arr_destroy(act_state_arr *const arr) {
    assert(arr);
    for (size_t i = 0; i < arr->size; ++i) {
        act_state_destroy(arr->arr + i);
    }
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

struct actor_system *act_system = NULL;
_Thread_local actor_id_t curr_actor;

actor_id_t actor_id_self() {
    return curr_actor;
}

static void spawn_actor(actor_id_t *const new_actor, role_t *const role) {
    int err;
    if (act_system->actors.size == CAST_LIMIT)
        fatal("Maximum number of actors exceeded");
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    *new_actor = act_system->actors.size;
    ++act_system->alive_actors;
    verify(act_state_arr_emplace(&act_system->actors, role, *new_actor), "malloc failed");
    debug(printf("Spawned new actor %li.\n", *new_actor));
    verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
}

static void process_message(actor_id_t actor, message_t message) {
    actor_id_t new_actor;
    switch (message.message_type) {
        case MSG_SPAWN:
            if (act_system->interrupted)
                break;
            spawn_actor(&new_actor, (role_t *) message.data);
            send_message(new_actor, (message_t){.message_type = MSG_HELLO,
                    .nbytes = sizeof(actor_id_t),
                    .data = (void*)actor});
            break;
        case MSG_GODIE:
            act_system->actors.arr[actor].gone_die = true;
            --act_system->alive_actors;
            break;
        default:
            if (message.message_type >= (message_type_t)(act_system->actors.arr[actor].role.nprompts))
                fatal("Requested message number not present in actor's control array.");
            act_system->actors.arr[actor].role.prompts[message.message_type]
                    (&act_system->actors.arr[actor].state, message.nbytes, message.data);
    }
}

static void actor_system_destroy() {
    int err;
    if (act_system == NULL)
        return;

    verify(pthread_cond_destroy(&act_system->new_request), "cond destruction failed");
    verify(pthread_mutex_destroy(&act_system->mutex), "mutex destruction failed");
    actors_queue_destroy(&act_system->act_queue);
    act_state_arr_destroy(&act_system->actors);

    // bring the previous handling method back
    sigaction(SIGINT, &act_system->old_sigact, NULL);

    free(act_system);
    debug(puts("System destroyed!"));
}

static void* worker(__attribute__((unused)) void *data) {
    int err;
    message_t message;
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    ++act_system->alive_threads;
    verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
    debug(printf("Thread %lu started!\n", pthread_self() % 100));

    while (true) {
        // determine job
        debug(printf("Thread %lu applies for a new job!\n", pthread_self() % 100));
        verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");

        while (actors_queue_is_empty(&act_system->act_queue) && act_system->alive_actors > 0) {
            debug(printf("Thread %lu went asleep.\n", pthread_self() % 100));
            verify(pthread_cond_wait(&act_system->new_request, &act_system->mutex),
                    "cond wait failed");
            debug(printf("Thread %lu woke up!\n", pthread_self() % 100));
        }
        if (actors_queue_is_empty(&act_system->act_queue) && act_system->alive_actors == 0) {
            verify(pthread_cond_signal(&act_system->new_request),
                   "cond signal failed");
            verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
            break;
        }
        assert(!actors_queue_is_empty(&act_system->act_queue));
        curr_actor = actors_queue_pop(&act_system->act_queue);
        verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");

        // work on actor begins
        debug(printf("Thread %lu began working on actor %ld!\n",
                pthread_self() % 100, curr_actor));
        verify(pthread_mutex_lock(&act_system->actors.arr[curr_actor].mutex),
               "mutex lock failed");
        act_system->actors.arr[curr_actor].worked_at = true;

        // loop in order to reduce resource waste on context switch
        for (size_t i = 0; i < MAX_MESSAGES_PROCESSED_IN_ONE_ITERATION; ++i) {
            assert(!message_queue_is_empty(&act_system->actors.arr[curr_actor].queue));
            message = message_queue_pop(&act_system->actors.arr[curr_actor].queue);
            verify(pthread_mutex_unlock(&act_system->actors.arr[curr_actor].mutex),
                   "mutex unlock failed");

            debug(printf("Thread %lu has started processing message of type %ld on actor %ld!\n",
                         pthread_self() % 100, message.message_type,  curr_actor));
            process_message(curr_actor, message);

            debug(printf("Thread %lu has processed message of type %ld on actor %ld!\n",
                    pthread_self() % 100, message.message_type,  curr_actor));
            verify(pthread_mutex_lock(&act_system->actors.arr[curr_actor].mutex),
                   "mutex lock failed");
            if (message_queue_is_empty(&act_system->actors.arr[curr_actor].queue))
                break;
        }
        if (!message_queue_is_empty(&act_system->actors.arr[curr_actor].queue)) {
            verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
            assert(!actors_queue_is_full(&act_system->act_queue));
            actors_queue_push(&act_system->act_queue, curr_actor);
            verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
        }
        act_system->actors.arr[curr_actor].worked_at = false;
        verify(pthread_mutex_unlock(&act_system->actors.arr[curr_actor].mutex),
               "mutex unlock failed");
    }
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    --act_system->alive_threads;
    if (act_system->interrupted && act_system->alive_threads == 0) {
        verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
        actor_system_destroy();
        raise(SIGINT);
    } else
        verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");

    debug(printf("Thread %lu finished!\n", pthread_self() % 100));
    return NULL;
}

static void interrupt() {
    int err;
    debug(fputs("Interrupted!", stderr));
    act_system->interrupted = true;
    act_system->alive_actors = 0;
    for (size_t i = 0; i < act_system->actors.size; ++i) {
        act_system->actors.arr[i].gone_die = true;
    }
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    verify(pthread_cond_broadcast(&act_system->new_request), "cond broadcast failed");
    verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
}

int actor_system_create(actor_id_t *actor, role_t *const role) {
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
    sigact.sa_flags = 0;
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
    pthread_mutex_destroy(&act_system->mutex);
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

void actor_system_join(actor_id_t actor) {
    int err;
    if (act_system == NULL)
        return;
    if (actor >= 0 && actor < (actor_id_t)act_system->actors.size) {
        for (size_t i = 0; i < POOL_SIZE; ++i)
            verify(pthread_join(act_system->pool[i], NULL), "join failed");
        actor_system_destroy();
    }
}

int send_message(actor_id_t actor, message_t message) {
    int err;
    if (actor >= (actor_id_t)act_system->actors.size)
        return -2;
    if (act_system->actors.arr[actor].gone_die)
        return -1;

    debug(printf("Sending message of type %li to actor %li...\n", message.message_type, actor));
    verify(pthread_mutex_lock(&act_system->actors.arr[actor].mutex), "mutex lock failed");
    bool was_empty = !act_system->actors.arr[actor].worked_at &&
            message_queue_is_empty(&act_system->actors.arr[actor].queue);

    assert(!message_queue_is_full(&act_system->actors.arr[actor].queue));
    message_queue_push(&act_system->actors.arr[actor].queue, message);
    verify(pthread_mutex_unlock(&act_system->actors.arr[actor].mutex), "mutex unlock failed");
    debug(printf("Sent message to actor %li.\n", actor));

    if (was_empty) {
        verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
        assert(!actors_queue_is_full(&act_system->act_queue));
//        debug(printf("Actors queue before push: beg: %lu, end: %lu, size: %lu, cap: %lu\n", act_system->act_queue.beg,
//                act_system->act_queue.end, act_system->act_queue.size, act_system->act_queue.capacity));
        actors_queue_push(&act_system->act_queue, actor);
//        debug(printf("Actors queue after push: beg: %lu, end: %lu, size: %lu, cap: %lu\n", act_system->act_queue.beg,
//                act_system->act_queue.end, act_system->act_queue.size, act_system->act_queue.capacity));
        debug(printf("Pushed actor %li to actors queue.\n", actor));
        verify(pthread_cond_signal(&act_system->new_request), "cond signal failed");
        verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
    }
    return 0;
}
