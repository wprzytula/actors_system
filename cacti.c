#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include "err.h"
#include "cacti.h"
#include "message_queue.h"
#include "actors_queue.h"

// TODO: delete this
#include <stdio.h>

/* Actor state struct & operations */
typedef struct {
    int gone_die;
    void *state;
    actor_id_t id;
    role_t role;
    pthread_mutex_t mutex;
    pthread_cond_t message_cond;
    message_queue_t queue;
} act_state_t;

int act_state_init(act_state_t *const state, role_t *const role, actor_id_t new_id) {
    if (role == NULL)
        return -1;
    if (pthread_mutex_init(&state->mutex, NULL) != 0) // TODO : mutexattr
        return -1;
    if (pthread_cond_init(&state->message_cond, NULL) != 0)
        return -1;
    state->gone_die = false;
    state->id = new_id;
    state->role = *role;
    state->state = NULL;
    // TODO : message_queue
    return 0;
}

void act_state_destroy(act_state_t *const state) {
    int err;
    verify(pthread_mutex_destroy(&state->mutex),"mutex destruction failed.");
    verify(pthread_cond_destroy(&state->message_cond),"cond destruction failed.");
}

/* Actor states array struct & operations */
typedef struct {
    size_t capacity;
    size_t size;
    act_state_t *arr;
} act_state_arr;

int act_state_arr_init(act_state_arr *const arr) {
    if (arr == NULL)
        return 1;
    arr->capacity = 16;
    arr->arr = malloc(arr->capacity * sizeof(act_state_t));
    arr->size = 0;
    return arr->arr == NULL;
}

int act_state_arr_emplace(act_state_arr *const arr, role_t *const role, actor_id_t new_id) {
    if (arr == NULL || role == NULL)
        return -1;
    if (arr->capacity == arr->size) {
        arr->capacity *= 2;
        // HERE WE GO - we act as a privileged Writer in Readers & Writers problem
        if ((arr->arr = realloc(arr->arr, arr->capacity)) == NULL)
            return -1;
    }
    return act_state_init(arr->arr + arr->size++, role, new_id);
}

void act_state_arr_destroy(act_state_arr *arr) {
    if (arr == NULL)
        return;
    for (size_t i = 0; i < arr->size; ++i) {
        act_state_destroy(arr->arr + i);
    }
    free(arr->arr);
}


/* Thread state structure */
typedef size_t thread_id_t;

struct thread_state {
    pthread_t pthread;
    actor_id_t caller;
};

/* Actor system structure & operations */
struct actor_system {
    struct thread_state pool[POOL_SIZE];
    pthread_mutex_t mutex;
    pthread_cond_t new_request;
    pthread_cond_t finished;
    act_state_arr actors;
    actor_id_t max_curr_id;
    size_t alive_actors;
    actors_queue_t act_queue;
    // TODO
};

struct actor_system *act_system = NULL;

void* worker(void *data) {
    int err;
    size_t num = *(size_t*)data;
//    pthread_key_t key;
//    pthread_key_create(&key, NULL);
//    pthread_setspecific(key, data);
    while (act_system->act_queue.size > 0 || act_system->alive_actors > 0) {
        verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
        while (act_system->act_queue.size == 0) {
            verify(pthread_cond_wait(&act_system->new_request, &act_system->mutex),
                    "cond wait failed");
        }
    }

    return NULL;
}

int actor_system_create(actor_id_t *actor, role_t *const role) {
    if (act_system != NULL)
        return -1;
    act_system = malloc(sizeof(struct actor_system));
    if (act_system == NULL)
        goto MAIN_MALLOC_FAILED;
    if (act_state_arr_init(&act_system->actors) != 0) {
        goto ACT_STATE_ARR_INIT_FAILED;
    }
    if (act_state_arr_emplace(&act_system->actors, role, 0) != 0)
        goto ACT_STATE_INIT_FAILED;
    if (actors_queue_init(&act_system->act_queue, CAST_LIMIT) != 0)
        goto ACTOR_QUEUE_INIT_FAILED;
    if (message_queue_init(&act_system->actors.arr[0].queue, ACTOR_QUEUE_LIMIT) != 0)
        goto MESSAGE_QUEUE_INIT_FAILED;
    if (pthread_mutex_init(&act_system->mutex, NULL) != 0)
        goto MUTEX_INIT_FAILED;
    if (pthread_cond_init(&act_system->finished, NULL) != 0)
        goto FINISHED_INIT_FAILED;
    if (pthread_cond_init(&act_system->new_request, NULL) != 0)
        goto NEW_REQUEST_INIT_FAILED;

    act_system->max_curr_id = 0;
    act_system->alive_actors = 0;
    *actor = 0;
    puts("System created!");
    return 0;

NEW_REQUEST_INIT_FAILED:
    pthread_cond_destroy(&act_system->finished);
FINISHED_INIT_FAILED:
    pthread_mutex_destroy(&act_system->mutex);
MUTEX_INIT_FAILED:
    message_queue_destroy(&act_system->actors.arr[0].queue);
MESSAGE_QUEUE_INIT_FAILED:
    actors_queue_destroy(&act_system->act_queue);
ACTOR_QUEUE_INIT_FAILED:
ACT_STATE_INIT_FAILED:
    act_state_arr_destroy(&act_system->actors);
ACT_STATE_ARR_INIT_FAILED:
    free(act_system);
MAIN_MALLOC_FAILED:
    return -1;
}

int actor_system_destroy() {
    int retval = 0;
    if (act_system == NULL)
        return -1;
    // stop all threads in the pool
    for (size_t i = 0; i < POOL_SIZE; ++i) {
        if (act_system->pool[i].pthread != pthread_self()) {
            pthread_cancel(act_system->pool[i].pthread);
            if (pthread_join(act_system->pool[i].pthread, NULL) != 0)
                retval = -1;
        }
    }
    // free actor system structures
    pthread_cond_destroy(&act_system->new_request);
    pthread_cond_destroy(&act_system->finished);
    pthread_mutex_destroy(&act_system->mutex);
    message_queue_destroy(&act_system->actors.arr[0].queue);
    actors_queue_destroy(&act_system->act_queue);
    act_state_arr_destroy(&act_system->actors);
    free(act_system);

    puts("System destroyed!");
    return retval;
}

void actor_system_join(actor_id_t actor) {
    int err;
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    while (actor >= 0 && actor <= act_system->max_curr_id) {
        verify(pthread_cond_wait(&act_system->finished, &act_system->mutex),
                "cond wait failed");
    }
    verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
    actor_system_destroy();
}

void spawn(actor_id_t *const new_actor, role_t *const role) {
    int err;
    verify(pthread_mutex_lock(&act_system->mutex), "mutex lock failed");
    *new_actor = ++act_system->max_curr_id;
    verify(act_state_arr_emplace(&act_system->actors, role, *new_actor), "malloc failed");
    verify(pthread_mutex_unlock(&act_system->mutex), "mutex unlock failed");
}

void process_message(actor_id_t actor, message_t message) {
    actor_id_t new_actor;
    switch (message.message_type) {
        case MSG_SPAWN:
            spawn(&new_actor, (role_t*)message.data);
            send_message(new_actor, (message_t){.message_type = MSG_HELLO,
                                                .nbytes = sizeof(actor_id_t),
                                                .data = (void*)actor});
            break;
        case MSG_GODIE:
            act_system->actors.arr[actor].gone_die = true;
            --act_system->alive_actors;
            break;
        /*case MSG_HELLO:
            break;*/
        default:
            act_system->actors.arr[actor].role.prompts[message.message_type]
            (&act_system->actors.arr[actor].state, message.nbytes, message.data);
    }
}

int send_message(actor_id_t actor, message_t message) {
    if (actor > act_system->max_curr_id)
        return -2;
    if (act_system->actors.arr[actor].gone_die)
        return -1;

    message_queue_push(&act_system->actors.arr[actor].queue, message); // TODO: malloc errors
    actors_queue_push(&act_system->act_queue, actor);
    return 0;
}