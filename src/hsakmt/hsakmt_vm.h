/*
 * Copyright 2025 Advanced Micro Devices, Inc
 * SPDX-License-Identifier: MIT
 */

#ifndef HSAKMT_VM_H
#define HSAKMT_VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <hsakmt/hsakmt.h>

#include "util/rbtree.h"
#include "util/list.h"
#include "util/u_thread.h"

#define VHSA_1G_SIZE                 (0x40000000UL)
#define VHSA_CTX_RESERVE_SIZE        (32UL * VHSA_1G_SIZE)
#define VHSA_CTX_SCRATCH_SIZE        (0x100000000UL)
#define VHSA_MAX_CTX_SIZE            (5UL)
#define VHSA_DEV_RESERVE_SIZE        (VHSA_MAX_CTX_SIZE * VHSA_CTX_RESERVE_SIZE)
#define VHSA_DEV_SCRATCH_RESERVE_SIZE (VHSA_MAX_CTX_SIZE * VHSA_CTX_SCRATCH_SIZE)
#define VHSA_HEAP_INTERVAL_SIZE      (2UL * 1024UL * VHSA_1G_SIZE)

#define VIRTGPU_HSAKMT_CONTEXT_AMDGPU      1
#define VHSA_VAMGR_VM_TYPE_FIXED_BASE      1
#define VHSA_VAMGR_VM_TYPE_HEAP_INTERVAL_BASE 2
#define VHSA_FIXED_VM_BASE_ADDR            0x700000000000UL

#define hsakmt_container_of(ptr, type, member) \
   ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct vhsakmt_mem_frag {
   bool is_free : 1;
   bool is_list_head : 1;

   struct list_head head;
   void *dummy_list_head;

   rbtree_node_t rbt;
   rbtree_node_t free_frag_rbt;
} vhsakmt_mem_frag_t;

typedef struct hsakmt_vamgr {
   rbtree_t frag_tree;
   rbtree_t free_frag_tree;

   mtx_t frag_tree_lock;

   uint64_t vm_va_base_addr;
   uint64_t vm_va_high_addr;
   uint64_t reserve_size;
   uint64_t mem_used_size;

   bool dump_va;
} hsakmt_vamgr_t;

long vhsakmt_page_size(void);

int vhsakmt_init_vamgr(hsakmt_vamgr_t *mgr, uint64_t start, uint64_t size);

int vhsakmt_destroy_vamgr(hsakmt_vamgr_t *mgr);

uint64_t hsakmt_alloc_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t size);

uint64_t hsakmt_alloc_from_vamgr_aligned(hsakmt_vamgr_t *mgr, uint64_t size,
                                         uint64_t align);

int hsakmt_free_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t addr);

void hsakmt_set_dump_va(hsakmt_vamgr_t *mgr, int dump_va);

#endif /* HSAKMT_VM_H */
