/*
 *  Copyright 2005-2006 The Apache Software Foundation or its licensors, as applicable.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "sspace_mark_sweep.h"
#include "sspace_verify.h"
#include "../gen/gen.h"
#include "../thread/collector.h"
#include "../finalizer_weakref/finalizer_weakref.h"


POINTER_SIZE_INT alloc_mask_in_table = ~BLACK_MASK_IN_TABLE;
POINTER_SIZE_INT mark_mask_in_table = BLACK_MASK_IN_TABLE;
POINTER_SIZE_INT cur_alloc_color = OBJ_COLOR_WHITE;
POINTER_SIZE_INT cur_mark_color = OBJ_COLOR_BLACK;

static void ops_color_flip(void)
{
  uint32 temp = cur_alloc_color;
  cur_alloc_color = cur_mark_color;
  cur_mark_color = temp;
  alloc_mask_in_table = ~alloc_mask_in_table;
  mark_mask_in_table = ~mark_mask_in_table;
}

void collector_init_free_chunk_list(Collector *collector)
{
  Free_Chunk_List *list = (Free_Chunk_List*)STD_MALLOC(sizeof(Free_Chunk_List));
  free_chunk_list_init(list);
  collector->free_chunk_list = list;
}

extern Chunk_Heaer_Basic *volatile next_chunk_for_sweep;
static void gc_init_chunk_for_sweep(GC *gc, Sspace *sspace)
{
  next_chunk_for_sweep = (Chunk_Heaer_Basic*)space_heap_start((Space*)sspace);
  next_chunk_for_sweep->adj_prev = NULL;
  
  unsigned int i = gc->num_collectors;
  while(i--){
    Free_Chunk_List *list = gc->collectors[i]->free_chunk_list;
    assert(!list->head);
    assert(!list->tail);
    assert(list->lock == FREE_LOCK);
  }
}


static volatile unsigned int num_marking_collectors = 0;
static volatile unsigned int num_sweeping_collectors = 0;

void mark_sweep_sspace(Collector *collector)
{
  GC *gc = collector->gc;
  Sspace *sspace = (Sspace*)gc_get_pos((GC_Gen*)gc);
  
  unsigned int num_active_collectors = gc->num_active_collectors;
  
  /* Pass 1: **************************************************
     mark all live objects in heap ****************************/
  unsigned int old_num = atomic_cas32(&num_marking_collectors, 0, num_active_collectors+1);
  
  sspace_mark_scan(collector);
  
  old_num = atomic_inc32(&num_marking_collectors);
  if( ++old_num == num_active_collectors ){
    /* last collector's world here */
#ifdef SSPACE_TIME
    sspace_mark_time(FALSE);
#endif
    if(!IGNORE_FINREF )
      collector_identify_finref(collector);
#ifndef BUILD_IN_REFERENT
    else {
      gc_set_weakref_sets(gc);
      gc_update_weakref_ignore_finref(gc);
    }
#endif
    gc_init_chunk_for_sweep(gc, sspace);
    /* let other collectors go */
    num_marking_collectors++;
  }
  while(num_marking_collectors != num_active_collectors + 1);
  
  /* Pass 2: **************************************************
     sweep dead objects ***************************************/
  atomic_cas32( &num_sweeping_collectors, 0, num_active_collectors);
  
  sspace_sweep(collector, sspace);
  
  atomic_inc32(&num_sweeping_collectors);
  while(num_sweeping_collectors != num_active_collectors);
  
  if( collector->thread_handle != 0 )
    return;
  
  /* Leftover: ************************************************ */
#ifdef SSPACE_TIME
  sspace_sweep_time(FALSE);
#endif

  gc_collect_free_chunks(gc, sspace);

#ifdef SSPACE_TIME
  sspace_merge_time(FALSE);
#endif
  
  ops_color_flip();
  gc->root_set = NULL;  // FIXME:: should be placed to a more appopriate place
  gc_set_pool_clear(gc->metadata->gc_rootset_pool);

#ifdef SSPACE_VERIFY
  sspace_verify_after_collection(gc);
#endif
}
