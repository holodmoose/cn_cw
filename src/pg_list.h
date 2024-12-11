/*-------------------------------------------------------------------------
 *
 * pg_list.h
 *	  interface for PostgreSQL generic linked list package
 *
 * This package implements singly-linked homogeneous lists.
 *
 * It is important to have constant-time length, append, and prepend
 * operations. To achieve this, we deal with two distinct data
 * structures:
 *
 *		1. A set of "list cells": each cell contains a data field and
 *		   a link to the next cell in the list or NULL.
 *		2. A single structure containing metadata about the list: the
 *		   type of the list, pointers to the head and tail cells, and
 *		   the length of the list.
 *
 * We support three types of lists:
 *
 *	T_List: lists of pointers
 *		(in practice usually pointers to Nodes, but not always;
 *		declared as "void *" to minimize casting annoyances)
 *	T_IntList: lists of integers
 *	T_OidList: lists of Oids
 *
 * (At the moment, ints and Oids are the same size, but they may not
 * always be so; try to be careful to maintain the distinction.)
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pg_list.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LIST_H
#define PG_LIST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct ListCell ListCell;

typedef struct List {
    int length;
    ListCell *head;
    ListCell *tail;
} List;

struct ListCell {
    union {
        void *ptr_value;
        int int_value;
    } data;
    ListCell *next;
};

/*
 * The *only* valid representation of an empty list is NIL; in other
 * words, a non-NIL list is guaranteed to have length >= 1 and
 * head/tail != NULL
 */
#define NIL ((List *)NULL)

static inline ListCell *list_head(List *l) {
    return l ? l->head : NULL;
}

static inline ListCell *list_tail(List *l) {
    return l ? l->tail : NULL;
}

static inline int list_length(List *l) {
    return l ? l->length : 0;
}

/*
 * NB: There is an unfortunate legacy from a previous incarnation of
 * the List API: the macro lfirst() was used to mean "the data in this
 * cons cell". To avoid changing every usage of lfirst(), that meaning
 * has been kept. As a result, lfirst() takes a ListCell and returns
 * the data it contains; to get the data in the first cell of a
 * List, use linitial(). Worse, lsecond() is more closely related to
 * linitial() than lfirst(): given a List, lsecond() returns the data
 * in the second cons cell.
 */

#define lnext(lc) ((lc)->next)
#define lfirst(lc) ((lc)->data.ptr_value)
#define lfirst_int(lc) ((lc)->data.int_value)

#define linitial(l) lfirst(list_head(l))
#define linitial_int(l) lfirst_int(list_head(l))

#define lsecond(l) lfirst(lnext(list_head(l)))
#define lsecond_int(l) lfirst_int(lnext(list_head(l)))

#define lthird(l) lfirst(lnext(lnext(list_head(l))))
#define lthird_int(l) lfirst_int(lnext(lnext(list_head(l))))

#define lfourth(l) lfirst(lnext(lnext(lnext(list_head(l)))))
#define lfourth_int(l) lfirst_int(lnext(lnext(lnext(list_head(l)))))

#define llast(l) lfirst(list_tail(l))
#define llast_int(l) lfirst_int(list_tail(l))

/*
 * Convenience macros for building fixed-length lists
 */
#define list_make1(x1) lcons(x1, NIL)
#define list_make2(x1, x2) lcons(x1, list_make1(x2))
#define list_make3(x1, x2, x3) lcons(x1, list_make2(x2, x3))
#define list_make4(x1, x2, x3, x4) lcons(x1, list_make3(x2, x3, x4))

#define list_make1_int(x1) lcons_int(x1, NIL)
#define list_make2_int(x1, x2) lcons_int(x1, list_make1_int(x2))
#define list_make3_int(x1, x2, x3) lcons_int(x1, list_make2_int(x2, x3))
#define list_make4_int(x1, x2, x3, x4) lcons_int(x1, list_make3_int(x2, x3, x4))

/*
 * foreach -
 *	  a convenience macro which loops through the list
 */
#define foreach(cell, l) for (ListCell * (cell) = list_head(l); (cell) != NULL; (cell) = lnext(cell))

/*
 * for_each_cell -
 *	  a convenience macro which loops through a list starting from a
 *	  specified cell
 */
#define for_each_cell(cell, initcell) for (ListCell * (cell) = (initcell); (cell) != NULL; (cell) = lnext(cell))

/*
 * forboth -
 *	  a convenience macro for advancing through two linked lists
 *	  simultaneously. This macro loops through both lists at the same
 *	  time, stopping when either list runs out of elements. Depending
 *	  on the requirements of the call site, it may also be wise to
 *	  assert that the lengths of the two lists are equal.
 */
#define forboth(cell1, list1, cell2, list2)                                                                            \
    for (ListCell * (cell1) = list_head(list1), (cell2) = list_head(list2); (cell1) != NULL && (cell2) != NULL;        \
         (cell1) = lnext(cell1), (cell2) = lnext(cell2))

/*
 * forthree -
 *	  the same for three lists
 */
#define forthree(cell1, list1, cell2, list2, cell3, list3)                                                             \
    for (ListCell * (cell1) = list_head(list1), (cell2) = list_head(list2), (cell3) = list_head(list3);                \
         (cell1) != NULL && (cell2) != NULL && (cell3) != NULL;                                                        \
         (cell1) = lnext(cell1), (cell2) = lnext(cell2), (cell3) = lnext(cell3))

extern List *lappend(List *list, void *datum);
extern List *lappend_int(List *list, int datum);

extern ListCell *lappend_cell(List *list, ListCell *prev, void *datum);
extern ListCell *lappend_cell_int(List *list, ListCell *prev, int datum);

extern List *lcons(void *datum, List *list);
extern List *lcons_int(int datum, List *list);

extern List *list_concat(List *list1, List *list2);
extern List *list_truncate(List *list, int new_size);

extern void *list_nth(List *list, int n);
extern int list_nth_int(List *list, int n);

extern bool list_member_ptr(List *list, void *datum);
extern bool list_member_int(List *list, int datum);

extern List *list_delete_ptr(List *list, void *datum);
extern List *list_delete_int(List *list, int datum);
extern List *list_delete_first(List *list);
extern List *list_delete_cell(List *list, ListCell *cell, ListCell *prev);

extern List *list_union_ptr(List *list1, List *list2);
extern List *list_union_int(List *list1, List *list2);
extern List *list_union_oid(List *list1, List *list2);

extern List *list_difference_ptr(List *list1, List *list2);
extern List *list_difference_int(List *list1, List *list2);
extern List *list_difference_oid(List *list1, List *list2);

extern List *list_append_unique_ptr(List *list, void *datum);
extern List *list_append_unique_int(List *list, int datum);

extern List *list_concat_unique_ptr(List *list1, List *list2);
extern List *list_concat_unique_int(List *list1, List *list2);

extern List *list_copy(List *list);
extern List *list_copy_tail(List *list, int nskip);

extern void list_free(List *list);
extern void list_free_deep(List *list);


#endif /* PG_LIST_H */
