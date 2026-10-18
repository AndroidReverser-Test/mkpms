#include "protocol_test_env.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int wx_page_shift=12;
void *current=(void *)1;
typeof(mm_struct_offset) mm_struct_offset;
void *(*kfunc_kzalloc)(unsigned long,unsigned int);
void (*kfunc_kfree)(void *);
unsigned long (*kfunc___get_free_pages)(unsigned int,unsigned int);
void (*kfunc_free_pages)(unsigned long,unsigned int);
static void *get_mm(void *t) { assert(t==current); return (void *)2; }
static void put_mm(void *m) { assert(m==(void *)2); }
void *(*kfunc_get_task_mm)(void *)=get_mm;
void (*kfunc_mmput)(void *)=put_mm;
void *kfunc_do_page_fault=(void *)1, *kfunc_dup_mmap=(void *)1, *kfunc_follow_page_pte=(void *)1;
struct wxshadow_page *wxshadow_find_page(void *m,unsigned long a) { return NULL; }
void wxshadow_page_put(struct wxshadow_page *p) {}
unsigned long kaddr_to_pfn(unsigned long a) { return a>>12; }
void *pfn_to_kaddr(unsigned long p) { return (void *)(p<<12); }
static bool copy_in_fails, copy_out_fails, active;
static unsigned int request_calls;
static unsigned long copy_in(void *to,const void *from,unsigned long size) {
  if(copy_in_fails) return size; memcpy(to,from,size); return 0;
}
static unsigned long copy_out(void *to,const void *from,unsigned long size) {
  if(copy_out_fails) return size; memcpy(to,from,size); return 0;
}
static unsigned long long random_value(void) { return 1234; }
unsigned long wxshadow_lookup_symbol(const char *name) {
  if(!strcmp(name,"get_random_u64")) return (unsigned long)random_value;
  if(!strcmp(name,"__kmalloc")) return (unsigned long)calloc;
  if(!strcmp(name,"copy_from_user_nofault")) return (unsigned long)copy_in;
  if(!strcmp(name,"copy_to_user_nofault")) return (unsigned long)copy_out;
  return 0;
}
int wx_linux_init(const struct wx_linux_host *h) { return 0; }
int wx_linux_available(void) { return 1; }
int wx_linux_owns(void *mm,unsigned long a) { return 0; }
int wx_linux_request(int command,void *req,void *mm) {
  ++request_calls; assert(mm==(void *)2);
  if(command==PR_WXSHADOW_PATCH_EX) { active=true; ((struct wx_patch_request *)req)->state=WX_PATCH_ACTIVE; }
  else {
    if(command==PR_WXSHADOW_RELEASE_EX) active=false;
    ((struct wx_identity *)req)->state=active ? WX_PATCH_ACTIVE : WX_PATCH_RETIRED;
  }
  return 0;
}
static long call(int command,void *r) { return wx_protocol_request(command,(unsigned long)r,0,0,0); }
int main(void) {
  wx_protocol_init();
  struct wx_query q={.header={1,sizeof(q),0,0}};
  assert(!call(PR_WXSHADOW_QUERY,&q) && q.instance==1234 && (q.capabilities & WX_CAP_REQUIRED)==WX_CAP_REQUIRED);
  q.header.flags=1; assert(call(PR_WXSHADOW_QUERY,&q)==-EINVAL); q.header.flags=0;
  q.header.size--; assert(call(PR_WXSHADOW_QUERY,&q)==-EINVAL); q.header.size++;
  q.header.version=2; assert(call(PR_WXSHADOW_QUERY,&q)==-EINVAL); q.header.version=1;
  q.reserved[1]=1; assert(call(PR_WXSHADOW_QUERY,&q)==-EINVAL); q.reserved[1]=0;
  assert(wx_protocol_request(PR_WXSHADOW_QUERY,(unsigned long)&q,1,0,0)==-EINVAL);
  struct wx_patch_request patch={.header={1,sizeof(patch),0,0},.instance=q.instance,.cookie=1,.address=4096};
  patch.instance++; assert(call(PR_WXSHADOW_PATCH_EX,&patch)==-ESTALE); patch.instance--;
  copy_in_fails=true; assert(call(PR_WXSHADOW_PATCH_EX,&patch)==-EFAULT && !request_calls); copy_in_fails=false;
  copy_out_fails=true; assert(call(PR_WXSHADOW_PATCH_EX,&patch)==-EFAULT && active && request_calls==1);
  struct wx_identity id={.header={1,sizeof(id),0,0},.instance=q.instance,.cookie=1,.address=4096};
  copy_out_fails=false; assert(!call(PR_WXSHADOW_QUERY_PATCH,&id) && id.state==WX_PATCH_ACTIVE);
  copy_out_fails=true; assert(call(PR_WXSHADOW_RELEASE_EX,&id)==-EFAULT && !active);
  copy_out_fails=false; assert(!call(PR_WXSHADOW_QUERY_PATCH,&id) && id.state==WX_PATCH_RETIRED);
  puts("Protocol validation, copy failures and queryable committed results passed");
}
