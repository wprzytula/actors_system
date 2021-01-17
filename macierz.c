#include "cacti.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void one(void **stateptr, size_t nbytes, void *data) {
    puts("Hey Hey!");
}
void two(void **stateptr, size_t nbytes, void *data) {
    puts("Hello!");
}

int main() {
    actor_id_t actor_id;
    role_t *role = malloc(sizeof(role_t));
    role->prompts = malloc(2 * sizeof(act_t));
    act_t temp[2] = {one, two};
    memcpy(role->prompts, temp, 2);
    actor_system_create(&actor_id, role);
    send_message(actor_id, (message_t){.message_type = MSG_GODIE});
    actor_system_join(actor_id);

    free(role->prompts);
    free(role);
	return 0;
}
