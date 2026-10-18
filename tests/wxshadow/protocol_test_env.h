#ifndef WX_PROTOCOL_TEST_ENV_H
#define WX_PROTOCOL_TEST_ENV_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include "../../kpms/wxshadow/wxshadow_linux.h"
#define pr_info(...) ((void)0)
extern int wx_page_shift;
extern void *current;
extern struct { unsigned int pgd_offset; } mm_struct_offset;
extern void *(*kfunc_kzalloc)(unsigned long, unsigned int);
extern void (*kfunc_kfree)(void *);
extern unsigned long (*kfunc___get_free_pages)(unsigned int,unsigned int);
extern void (*kfunc_free_pages)(unsigned long,unsigned int);
extern void *(*kfunc_get_task_mm)(void *);
extern void (*kfunc_mmput)(void *);
extern void *kfunc_do_page_fault, *kfunc_dup_mmap, *kfunc_follow_page_pte;
struct wxshadow_page;
struct wxshadow_page *wxshadow_find_page(void *,unsigned long);
void wxshadow_page_put(struct wxshadow_page *);
unsigned long kaddr_to_pfn(unsigned long);
void *pfn_to_kaddr(unsigned long);
#endif
