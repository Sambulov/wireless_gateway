/*!
    LinkedList.h
  
    Created on: 15.02.2017
        Author: Sambulov Dmitry
        e-mail: dmitry.sambulov@gmail.com
 */
#ifndef LINKED_LIST_H_INCLUDED
#define LINKED_LIST_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define libNULL NULL
#define LIB_ASSERRT_STRUCTURE_CAST(private_type, public_type, prv_size_def, def_file)   _Static_assert(sizeof(private_type) == sizeof(public_type), "In "#def_file" data structure size of "#public_type" doesn't match, check "#prv_size_def)
#define CL_PRIVATE(size)       uint32_t __private[(size + sizeof(void *) - 1) >> 2]


static inline void *__LinkedListCalcObjPtr(size_t llOffset, void *llPtr) { return (llPtr == libNULL) ? libNULL : ((uint8_t *)(llPtr) - llOffset); }

#define LinkedListGetObject(objType, listPtr)           ((objType *)__LinkedListCalcObjPtr(offsetof(objType, __ll),(listPtr)))
#define __LinkedListObject__                            LinkedListItem_t __ll;
#define LinkedListItem(pxObj)                           (&((pxObj)->__ll))

#define LL_ITEM_SIZE    16

	typedef struct LL_ITEM_T {
		CL_PRIVATE(LL_ITEM_SIZE);
	} LinkedListItem_t;

	typedef LinkedListItem_t* LinkedList_t;
	typedef int32_t(*ListItemComparer_t)(LinkedListItem_t *, LinkedListItem_t *);
	typedef void(*LinkedListAction_t)(LinkedListItem_t*, void *);
	typedef uint8_t(*LinkedListMatch_t)(LinkedListItem_t*, void *);

	LinkedListItem_t *pxLinkedListFindFirst(LinkedList_t xList, LinkedListMatch_t pfMatch, void *pxMatchArg);
	LinkedListItem_t *pxLinkedListFindNextOverlap(LinkedListItem_t *pxCurrentItem, LinkedListMatch_t pfMatch, void *pxMatchArg);
	LinkedListItem_t *pxLinkedListFindNextNoOverlap(LinkedListItem_t *pxCurrentItem, LinkedListMatch_t pfMatch, void *pxMatchArg);
	uint32_t ulLinkedListDoForeach(LinkedList_t xList, LinkedListAction_t fAction, void *pxArg);
	void vLinkedListDoWhile(LinkedList_t xList, LinkedListMatch_t fAction, void *pxArg);
	void vLinkedListInsert(LinkedList_t *pxList, LinkedListItem_t *pxItem, ListItemComparer_t pfCmp);
	static inline void vLinkedListInsertLast(LinkedList_t *pxList, LinkedListItem_t *pxItem) {
		vLinkedListInsert(pxList, pxItem, libNULL);
	}
	void vLinkedListUnlink(LinkedListItem_t *pxItem);
	uint32_t ulLinkedListCount(LinkedList_t xList, LinkedListMatch_t pfMatch, void *pxMatchArg);
	void vLinkedListClear(LinkedList_t xList);
	uint8_t bLinkedListContains(LinkedList_t xList, LinkedListItem_t *pxItem);

/*!
  Snake notation
*/

#define linked_list_get_object(obj_type, list_ptr)  LinkedListGetObject(obj_type, list_ptr)

#define __linked_list_object__                                __LinkedListObject__
#define linked_list_item(obj)                                 LinkedListItem(obj)

typedef LinkedListItem_t linked_list_item_t;
typedef LinkedListItem_t* linked_list_t;
typedef ListItemComparer_t list_item_comparer_t;
typedef LinkedListAction_t linked_list_action_t;
typedef LinkedListMatch_t linked_list_match_t;

linked_list_item_t *linked_list_find_first(linked_list_t linked_list, linked_list_match_t match_fn, void *search_args);
linked_list_item_t *linked_list_find_next_overlap(linked_list_item_t *current, linked_list_match_t match_fn, void *search_args);
linked_list_item_t *linked_list_find_next_no_overlap(linked_list_item_t *current, linked_list_match_t match_fn, void *search_args);
uint32_t linked_list_do_foreach(linked_list_t linked_list, linked_list_action_t action_fn, void *arg);
void linked_list_insert(linked_list_t *linked_list_ptr, linked_list_item_t *item, list_item_comparer_t comparer_fn);
void linked_list_insert_last(linked_list_t *linked_list_ptr, linked_list_item_t *item);
void linked_list_unlink(linked_list_item_t *item);
uint32_t linked_list_count(linked_list_t linked_list, linked_list_match_t match_fn, void *search_args);
void linked_list_clear(LinkedList_t *linked_list_ptr);
uint8_t linked_list_contains(linked_list_t linked_list, linked_list_item_t *item);

#ifdef __cplusplus
}
#endif

#endif /* LINKED_LIST_H_INCLUDED */
