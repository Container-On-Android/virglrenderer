/*
 * Copyright 2025 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "hsakmt_vm.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

long vhsakmt_page_size(void)
{
   return sysconf(_SC_PAGESIZE);
}

static vhsakmt_mem_frag_t *
vhsakmt_create_mem_frag(uint64_t addr, uint64_t size)
{
   vhsakmt_mem_frag_t *f = calloc(1, sizeof(vhsakmt_mem_frag_t));
   if (!f)
      return NULL;

   list_inithead(&f->head);
   f->is_free = true;
   f->is_list_head = false;
   f->rbt.key = rbtree_key(addr, size);
   /* key.addr is size in free_frag_tree */
   f->free_frag_rbt.key = rbtree_key(size, addr);

   return f;
}

static vhsakmt_mem_frag_t *
vhsakmt_create_free_frag_dummy_list_head(uint64_t addr, uint64_t size)
{
   vhsakmt_mem_frag_t *f = vhsakmt_create_mem_frag(addr, size);
   if (!f)
      return NULL;

   f->is_list_head = true;
   /* free fragment list head won't exist in fragment rbtree */
   f->rbt.key = rbtree_key(0, 0);
   return f;
}

static inline vhsakmt_mem_frag_t *
vhsakmt_free_frag_rbt_to_mem_frag(rbtree_node_t *n)
{
   return hsakmt_container_of(n, vhsakmt_mem_frag_t, free_frag_rbt);
}

static inline vhsakmt_mem_frag_t *vhsakmt_frag_rbt_to_mem_frag(rbtree_node_t *n)
{
   return hsakmt_container_of(n, vhsakmt_mem_frag_t, rbt);
}

static inline vhsakmt_mem_frag_t *vhsakmt_list_to_mem_frag(struct list_head *l)
{
   return hsakmt_container_of(l, vhsakmt_mem_frag_t, head);
}

static vhsakmt_mem_frag_t *
vhsakmt_insert_free_mem_frag(hsakmt_vamgr_t *mgr, uint64_t size, uint64_t addr,
                             vhsakmt_mem_frag_t *frag)
{
   rbtree_node_t *n = NULL;
   vhsakmt_mem_frag_t *f = NULL;
   vhsakmt_mem_frag_t *free_head_node = NULL;
   rbtree_key_t key;
   vhsakmt_mem_frag_t *new_f = NULL;

   if (frag) {
      f = frag;
      /* key.addr is size in free fragment tree */
      f->free_frag_rbt.key = rbtree_key(size, addr);
      list_inithead(&f->head);
   } else {
      new_f = vhsakmt_create_mem_frag(addr, size);
      if (!new_f)
         return NULL;
      f = new_f;
   }

   /* find the suitable size node, lookup by size for free_frag_tree */
   key = rbtree_key(size, 0);
   n = rbtree_lookup_nearest(&mgr->free_frag_tree, &key, LKP_ADDR, RIGHT);

   /* key.addr is size in free_frag_tree */
   if (n && n->key.addr == size) {
      /* if exists, insert into list */
      free_head_node = vhsakmt_free_frag_rbt_to_mem_frag(n);
      list_add(&f->head, &free_head_node->head);
   } else {
      /* not exist, insert into rbtree */
      free_head_node = vhsakmt_create_free_frag_dummy_list_head(addr, size);
      if (!free_head_node) {
         free(new_f);
         return NULL;
      }
      rbtree_insert(&mgr->free_frag_tree, &free_head_node->free_frag_rbt);
      list_add(&f->head, &free_head_node->head);
   }

   f->dummy_list_head = free_head_node;

   return f;
}

static vhsakmt_mem_frag_t *vhsakmt_remove_free_mem_frag(hsakmt_vamgr_t *mgr,
                                                        vhsakmt_mem_frag_t *f)
{
   vhsakmt_mem_frag_t *ret_frag = NULL;

   if (f->is_list_head)
      ret_frag = vhsakmt_list_to_mem_frag(f->head.next);
   else
      ret_frag = f;

   list_del(&ret_frag->head);

   if (list_is_empty(&((vhsakmt_mem_frag_t *)(ret_frag->dummy_list_head))->head)) {
      rbtree_delete(&mgr->free_frag_tree,
                    &((vhsakmt_mem_frag_t *)(ret_frag->dummy_list_head))->free_frag_rbt);
   }

   ret_frag->free_frag_rbt.key = rbtree_key(0, 0);
   ret_frag->dummy_list_head = NULL;

   return ret_frag;
}

static vhsakmt_mem_frag_t *vhsakmt_find_suitable_free_frag(hsakmt_vamgr_t *mgr,
                                                           uint64_t size)
{
   rbtree_node_t *n = NULL;
   rbtree_key_t key;
   vhsakmt_mem_frag_t *f;

   key = rbtree_key(size, 0);
   n = rbtree_lookup_nearest(&mgr->free_frag_tree, &key, LKP_ADDR, RIGHT);

   if (!n)
      return NULL;

   f = vhsakmt_free_frag_rbt_to_mem_frag(n);

   return vhsakmt_remove_free_mem_frag(mgr, f);
}

static vhsakmt_mem_frag_t *vhsakmt_find_suitable_free_frag_aligned(hsakmt_vamgr_t *mgr,
                                                                   uint64_t size,
                                                                   uint64_t align)
{
   rbtree_node_t *n = NULL;
   rbtree_key_t key;
   vhsakmt_mem_frag_t *f;
   uint64_t aligned_addr;
   uint64_t waste_before;

   /* start searching from the minimum size that could accommodate the request */
   key = rbtree_key(size, 0);
   n = rbtree_lookup_nearest(&mgr->free_frag_tree, &key, LKP_ADDR, RIGHT);

   while (n) {
      f = vhsakmt_free_frag_rbt_to_mem_frag(n);

      /* check all fragments in this size group */
      list_for_each_entry(vhsakmt_mem_frag_t, candidate, &f->head, head) {
         if (candidate->is_list_head)
            continue;

         /* calculate aligned address */
         aligned_addr = (candidate->rbt.key.addr + align - 1) & ~(align - 1);
         waste_before = aligned_addr - candidate->rbt.key.addr;

         /* check if this fragment can accommodate the aligned allocation */
         if (waste_before + size <= candidate->rbt.key.size) {
            return vhsakmt_remove_free_mem_frag(mgr, candidate);
         }
      }

      n = rbtree_next(&mgr->free_frag_tree, n);
   }

   return NULL;
}

static vhsakmt_mem_frag_t *vhsakmt_insert_mem_frag(hsakmt_vamgr_t *mgr,
                                                   vhsakmt_mem_frag_t *f)
{
   rbtree_insert(&mgr->frag_tree, &f->rbt);
   return f;
}

static int vhsakmt_add_free_mem_frag(hsakmt_vamgr_t *mgr, uint64_t size,
                                     uint64_t addr)
{
   vhsakmt_mem_frag_t *f = vhsakmt_insert_free_mem_frag(mgr, size, addr, NULL);
   if (!f)
      return -ENOMEM;
   vhsakmt_insert_mem_frag(mgr, f);

   return 0;
}

static void hsakmt_vamgr_dump_va(hsakmt_vamgr_t *mgr)
{
   if (!mgr->dump_va)
      return;

   mtx_lock(&mgr->frag_tree_lock);

   rbtree_node_t *ln = rbtree_min_max(&mgr->frag_tree, LEFT);
   vhsakmt_mem_frag_t *f;

   while (ln) {
      f = vhsakmt_frag_rbt_to_mem_frag(ln);
      printf("[0x%lx - 0x%lx] - 0x%lx: %s\n", f->rbt.key.addr,
             f->rbt.key.addr + f->rbt.key.size, f->rbt.key.size,
             f->is_free ? "free" : "used");

      ln = rbtree_next(&mgr->frag_tree, ln);
   }

   printf("vm status: 0x%lx / 0x%lx, used: %.2f%%\n", mgr->mem_used_size,
          mgr->reserve_size,
          ((double)mgr->mem_used_size / (double)mgr->reserve_size) * 100.0);

   mtx_unlock(&mgr->frag_tree_lock);
}

uint64_t hsakmt_alloc_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t size)
{
   const size_t page_size = (size_t)vhsakmt_page_size();

   if (size % page_size != 0)
      return 0;

   uint64_t addr = 0;
   uint64_t node_size = 0;
   vhsakmt_mem_frag_t *f = NULL;

   mtx_lock(&mgr->frag_tree_lock);

   f = vhsakmt_find_suitable_free_frag(mgr, size);
   if (!f)
      goto failed;

   /* suitable addr found */
   addr = f->rbt.key.addr;
   node_size = f->rbt.key.size;

   assert(node_size >= size);
   f->rbt.key.size = size;
   f->is_free = false;

   /* fragment split: add new fragment in frag tree and free frag tree */
   if (node_size > size) {
      if (vhsakmt_add_free_mem_frag(mgr, node_size - size, addr + size))
         goto failed;
   }

   mgr->mem_used_size += size;

   mtx_unlock(&mgr->frag_tree_lock);

   hsakmt_vamgr_dump_va(mgr);

   return addr;

failed:
   mtx_unlock(&mgr->frag_tree_lock);
   return 0;
}

uint64_t hsakmt_alloc_from_vamgr_aligned(hsakmt_vamgr_t *mgr, uint64_t size,
                                         uint64_t align)
{
   const size_t page_size = (size_t)vhsakmt_page_size();

   if (size % page_size != 0)
      return 0;

   /* alignment must be a power of 2 and at least page aligned */
   if (align == 0 || (align & (align - 1)) != 0 || align < page_size)
      return 0;

   uint64_t addr = 0;
   uint64_t aligned_addr = 0;
   uint64_t node_size = 0;
   uint64_t waste_before = 0;
   uint64_t waste_after = 0;
   vhsakmt_mem_frag_t *f = NULL;

   mtx_lock(&mgr->frag_tree_lock);

   f = vhsakmt_find_suitable_free_frag_aligned(mgr, size, align);
   if (!f)
      goto failed;

   /* calculate the aligned address and waste */
   addr = f->rbt.key.addr;
   node_size = f->rbt.key.size;
   aligned_addr = (addr + align - 1) & ~(align - 1);
   waste_before = aligned_addr - addr;
   waste_after = node_size - waste_before - size;

   /* create free fragment before the aligned allocation if there's waste */
   if (waste_before > 0) {
      if (vhsakmt_add_free_mem_frag(mgr, waste_before, addr))
         goto failed;
   }

   /* update current fragment to represent the allocated region */
   f->rbt.key.addr = aligned_addr;
   f->rbt.key.size = size;
   f->is_free = false;

   /* create free fragment after the aligned allocation if there's waste */
   if (waste_after > 0) {
      if (vhsakmt_add_free_mem_frag(mgr, waste_after, aligned_addr + size))
         goto failed;
   }

   mgr->mem_used_size += size;

   mtx_unlock(&mgr->frag_tree_lock);

   hsakmt_vamgr_dump_va(mgr);

   return aligned_addr;

failed:
   mtx_unlock(&mgr->frag_tree_lock);
   return 0;
}

int hsakmt_free_from_vamgr(hsakmt_vamgr_t *mgr, uint64_t addr)
{
   if (!addr)
      return 0;

   rbtree_node_t *n = NULL;
   rbtree_node_t *l_node = NULL;
   rbtree_node_t *r_node = NULL;
   vhsakmt_mem_frag_t *f = NULL;
   vhsakmt_mem_frag_t *i_frag = NULL;
   vhsakmt_mem_frag_t *l_frag = NULL;
   vhsakmt_mem_frag_t *r_frag = NULL;
   rbtree_key_t key;
   uint64_t base = 0;
   uint64_t free_size = 0;

   mtx_lock(&mgr->frag_tree_lock);

   key = rbtree_key(addr, 0);
   n = rbtree_lookup_nearest(&mgr->frag_tree, &key, LKP_ADDR, RIGHT);

   if (!n || n->key.addr != addr)
      goto fail;

   f = vhsakmt_frag_rbt_to_mem_frag(n);

   if (f->is_free) {
      mtx_unlock(&mgr->frag_tree_lock);
      return 0;
   }

   base = addr;
   free_size = f->rbt.key.size;

   /* merge with left adjacent free fragment if exists */
   l_node = rbtree_prev(&mgr->frag_tree, &f->rbt);
   if (l_node) {
      l_frag = vhsakmt_frag_rbt_to_mem_frag(l_node);
      if (l_frag->is_free) {
         vhsakmt_remove_free_mem_frag(mgr, l_frag);
         base -= l_frag->rbt.key.size;
         l_frag->rbt.key.size += f->rbt.key.size;
         rbtree_delete(&mgr->frag_tree, &f->rbt);
         free(f);

         f = l_frag;
         f->free_frag_rbt.key.addr = f->rbt.key.size;
      }
   }

   /* merge with right adjacent free fragment if exists */
   r_node = rbtree_next(&mgr->frag_tree, &f->rbt);
   if (r_node) {
      r_frag = vhsakmt_frag_rbt_to_mem_frag(r_node);
      if (r_frag->is_free) {
         vhsakmt_remove_free_mem_frag(mgr, r_frag);
         f->rbt.key.size += r_frag->rbt.key.size;
         rbtree_delete(&mgr->frag_tree, &r_frag->rbt);
         free(r_frag);
      }
   }

   i_frag = vhsakmt_insert_free_mem_frag(mgr, f->rbt.key.size, base, f);
   if (!i_frag)
      goto fail;

   f->is_free = true;

   mgr->mem_used_size -= free_size;

   mtx_unlock(&mgr->frag_tree_lock);

   hsakmt_vamgr_dump_va(mgr);

   return 0;

fail:
   mtx_unlock(&mgr->frag_tree_lock);
   return -EINVAL;
}

/* need special feature in libhsakmt */
#ifdef HSAKMT_VIRTIO
static int vhsakmt_reserve_va(uint64_t start, uint64_t size)
{
   const int32_t protFlags = PROT_NONE;
   const int32_t mapFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

   void *va = mmap((void *)start, size, protFlags, mapFlags, -1, 0);
   if (va == MAP_FAILED) {
      fprintf(stderr, "hsakmt: failed to reserve va start 0x%lx, size 0x%lx\n",
              start, size);
      return -ENOMEM;
   }

   if (va != (void *)start) {
      fprintf(stderr, "hsakmt: reserve va mismatch, start 0x%lx, got %p\n",
              start, va);
      munmap(va, size);
      return -ENOMEM;
   }

   if (madvise(va, size, MADV_DONTFORK))
      fprintf(stderr, "hsakmt: warning: madvise MADV_DONTFORK failed for va %p\n", va);

   return 0;
}

static int vhsakmt_dereserve_va(uint64_t start, uint64_t size)
{
   if (munmap((void *)start, size)) {
      fprintf(stderr, "hsakmt: failed to dereserve va start 0x%lx, size 0x%lx\n",
              start, size);
      return -EFAULT;
   }

   return 0;
}
#endif

int vhsakmt_init_vamgr(hsakmt_vamgr_t *mgr, uint64_t start, uint64_t size)
{
   if (mgr->vm_va_base_addr)
      return 0;

   rbtree_init(&mgr->frag_tree);
   rbtree_init(&mgr->free_frag_tree);

   mtx_init(&mgr->frag_tree_lock, mtx_plain);

   mgr->reserve_size = size;
   mgr->vm_va_base_addr = start;
   mgr->vm_va_high_addr = start + mgr->reserve_size;
   mgr->mem_used_size = 0;
   mgr->dump_va = false;

   vhsakmt_add_free_mem_frag(mgr, mgr->reserve_size, mgr->vm_va_base_addr);

   return 0;
}

int vhsakmt_destroy_vamgr(hsakmt_vamgr_t *mgr)
{
   /* all memory fragments should be released before here in theory */
   mtx_destroy(&mgr->frag_tree_lock);

   return 0;
}

void hsakmt_set_dump_va(hsakmt_vamgr_t *mgr, int dump_va)
{
   mgr->dump_va = (dump_va != 0);
}
