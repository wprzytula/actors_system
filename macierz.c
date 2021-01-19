#include "cacti.h"
#include "err.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

struct field {
    int v;
    int t;
};

struct state {
    int n;
    int k;
    int my_col;
    actor_id_t child;
    actor_id_t leader;
    struct field** matrix;
};

struct container {
    int curr_row;
    int sum;
};

const int MSG_INTRO = 0x1;
const int MSG_COMP = 0x2;


void spawn(void **stateptr, size_t nbytes, void *data);
void hello(void **stateptr, size_t nbytes, void *data);
void introduce(void **stateptr, size_t nbytes, void *data);
act_t prompts[] = {hello, introduce};
role_t role = {.nprompts = 2, .prompts = prompts};

void spawn(void **stateptr, size_t nbytes, void *data) {

}

void hello(void **stateptr, __attribute__((unused)) size_t nbytes, void *data) {
    *stateptr = malloc(sizeof(struct state));
    ((struct state*)*stateptr)->leader = (actor_id_t)data;
    send_message((actor_id_t)data, (message_t)
        {.message_type = MSG_INTRO, .nbytes = sizeof(actor_id_t),
         .data = (void*)actor_id_self()});

//    printf("My stateptr is %li.\n", *(actor_id_t*)stateptr);
//    send_message((actor_id_t)data, (message_t){.message_type = });
}
void introduce(void **stateptr, __attribute__((unused)) size_t nbytes, void *data) {
    ((struct state*)*stateptr)->child = (actor_id_t)data;

//    send_message(*(actor_id_t*)stateptr, (message_t)
//        {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role});

}

void compute_row(void **stateptr, __attribute__((unused)) size_t nbytes, void *data) {
    struct field f = ((struct state*)*stateptr)->matrix
                        [((struct container*)data)->curr_row]
                        [((struct state*)*stateptr)->my_col];
    ((struct container*)data)->sum += f.v;
    usleep(1000 * f.t);
    send_message(((struct state*)*stateptr)->child, (message_t)
            {.message_type = MSG_COMP, .nbytes = sizeof(struct container),
                    .data = data});
    if ()
}

int main() {
    actor_id_t leader_actor;
    role.nprompts = 2;
    role.prompts = prompts;

    int n, k;

    scanf("%d", &n);
    scanf("%d", &k);

    struct field **matrix = malloc(n * sizeof(struct field*));
    if (!matrix)
        fatal("malloc failed");
    for (int i = 0; i < n; ++i) {
        matrix[i] = malloc(k * sizeof(struct field));
        if (!matrix[i])
            fatal("malloc failed");
        for (int j = 0; j < k; ++k) {
            scanf("%d", &matrix[i][j].v);
            scanf("%d", &matrix[i][j].t);
        }
    }

    actor_system_create(&leader_actor, &role);
    send_message(leader_actor, (message_t)
        {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role});
    actor_system_join(leader_actor);

	return 0;
}
