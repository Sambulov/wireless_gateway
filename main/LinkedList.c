#include "LinkedList.h"

#define LINKED_LIST_VALIDATION    ((uint16_t)(0xD639A5BC))

typedef struct __LL_ITEM_T {
	uint32_t ovn;
	struct __LL_ITEM_T *next;
	struct __LL_ITEM_T *prev;
	LinkedList_t *list;
} __LinkedListItem_t;

LIB_ASSERRT_STRUCTURE_CAST(__LinkedListItem_t, LinkedListItem_t, LL_ITEM_SIZE, LinkedList.h);

#define _bIsValidItem(it)         ((it != libNULL) && ((it)->ovn == LINKED_LIST_VALIDATION))

static void _vLinkBefore(__LinkedListItem_t *pxCurrentItem, __LinkedListItem_t *pxNew) {
	__LinkedListItem_t* itemBeforeCurrent = pxCurrentItem->prev;
	pxNew->next = pxCurrentItem;
	pxNew->prev = itemBeforeCurrent;
	pxNew->list = pxCurrentItem->list;
	pxCurrentItem->prev = pxNew;
	itemBeforeCurrent->next = pxNew;
}

void vLinkedListUnlink(LinkedListItem_t *pxItem) {
	__LinkedListItem_t *item = (__LinkedListItem_t *)pxItem;
	if (_bIsValidItem(item)) {
		if (item->list != libNULL) {
			if (*(item->list) == pxItem) {
				if (item->next == item) {
					*item->list = libNULL;
				}
				else {
					*item->list = (LinkedList_t)item->next;
				}
			}
			__LinkedListItem_t* prevItem = item->prev;
			__LinkedListItem_t* nextItem = item->next;
			prevItem->next = item->next;
			nextItem->prev = item->prev;
		}
	}
	if(item != libNULL) {
		item->ovn = LINKED_LIST_VALIDATION;
		item->next = item;
		item->prev = item;
		item->list = libNULL;
	}
}

void vLinkedListInsert(LinkedList_t *pxList, LinkedListItem_t *pxItem, ListItemComparer_t pfCmp) {
	if ((pxItem == libNULL) || (pxList == libNULL)) {
		return;
	}
	__LinkedListItem_t *newItem = (__LinkedListItem_t *)pxItem;
	vLinkedListUnlink(pxItem);
	if (*pxList == libNULL) {
		*pxList = pxItem;
		newItem->list = pxList;
	}
	else {
		__LinkedListItem_t *listCurrentItem = (__LinkedListItem_t *)*pxList;
		if (!_bIsValidItem(listCurrentItem)) 
			return;
		if (pfCmp != libNULL) {
			while (pfCmp(pxItem, (LinkedListItem_t *)listCurrentItem) > 0) {
				listCurrentItem = listCurrentItem->next;
				if (listCurrentItem == (__LinkedListItem_t *)*pxList) {
					break;
				}
			}
		}
		_vLinkBefore(listCurrentItem, newItem);		
	}
}

uint8_t bLinkedListContains(LinkedList_t xList, LinkedListItem_t *pxItem) {
	__LinkedListItem_t *pxItemPr = (__LinkedListItem_t *)pxItem;
	__LinkedListItem_t *pxListPr = (__LinkedListItem_t *)xList;
	return _bIsValidItem(pxListPr) && _bIsValidItem(pxItemPr) && 
		(pxListPr->list == pxItemPr->list);	
}

static LinkedListItem_t *_pxLinkedListFind(__LinkedListItem_t *pxCurrentItem, LinkedListMatch_t pfMatch, void *pxMatchArg, uint8_t bFirst, uint8_t bOverlap) {
  if (_bIsValidItem(pxCurrentItem)) {
    __LinkedListItem_t *first = *(__LinkedListItem_t **)pxCurrentItem->list;
    if((first == libNULL) || !bFirst) {
      first = pxCurrentItem->next;
      if(((first == libNULL) || (first == *(__LinkedListItem_t **)pxCurrentItem->list)) && !bOverlap)
        return libNULL;
      }
    __LinkedListItem_t *last = first->prev;
    if(!bOverlap && (pxCurrentItem->list != libNULL))
      last = (*(__LinkedListItem_t **)pxCurrentItem->list)->prev;
	pxCurrentItem = first;
	while (1) {
		if ((pfMatch == libNULL) || pfMatch((LinkedListItem_t *)pxCurrentItem, pxMatchArg))
			return (LinkedListItem_t *)pxCurrentItem;
		if (pxCurrentItem == last)
			break;
		pxCurrentItem = pxCurrentItem->next;
	}
  }
  return libNULL;
}

LinkedListItem_t *pxLinkedListFindFirst(LinkedList_t xList, LinkedListMatch_t pfMatch, void *pxMatchArg) {
	return _pxLinkedListFind((__LinkedListItem_t *)xList, pfMatch, pxMatchArg, 1, 0);
}

LinkedListItem_t *pxLinkedListFindNextOverlap(LinkedListItem_t *pxCurrentItem, LinkedListMatch_t pfMatch, void *pxMatchArg) {
	return _pxLinkedListFind((__LinkedListItem_t *)pxCurrentItem, pfMatch, pxMatchArg, 0, 1);
}

LinkedListItem_t *pxLinkedListFindNextNoOverlap(LinkedListItem_t *pxCurrentItem, LinkedListMatch_t pfMatch, void *pxMatchArg) {
	return _pxLinkedListFind((__LinkedListItem_t *)pxCurrentItem, pfMatch, pxMatchArg, 0, 0);
}

uint32_t ulLinkedListDoForeach(LinkedList_t xList, LinkedListAction_t fAction, void *pxArg) {
	__LinkedListItem_t *item = (__LinkedListItem_t *)xList;
	uint32_t amount = 0;
	if ((fAction != libNULL) && _bIsValidItem(item)) {
		__LinkedListItem_t **pxRoot = (__LinkedListItem_t **)item->list;
		__LinkedListItem_t *currentItem = (pxRoot != libNULL) ? *pxRoot : item;
		do {
			__LinkedListItem_t *nextItem = (currentItem->next == (__LinkedListItem_t *)*pxRoot) ? libNULL : currentItem->next;
			fAction((LinkedListItem_t *)currentItem, pxArg);
			currentItem = nextItem;
			amount++;
		} while ((currentItem != libNULL) && (*pxRoot != libNULL) && (currentItem->list == (LinkedList_t *)pxRoot));
	}
	return amount;
}

// void vLinkedListDoWhile(LinkedList_t xList, LinkedListMatch_t fAction, void *pxArg) {
// 	__LinkedListItem_t *item = (__LinkedListItem_t *)xList;
// 	if ((fAction != libNULL) && _bIsValidItem(item)) {
// 		__LinkedListItem_t **pxRoot = (__LinkedListItem_t **)item->list;
// 		__LinkedListItem_t *currentItem = (pxRoot != libNULL) ? *pxRoot : item;
// 		uint8_t res;
// 		do {
// 			__LinkedListItem_t *nextItem = (currentItem->next == (__LinkedListItem_t *)*pxRoot) ? libNULL : currentItem->next;
// 			res = fAction((LinkedListItem_t *)currentItem, pxArg);
// 			currentItem = nextItem;
// 		} while (res && (currentItem != libNULL) && (*pxRoot != libNULL) && (currentItem->list == (LinkedList_t *)pxRoot));
// 	}
// }

typedef struct {
	LinkedListMatch_t fMatch;
	void *pvMatchArg;
	uint32_t ulCounter;
} __CounterArgTuple_t;

static inline void _vCountForeachWrap(LinkedListItem_t *pxItem, void *pxArg) {
	__CounterArgTuple_t *arg = (__CounterArgTuple_t *)pxArg;
	if ((arg->fMatch == libNULL) || (arg->fMatch(pxItem, arg->pvMatchArg))) {
		(arg->ulCounter)++;
	}
}

uint32_t ulLinkedListCount(LinkedList_t xList, LinkedListMatch_t pfMatch, void *pxMatchArg) {
	__CounterArgTuple_t arg = { .fMatch = pfMatch, .pvMatchArg = pxMatchArg, .ulCounter = 0 };
	ulLinkedListDoForeach(xList, &_vCountForeachWrap, &arg);
	return arg.ulCounter;
}

static inline void _vUnlinkForeachWrap(LinkedListItem_t *pxItem, void *pxArg) {
	(void)pxArg;
	vLinkedListUnlink(pxItem);
}

void vLinkedListClear(LinkedList_t xList) {
	ulLinkedListDoForeach(xList, &_vUnlinkForeachWrap, libNULL);
}


linked_list_item_t *linked_list_find_first(linked_list_t, linked_list_match_t, void *)\
                                                      __attribute__ ((alias ("pxLinkedListFindFirst")));

linked_list_item_t *linked_list_find_next_overlap(linked_list_item_t *, linked_list_match_t, void *)\
                                                      __attribute__ ((alias ("pxLinkedListFindNextOverlap")));

linked_list_item_t *linked_list_find_next_no_overlap(linked_list_item_t *, linked_list_match_t, void *)\
                                                      __attribute__ ((alias ("pxLinkedListFindNextNoOverlap")));

uint32_t linked_list_do_foreach(linked_list_t, linked_list_action_t, void *)\
                                                      __attribute__ ((alias ("ulLinkedListDoForeach")));

void linked_list_insert(linked_list_t *, linked_list_item_t *, list_item_comparer_t)\
                                                      __attribute__ ((alias ("vLinkedListInsert")));

void linked_list_insert_last(linked_list_t *, linked_list_item_t *)\
                                                      __attribute__ ((alias ("vLinkedListInsertLast")));

void linked_list_unlink(linked_list_item_t *)     __attribute__ ((alias ("vLinkedListUnlink")));

uint32_t linked_list_count(linked_list_t, linked_list_match_t, void *)\
                                                      __attribute__ ((alias ("ulLinkedListCount")));

void linked_list_clear(LinkedList_t *) __attribute__ ((alias ("vLinkedListClear")));

uint8_t linked_list_contains(linked_list_t, linked_list_item_t *)\
                                                      __attribute__ ((alias ("bLinkedListContains")));
