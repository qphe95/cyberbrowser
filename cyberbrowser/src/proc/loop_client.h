#ifndef LOOP_CLIENT_H
#define LOOP_CLIENT_H

/* The network process's side of the loopback relay (loop_client.c). */

#include <stdbool.h>
#include <windows.h>

/* Use the channel the coordinator granted (GRANT_LOOP) to reach loopback
 * hosts from now on.  False if it can't be used. */
bool loop_client_start(HANDLE section, HANDLE ev0, HANDLE ev1);

/* End a connection through the relay. */
void loop_client_close_id(int id);

#endif
