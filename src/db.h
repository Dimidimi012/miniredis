#ifndef MINIREDIS_DB_H
#define MINIREDIS_DB_H

#include <stddef.h>

#include "dict.h"
#include "resp.h"

struct client;   /* full definition in server.h */

struct db {
    dict *d;              /* key -> robj* */
    size_t expire_cursor; /* bucket cursor for the periodic expire cycle */
};
typedef struct db db;

db *db_create(void);
void db_free(db *db);
size_t db_size(const db *db);

/* Active expiration: scan a bounded number of buckets (cursor-based, called
 * ~10x/sec from the event loop) and delete expired keys so they do not linger
 * in memory until accessed. */
void db_expire_cycle(db *db);

void dispatch_command(db *store, struct client *c, command *cmd);

#endif /* MINIREDIS_DB_H */
