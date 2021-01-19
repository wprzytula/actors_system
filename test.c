#include "cacti.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <zconf.h>
#include <signal.h>

void hello(void **stateptr, size_t nbytes, void *data);
void introduce(void **stateptr, size_t nbytes, void *data);
act_t prompts[2] = {say_hello, introduce};
role_t role = {.nprompts = 2, .prompts = prompts};

void hello(void **stateptr, size_t nbytes, void *data) {
    puts("Hey Hey!");
    *stateptr = data;
    printf("My stateptr is %li.\n", *(actor_id_t*)stateptr);
//    **(actor_id_t**)stateptr = (actor_id_t)data;
    send_message(actor_id_self(), (message_t){.message_type = 0x1});
}
void introduce(void **stateptr, size_t nbytes, void *data) {
    printf("I'm being printed on %li actor.\n", actor_id_self());
    printf("My parent is %li.\n", *(actor_id_t*)stateptr);

    if (actor_id_self() < 10)
        send_message(*(actor_id_t*)stateptr, (message_t)
            {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role});
}

int main() {
    actor_id_t leader_actor;
    role.nprompts = 2;
//    memcpy(role->prompts, temp, 2);
    actor_system_create(&leader_actor, &role);
//    sleep(1);
//    send_message(leader_actor, (message_t){.message_type = MSG_HELLO});
//    send_message(leader_actor, (message_t){.message_type = 0x2});
//    send_message(leader_actor, (message_t){.message_type = 0x1});
    send_message(leader_actor, (message_t)
    {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role});
//    send_message(leader_actor, (message_t){.message_type = MSG_GODIE});
    sleep(2);
//    fputs("Time's up!", stderr);
//    raise(SIGINT);
    actor_system_join(leader_actor);

	return 0;
}
