#ifndef __CR_SHMEM_H__
#define __CR_SHMEM_H__

#include "lock.h"
#include "protobuf/vma.pb-c.h"

struct _VmaEntry;
extern int collect_shmem(int pid, struct _VmaEntry *vi);
extern int prepare_shmem_restore(void);
extern void show_saved_shmems(void);
extern int get_shmem_fd(int pid, VmaEntry *vi);

extern unsigned long nr_shmems;
extern unsigned long rst_shmems;

extern int cr_dump_shmem(void);
extern int add_shmem_area(pid_t pid, VmaEntry *vma);

#endif /* __CR_SHMEM_H__ */
