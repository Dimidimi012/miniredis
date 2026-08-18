#ifndef MINIREDIS_DB_H
#define MINIREDIS_DB_H

#include <stddef.h>

#include "dict.h"
#include "resp.h"

struct client;   /* full definition in server.h */

typedef struct db db;

db *db_create(void);
void db_free(db *db);
size_t db_size(const db *db);

void dispatch_command(db *store, struct client *c, command *cmd);

#endif /* MINIREDIS_DB_H */
