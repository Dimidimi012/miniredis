#ifndef MINIREDIS_RDB_H
#define MINIREDIS_RDB_H

#include "db.h"

/* RDB snapshot format (documented; all multi-byte integers big-endian):
 *
 *   "MINIREDIS"         9-byte magic
 *   u32 version         = 1
 *   u32 key count
 *   per key:
 *     u32 key length, key bytes
 *     s64 expire_at     (ms since epoch; -1 = no expiry)
 *     u8  type          (OBJ_STRING..OBJ_ZSET)
 *     payload:
 *       STRING: u32 len, bytes
 *       LIST:   u32 count, { u32 len, bytes } x count
 *       HASH:   u32 count, { u32 flen, field, u32 vlen, value } x count
 *       SET:    u32 count, { u32 len, bytes } x count
 *       ZSET:   u32 count, { u32 len, member, f64 score } x count
 *
 * f64 is stored as the IEEE-754 bit pattern in big-endian byte order.
 *
 * Saves are atomic: the data is written to "<path>.tmp" and renamed over the
 * target file. */

/* Write a snapshot to `path` (atomically). Returns 0 on success, -1 on error. */
int rdb_save(const char *path, const db *store);

/* Load a snapshot into `store` (appending to any existing keys).
 * Returns 0 on success, -1 on error (partial data may have been loaded). */
int rdb_load(const char *path, db *store);

#endif /* MINIREDIS_RDB_H */
