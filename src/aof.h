#ifndef MINIREDIS_AOF_H
#define MINIREDIS_AOF_H

#include "db.h"
#include "resp.h"

/* Append-only log of the commands that mutated the dataset. Every write
 * command is appended (as RESP) before it is executed, so replaying the file
 * reproduces the same state. The file is fsynced on clean shutdown and, when
 * the server is running, once per second (appendfsync everysec).
 *
 * Note: relative expirations (EXPIRE / SET EX) are re-applied relative to the
 * replay time, so absolute expiry times can drift slightly after a restart
 * (same trade-off as Redis without AOF rewriting). */

/* Open (or create) the AOF file for appending. Returns the fd, or -1. */
int aof_open(const char *path);

/* fsync + close the AOF fd. */
void aof_close(int fd);

/* Append one command to the AOF. Returns 0 on success, -1 on error. */
int aof_append_command(int fd, const command *cmd);

/* Load and replay an AOF file into `store` (responses are discarded).
 * Returns 0 on success, -1 if the file was unreadable or corrupt. */
int aof_replay(const char *path, db *store);

/* Called from the event loop each iteration; fsyncs the AOF if >= 1s has
 * elapsed since the last fsync (appendfsync everysec). No-op when AOF is
 * disabled. */
void aof_periodic(void);

#endif /* MINIREDIS_AOF_H */
