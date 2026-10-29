#ifndef TLS_SPACE_H
#define TLS_SPACE_H

/* Private Network Access (the network process, SANDBOX_DESIGN.md section
 * 2.4).  With a classifier set, each connect resolves the host itself and
 * connects only to an address whose space is at most this thread's limit;
 * a pooled connection to a space above it isn't handed out.  The spaces are
 * the classifier's numbers (more private: higher).  limit -1: no limit. */
struct sockaddr;
typedef int (*TlsSpaceFn)(const struct sockaddr *addr);
void tls_set_space_policy(TlsSpaceFn classify);
void tls_set_space_limit(int limit);
/* The space of the connection this thread's last request used: -1 if
 * unknown (no classifier, or no connection: a replay). */
int tls_last_space(void);
void tls_reset_last_space(void);

#endif
