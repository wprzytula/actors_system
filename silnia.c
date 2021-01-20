#include "cacti.h"
#include <stdio.h>
#include <stdlib.h>
#include "err.h"

typedef struct factorial {
    int n;
    int k;
    long long k_fact;
} fact_t;

const int MSG_COMP = 0x1;
const int MSG_PASS = 0x2;

void hello(void **stateptr, size_t nbytes, void *data);
void compute(fact_t **stateptr, size_t nbytes, fact_t *data);
void pass(void **stateptr, size_t nbytes, void *data);

role_t role;
act_t prompts[3] = {hello, (act_t)compute, pass};

void hello(__attribute__((unused)) void **stateptr,
        __attribute__((unused)) size_t nbytes, void *data) {
    send_message((actor_id_t)data, (message_t)
            {.message_type = MSG_PASS, .nbytes = sizeof(actor_id_t),
             .data = (void*)actor_id_self()});
}

void compute(fact_t **stateptr, __attribute__((unused)) size_t nbytes, fact_t *data) {
    *stateptr = malloc(sizeof(fact_t));
    if (!*stateptr)
        fatal("malloc failed");
    (*stateptr)->n = data->n;
    (*stateptr)->k = data->k + 1;
    (*stateptr)->k_fact = data->k_fact * (*stateptr)->k;
    free(data);
    debug(printf("Factorial computed in actor %ld is %lld\n", actor_id_self(),
            (*stateptr)->k_fact));
    if ((*stateptr)->k == (*stateptr)->n) {
        // print and clean
        printf("%lld\n", (*stateptr)->k_fact);
        free(*stateptr);
        send_message(actor_id_self(), (message_t){.message_type = MSG_GODIE});
    } else {
        send_message(actor_id_self(), (message_t)
        {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role});
    }
}

void pass(void **stateptr, __attribute__((unused)) size_t nbytes, void *data) {
    send_message((actor_id_t)data, (message_t)
            {.message_type = MSG_COMP, .nbytes = sizeof(fact_t), .data = *stateptr});
    send_message(actor_id_self(), (message_t){.message_type = MSG_GODIE});
}

int main() {
    int n;
    actor_id_t leader;
    role.prompts = prompts;
    role.nprompts = sizeof(prompts) / sizeof(act_t);

    scanf("%d", &n);

    if (n == 0) {
        puts("1");
        return 0;
    }
    if (n < 0) {
        fputs("Negative numbers are not allowed as factorial arguments!", stderr);
        exit(1);
    }

    fact_t *initial = malloc(sizeof(fact_t));
    if (!initial)
        fatal("malloc failed");
    *initial = (fact_t){.n = n, .k = 0, .k_fact = 1};
    if (actor_system_create(&leader, &role) != 0)
        fatal("failed to create actor system");
    send_message(leader, (message_t)
    {.message_type = MSG_COMP, .nbytes = sizeof(role_t), initial});

    actor_system_join(leader);

	return 0;
}
