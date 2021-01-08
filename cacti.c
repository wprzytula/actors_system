#include <pthread.h>
#include <stdlib.h>

#include "cacti.h"

typedef struct {

} message_queue_t;

typedef struct {
    int gone_die;
    actor_id_t id;
    role_t role;
    pthread_mutex_t mutex;
    pthread_cond_t message_cond;
    // TODO : message_queue

} act_state_t;

int init_act_state(act_state_t *const state, role_t *const role, actor_id_t new_id) {
    if (role == NULL)
        return -1;
    if (pthread_mutex_init(&state->mutex, NULL) != 0) // TODO : mutexattr
        return -1;
    if (pthread_cond_init(&state->message_cond, NULL) != 0)
        return -1;
    state->gone_die = 0;
    state->id = new_id;
    state->role = *role;
    // TODO : message_queue
    return 0;
}

struct actor_system {
    act_state_t leader_actor;
    pthread_mutex_t wait_mutex;
    pthread_cond_t wait_until_finished;
    actor_id_t max_curr_id;
    // TODO
};


struct actor_system *act_system = NULL;


actor_id_t actor_id_self() {
    // TODO
}

int actor_system_create(actor_id_t *actor, role_t *const role) {
//    struct actor_system *system = NULL;
    if (act_system != NULL)
        return -1;
    act_system = malloc(sizeof(struct actor_system));
    if (act_system == NULL)
        return -1;
    if (init_act_state(&act_system->leader_actor, role, 0) != 0)
        return -1;
    act_system->max_curr_id = 0;
    *actor = 0;
    return 0;
}

void actor_system_join(actor_id_t actor) {
    if (actor <= act_system->max_curr_id)
        pthread_cond_wait(&act_system->wait_until_finished, &act_system->wait_mutex);
}

int send_message(actor_id_t actor, message_t message) {

}