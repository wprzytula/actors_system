#include "cacti.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void say_hello(void **stateptr, size_t nbytes, void *data) {
    puts("Hey Hey!");
}
void introduce(void **stateptr, size_t nbytes, void *data) {
    printf("I'm being printed on %li actor.\n", actor_id_self());
}

int main() {
    actor_id_t leader_actor;
    role_t *role = malloc(sizeof(role_t));
//    role->prompts = malloc(2 * sizeof(act_t));
    role->nprompts = 2;
    act_t temp[2] = {say_hello, introduce};
    role->prompts = temp;
//    memcpy(role->prompts, temp, 2);
    actor_system_create(&leader_actor, role);
    send_message(leader_actor, (message_t){.message_type = MSG_HELLO});
//    send_message(leader_actor, (message_t){.message_type = 0x2});
    send_message(leader_actor, (message_t){.message_type = 0x1});
    act_t prompts[2] = {introduce, say_hello};
    role_t role2 = {.nprompts = 2, .prompts = prompts};
    send_message(leader_actor, (message_t)
    {.message_type = MSG_SPAWN, .nbytes = sizeof(role_t), .data = &role2});
    send_message(leader_actor, (message_t){.message_type = MSG_GODIE});
    actor_system_join(leader_actor);

//    free(role->prompts);
    free(role);
	return 0;
}
