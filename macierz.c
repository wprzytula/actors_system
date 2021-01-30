#include "cacti.h"
#include "err.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>

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

struct rowsum {
    int curr_row;
    long long sum;
};

const int MSG_INTRO = 0x1;
const int MSG_ASSGN = 0x2;
const int MSG_READY = 0x3;
const int MSG_COMP = 0x4;

void hello(void **stateptr, size_t nbytes, void *data);
void introduce(struct state **stateptr, size_t nbytes, void *data);
void assign(struct state **stateptr, size_t nbytes, struct state *data);
void ready(struct state **stateptr, size_t nbytes, void *data);
void compute_row(struct state **stateptr, size_t nbytes, struct rowsum *data);

act_t prompts[] = {(act_t)hello, (act_t)introduce, (act_t)assign, (act_t)ready, (act_t)compute_row};
role_t role = {.nprompts = sizeof(prompts) / sizeof(act_t), .prompts = prompts};

void hello(__attribute__((unused)) void **stateptr, __attribute__((unused)) size_t nbytes,
        void *data) {
    send_message((actor_id_t)data, (message_t)
        {.message_type = MSG_INTRO, .nbytes = sizeof(actor_id_t),
         .data = (void*)actor_id_self()});
}

void introduce(struct state **stateptr, __attribute__((unused)) size_t nbytes,
        void *data) {
    (*stateptr)->child = (actor_id_t)data;
    struct state *child_stateptr = malloc(sizeof(struct state));
    if (!child_stateptr)
        fatal("malloc failed");
    child_stateptr->my_col = (*stateptr)->my_col + 1;
    child_stateptr->n = (*stateptr)->n;
    child_stateptr->k = (*stateptr)->k;
    child_stateptr->matrix = (*stateptr)->matrix;

    send_message((*stateptr)->child, (message_t)
            {.message_type = MSG_ASSGN, .nbytes = sizeof(struct state),
                    .data = child_stateptr});
}

void assign(struct state **stateptr, __attribute__((unused)) size_t nbytes,
        struct state *data) {
    *stateptr = data;
    if ((*stateptr)->my_col + 1 < (*stateptr)->k)
        send_message(actor_id_self(), (message_t)
                {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t),
                        .data = &role});
    else
        send_message((*stateptr)->leader, (message_t){.message_type = MSG_READY});

}

void ready(struct state **stateptr, __attribute__((unused)) size_t nbytes,
           __attribute__((unused)) void *data) {
    // if the actor handling is the leader, start computation for each row
    assert((*stateptr)->my_col == 0);
    for (int i = 0; i < (*stateptr)->n; ++i) {
        struct rowsum *sum = malloc(sizeof(struct rowsum));
        if (!sum)
            fatal("malloc failed");
        sum->curr_row = i;
        sum->sum = 0;
        send_message(actor_id_self(), (message_t)
                {.message_type = MSG_COMP, .nbytes = sizeof(struct rowsum), .data = sum});
    }
}

void compute_row(struct state **stateptr, __attribute__((unused)) size_t nbytes,
        struct rowsum *data) {
    bool free_data = false;

    struct field f = (*stateptr)->matrix[data->curr_row][(*stateptr)->my_col];
    data->sum += f.v;
    usleep(1000 * f.t);

    if ((*stateptr)->my_col + 1 < (*stateptr)->k) {
        send_message((*stateptr)->child, (message_t)
            {.message_type = MSG_COMP, .nbytes = sizeof(struct rowsum),
                    .data = data});
    } else {
        printf("%lld\n", data->sum);
        free_data = true;
    }
    if (data->curr_row + 1 == (*stateptr)->n) {
        free(*stateptr);
        send_message(actor_id_self(), (message_t){.message_type = MSG_GODIE});
    }
    if (free_data)
        free(data);
}

int main() {
    actor_id_t leader;

    int n, k;
    scanf("%d", &n);
    scanf("%d", &k);
    if (n <= 0 || k <= 0)
        fatal("Bad matrix dimensions");

    struct field **matrix = malloc(n * sizeof(struct field*));
    if (!matrix)
        fatal("malloc failed");
    for (int i = 0; i < n; ++i) {
        matrix[i] = malloc(k * sizeof(struct field));
        if (!matrix[i])
            fatal("malloc failed");
        for (int j = 0; j < k; ++j) {
            scanf("%d", &matrix[i][j].v);
            scanf("%d", &matrix[i][j].t);
        }
    }

    if (actor_system_create(&leader, &role) != 0)
        fatal("failed to create actor system");

    struct state *leader_state = malloc(sizeof(struct state));
    if (!leader_state)
        fatal("malloc failed");
    leader_state->k = k;
    leader_state->n = n;
    leader_state->my_col = 0;
    leader_state->matrix = matrix;
    leader_state->leader = leader;

    send_message(leader, (message_t)
        {.message_type = MSG_ASSGN, .nbytes = sizeof(struct state), .data = leader_state});

    actor_system_join(leader);

    for (int i = 0; i < n; ++i) {
        free(matrix[i]);
    }
    free(matrix);

    return 0;
}
