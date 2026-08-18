#include "list.h"

#include "util.h"

#include <stdlib.h>

list *list_create(void) {
    list *l = xmalloc(sizeof(*l));
    l->head = NULL;
    l->tail = NULL;
    l->len = 0;
    return l;
}

void list_free(list *l, void (*free_val)(void *)) {
    if (!l) return;
    list_node *n = l->head;
    while (n) {
        list_node *next = n->next;
        if (free_val) free_val(n->val);
        free(n);
        n = next;
    }
    free(l);
}

size_t list_len(const list *l) {
    return l->len;
}

static list_node *list_new_node(void *val) {
    list_node *n = xmalloc(sizeof(*n));
    n->prev = NULL;
    n->next = NULL;
    n->val = val;
    return n;
}

void list_push_head(list *l, void *val) {
    list_node *n = list_new_node(val);
    n->next = l->head;
    if (l->head) l->head->prev = n;
    else l->tail = n;
    l->head = n;
    l->len++;
}

void list_push_tail(list *l, void *val) {
    list_node *n = list_new_node(val);
    n->prev = l->tail;
    if (l->tail) l->tail->next = n;
    else l->head = n;
    l->tail = n;
    l->len++;
}

void *list_pop_head(list *l) {
    if (!l->head) return NULL;
    list_node *n = l->head;
    l->head = n->next;
    if (l->head) l->head->prev = NULL;
    else l->tail = NULL;
    l->len--;
    void *val = n->val;
    free(n);
    return val;
}

void *list_pop_tail(list *l) {
    if (!l->tail) return NULL;
    list_node *n = l->tail;
    l->tail = n->prev;
    if (l->tail) l->tail->next = NULL;
    else l->head = NULL;
    l->len--;
    void *val = n->val;
    free(n);
    return val;
}

void *list_peek_head(const list *l) {
    return l->head ? l->head->val : NULL;
}

void *list_peek_tail(const list *l) {
    return l->tail ? l->tail->val : NULL;
}

void list_insert_after(list *l, list_node *node, void *val) {
    list_node *n = list_new_node(val);
    if (!node) {
        n->next = l->head;
        if (l->head) l->head->prev = n;
        else l->tail = n;
        l->head = n;
    } else {
        n->prev = node;
        n->next = node->next;
        if (node->next) node->next->prev = n;
        else l->tail = n;
        node->next = n;
    }
    l->len++;
}

void *list_detach(list *l, list_node *node) {
    if (!node) return NULL;
    if (node->prev) node->prev->next = node->next;
    else l->head = node->next;
    if (node->next) node->next->prev = node->prev;
    else l->tail = node->prev;
    l->len--;
    void *val = node->val;
    free(node);
    return val;
}

list_node *list_first(const list *l) {
    return l->head;
}

list_node *list_last(const list *l) {
    return l->tail;
}

list_node *list_next(const list_node *node) {
    return node ? node->next : NULL;
}

list_node *list_prev(const list_node *node) {
    return node ? node->prev : NULL;
}

list_node *list_node_at(const list *l, long long index) {
    long long n = (long long)l->len;
    if (index < 0) index += n;
    if (index < 0 || index >= n) return NULL;

    list_node *x;
    if (index <= n / 2) {
        x = l->head;
        for (long long i = 0; i < index; i++) x = x->next;
    } else {
        x = l->tail;
        for (long long i = n - 1; i > index; i--) x = x->prev;
    }
    return x;
}
