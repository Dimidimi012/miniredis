#ifndef MINIREDIS_LIST_H
#define MINIREDIS_LIST_H

#include <stddef.h>

/* A generic doubly-linked list of void* values (used to implement the LIST
 * type; the stored values are robj*). */
typedef struct list_node {
    struct list_node *prev;
    struct list_node *next;
    void *val;
} list_node;

typedef struct list {
    list_node *head;
    list_node *tail;
    size_t len;
} list;

list *list_create(void);
void list_free(list *l, void (*free_val)(void *));
size_t list_len(const list *l);

void list_push_head(list *l, void *val);
void list_push_tail(list *l, void *val);
void *list_pop_head(list *l);
void *list_pop_tail(list *l);
void *list_peek_head(const list *l);
void *list_peek_tail(const list *l);

/* Insert `val` after `node` (or at the head when node == NULL). */
void list_insert_after(list *l, list_node *node, void *val);
/* Unlink `node` and return its value; the caller owns the value. */
void *list_detach(list *l, list_node *node);

list_node *list_first(const list *l);
list_node *list_last(const list *l);
list_node *list_next(const list_node *node);
list_node *list_prev(const list_node *node);
/* Indexed access; supports negative indices (-1 = tail). NULL if out of range. */
list_node *list_node_at(const list *l, long long index);

#endif /* MINIREDIS_LIST_H */
