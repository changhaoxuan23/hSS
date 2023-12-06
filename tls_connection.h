#ifndef TLS_H_
#define TLS_H_
#include <common.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
void tls_initialize_underlying(struct connection_information *connection);
#endif