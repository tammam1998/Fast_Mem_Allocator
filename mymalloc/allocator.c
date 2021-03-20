/*
 * Copyright (c) 2015 MIT License by 6.172 Staff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "./allocator_interface.h"
#include "./memlib.h"

// Don't call libc malloc!
#define malloc(...) (USE_MY_MALLOC)
#define free(...) (USE_MY_FREE)
#define realloc(...) (USE_MY_REALLOC)

// All blocks must have a specified minimum alignment.
// The alignment requirement (from config.h) is >= 8 bytes.
#ifndef ALIGNMENT
#define ALIGNMENT 8
#endif


//Bin Lists
struct free_list_t {
  struct free_list_t *prev;
  struct free_list_t *next;
};
typedef struct free_list_t free_list_t;


//header
struct header_t {
  uint32_t size;
  uint32_t prev_size;
};
typedef struct header_t header_t;

// The smallest aligned size that will hold a size_t value.
#define SIZE_T_SIZE align(sizeof(size_t))

//Bins where max and min size are powers of two
#define MIN_SIZE 5

#define SIZE_LIMIT 32

#define SMALLEST_BLOCK_SIZE 24

#define BIN_SIZE SIZE_LIMIT - MIN_SIZE

#define BIN_OFFSET SIZE_LIMIT - MIN_SIZE - 1


free_list_t *bin[BIN_SIZE];

__attribute__((always_inline))
static int align(int size) {
  return ((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

__attribute__((always_inline))
static int max(int a, int b) {
   return (a > b)? a : b;
}

__attribute__((always_inline))
static int min(int a, int b) {
   return (a > b)? b : a;
}

//Gets teh size of the block pointed to by ptr
__attribute__((always_inline))
static uint32_t get_size(void *ptr) {
  return ((header_t *) ((uint64_t) ptr - SIZE_T_SIZE))->size;
}

//Gets the size of the block before the one pointed to by ptr
__attribute__((always_inline))
static uint32_t get_prev_size(void *ptr) {
  return ((header_t *) ((uint64_t)ptr - SIZE_T_SIZE))->prev_size & ~1ull;
}


//Sets teh size of the block at ptr to new_size
__attribute__((always_inline))
static void set_size(void *ptr, int new_size) {
  ((header_t *) ((uint64_t)ptr - SIZE_T_SIZE))->size = new_size;
}

//Sets the block at ptr of size size free
static void mark_free(void *ptr, int size) {
   ((header_t *)((uint64_t)ptr + size))->prev_size = size + 1;
}

//Sets the block at ptr of size size to not free
static void mark_not_free(void *ptr, int size) {
   ((header_t *)((uint64_t)ptr + size))->prev_size = size;
}

//Gets the index in the bin of the free list that would contain a block
//of size size
static int get_bin(int size) {
   return min(SIZE_LIMIT - 1, max(0, BIN_OFFSET - __builtin_clz((uint32_t) size)));
}


/*
Given a pointer to a block, checks if the block in memory right before 
the block is free.

Requires that a previous block exists

Effect:
Returns 1 if the previous block is free
Returns 0 if the previous block is not free
*/
__attribute__((always_inline))
static uint8_t is_free_back(void *ptr) {
  header_t *header_ptr = (header_t *) ((uint64_t) ptr - SIZE_T_SIZE);
  return header_ptr->prev_size & 1;
}


/*
Given a pointer to a block, checks if the block in memory
right after the block is free.

Requires that a next block exists

Effect:
Returns 1 if the next block is free
Returns 0 if no next block exists or the next block is not free
*/
__attribute__((always_inline))
static uint8_t is_free_forward(void *ptr) {
  int curr_size = get_size(ptr) + SIZE_T_SIZE;
  int next_size = get_size(ptr + curr_size);
  header_t *next_header = (header_t *) ((uint64_t) ptr + curr_size + next_size);
  return next_header->prev_size & 1;
}



//-----Testing functions: run with -c to check after every heap operation------------

/*
returns 1 if block pointed to by ptr is free, or 0 otherwsie
*/
__attribute__((always_inline))
static uint8_t is_free(void *ptr) {
   return ((header_t *)((uint64_t) ptr + get_size(ptr)))->prev_size & 1;
}

/*
returns 1 if all blocks that should be coalesced are coalesced or 0 otherwise
The invariant it checks for is that there are no two conscutive free blocks
*/
static uint8_t check_coalesce(void) {
  free_list_t *free_list;
  int size;
  for (int i = 0; i < BIN_SIZE; ++i) {
    for (free_list = bin[i]; free_list != NULL; free_list = free_list->next) {
      size = get_size(free_list);
      if ((uint64_t) my_heap_hi() > ((uint64_t) free_list + size + SIZE_T_SIZE) && is_free_forward(free_list)) {
        return 0;
      }
      if ((uint64_t) my_heap_lo() < ((uint64_t) free_list - SIZE_T_SIZE) && is_free_back(free_list)) {
        return 0;
      }
    }
  }
  return 1;
}

/*
returns 1 if  all blocks on the free list are marked free or 0 otherwise
*/
static uint8_t check_all_free(void) {
  free_list_t *free_list;
  for (int i = 0; i < BIN_SIZE; ++i) {
    for (free_list = bin[i]; free_list != NULL; free_list = free_list->next) {
      if (is_free(free_list) == 0)
         return 0;
    }
  }
  return 1;
}

// check - This checks our invariant that the size_t header before every
// block points to either the beginning of the next block, or the end of the
// heap.
int my_check(void) {
  char* p;
  char* lo = (char*)mem_heap_lo();
  char* hi = (char*)mem_heap_hi() + 1;
  size_t size = 0;

  p = lo;
  while (lo <= p && p + SIZE_T_SIZE < hi) {
    size = align(((header_t *) p)->size + SIZE_T_SIZE);
    p += size;
  }

  if (p + SIZE_T_SIZE != hi) {
    printf("Bad headers did not end at heap_hi!\n");
    printf("heap_lo: %lu, heap_hi: %lu, size: %lu, p: %lu\n", (uint64_t) lo, (uint64_t) hi, size, (uint64_t) p);
    return -1;
  }
   
  if (check_all_free() == 0) {
    printf("some blocks on the free list are not marked free\n");
    return -1;
  }

  if (check_coalesce() == 0) {
    printf("some blocks on the free list should be coalesced, but are not\n");
    return -1;
  }  

  return 0;
}
//---------Testing functions end -----------------


//---------Free list manipulation functions ---------------

/*
Given a free_list_t pointer and its index in the bin, 
deletes the free_list

Args:
   free_list: a pointer to a free_list_t
   bin_index: the index in bin of the linked list that contains free_list

Effect: free_list gets deleted from the free_list

Requires free_list be contained in the linked list bin[bin_index]
*/
__attribute__((always_inline))
static void delete_node(free_list_t *free_list, int bin_index) {
  free_list_t *next = free_list->next;
  free_list_t *prev = free_list->prev;

  if (next != NULL) {
    next->prev = prev;
  }
  if (prev != NULL) {
    prev->next = next;
  }
  else {
    bin[bin_index] = next;
  }
}


/*
Given a ptr to a block of memory, this function coalesces the block with either
or both of the back and next memory blocks if they are free. It is required 
the block at ptr is not in the bin. This function removes the previous or next blocks or both if 
they get coalesced with ptr.
*/
__attribute__((always_inline))
static void *coalesce(void *ptr) {
  int size = get_size(ptr);
  //check the next block
  if (my_heap_hi() > (ptr + size + SIZE_T_SIZE) && is_free_forward(ptr)) {
      int next_offset = size + SIZE_T_SIZE;
      free_list_t *next_list = (free_list_t *) ((uint64_t) ptr + next_offset);
      int next_size = get_size((void *)next_list);
      size = next_offset + next_size;
      delete_node(next_list, get_bin(next_size + SIZE_T_SIZE));
      set_size(ptr, size);
  }

  //check the back block
  if (my_heap_lo() < ptr - SIZE_T_SIZE && is_free_back(ptr)) {
    int prev_size = get_prev_size(ptr) + SIZE_T_SIZE;
    size += prev_size;
    ptr = (char *) ptr - prev_size;
    delete_node((free_list_t *) ptr, get_bin(prev_size));
    set_size(ptr, size);
  }

  //mark the coalesced block free
  mark_free(ptr, size);
  return (void *) ptr;
}


/*
Given the required size of a new memory block, a free_list_t and the size of 
the free_list_t plus SIZE_T_SIZE, this function splits the free_list_t into two blocks of size 
aligned_size and free_list_size - aligned_size. It enters in the bin the latter block.

Args:
  aligned_size: an integer size, required >= SMALLEST_SIZE_BLOCK
  free_list: a pointer to a free_list_t object. Required >= aligned_size + SMALLEST_SIZE_BLOCK.
             It is required that free_list is not in the bin
  free_list_size: the size of free_list plus SIZE_T_SIZE

Effect:
  splits the memory associated with free_list (free_list and its header), into two blocks: one of size 
  aligned_size and one of size size_t_size - aligned_size. It sets the right size for the latter block 
  and enters it into the bin. It sets the right size for the first block. 
*/
__attribute__((always_inline))
static void split_free_list(int aligned_size, free_list_t *free_list, int free_list_size) {
  int free_list_remain = free_list_size - aligned_size;
  int remain_bin_index = get_bin(free_list_remain);
  free_list_t *remain_list = (free_list_t *)((char *)free_list + aligned_size);
  free_list_t *bin_head = bin[remain_bin_index];

  int remain_list_size = free_list_remain - SIZE_T_SIZE;
  set_size(remain_list, remain_list_size);
  mark_free(remain_list, remain_list_size);

  int block_size = aligned_size - SIZE_T_SIZE;
  set_size(free_list, block_size);
  mark_free(free_list, block_size);

  if (bin_head != NULL){
    bin_head->prev = remain_list;
  }
  remain_list->prev = NULL;
  remain_list->next = bin_head;
  bin[remain_bin_index] = remain_list;
}

//----------End of free list manipulation functions 


// init - Initialize the malloc package.  Called once before any other
// calls are made.  Since this is a very simple implementation, we just
// return success.
int my_init(void) { 
  int hi = (uint64_t) my_heap_hi() + 1;
  int req_size = align(hi) - hi;
  mem_sbrk(req_size);
  header_t *first_header = mem_sbrk(SIZE_T_SIZE);
  first_header->prev_size = 0; //indicate not free
  first_header->size = 0;
  //intialize the bins
  for (int i = 0; i < BIN_SIZE; i++) {
    bin[i] = NULL;
  }

  return 0;
}

/*
walks the bin to find and return a memory of size at least size.
Returns NULL if no such memory block exists in the bin.
*/
__attribute__((always_inline))
static void *malloc_from_free_list(size_t size) {
  uint32_t aligned_size = align(size) + SIZE_T_SIZE;
  int bin_index = get_bin(aligned_size);
  free_list_t *free_list = bin[bin_index];

  //tries to find a match in the bin of the best size. 
  while (free_list != NULL) {
    int free_list_size = get_size(free_list) + SIZE_T_SIZE;
    if (free_list_size >= aligned_size) {
      delete_node(free_list, bin_index);
      if (free_list_size - aligned_size >= SMALLEST_BLOCK_SIZE) {
        split_free_list(aligned_size, free_list, free_list_size);
      } else {
        size = free_list_size - SIZE_T_SIZE ;
      }
      mark_not_free(free_list, size);
      set_size(free_list, size);
      return (void *) free_list;
    }
    free_list = free_list->next;
  }
  

  for (int i = bin_index + 1; i < BIN_SIZE; ++i) {
    free_list_t *free_list = bin[i];
    if (free_list != NULL) {
      delete_node(free_list, i);
      int free_list_size = get_size((void *) free_list) + SIZE_T_SIZE;
      if (free_list_size - aligned_size >= SMALLEST_BLOCK_SIZE) {
        split_free_list(aligned_size, free_list, free_list_size);
      } else {
        size = free_list_size - SIZE_T_SIZE;
      }
      mark_not_free(free_list, size);
      set_size(free_list, size);
      return (void *) free_list;  
    }
  }

  //if it didn't find any freelist
  return NULL;
}


//  malloc - Allocate a block by incrementing the brk pointer.
//  Always allocate a block whose size is a multiple of the alignment.
void *my_malloc(size_t size) {

  // We allocate a little bit of extra memory so that we can store the
  // size of the block we've allocated and whether it is free. Take a look 
  // at realloc to see one example of a place where this can come in handy.
  size = align(size);
  uint32_t aligned_size = size + SIZE_T_SIZE;
  
  if (aligned_size < SMALLEST_BLOCK_SIZE) {
    size = SMALLEST_BLOCK_SIZE - SIZE_T_SIZE;
    aligned_size = SMALLEST_BLOCK_SIZE;
  }

  //first try to allocate from the linked list bin
  void *p = malloc_from_free_list(size);

  if (p != NULL) {
    return p;
  }

  //check if the last block in the heap is empty and increase it by the needed size
  if (is_free_back(my_heap_hi() + 1)) {
    int prev_size = get_prev_size(my_heap_hi() + 1);
    int req_size = size - prev_size;
    p = my_heap_hi() - prev_size - SIZE_T_SIZE  + 1;
    delete_node((free_list_t *) p, get_bin(prev_size + SIZE_T_SIZE));
    mem_sbrk(req_size);
    set_size(p, size);
    mark_not_free(p, size);
    return p;
  }

  p = mem_sbrk(aligned_size);

  if (p == (void *)-1) {
    // Whoops, an error of some sort occurred.  We return NULL to let
    // the client code know that we weren't able to allocate memory.
    return NULL;
  } else {
    // We store the size of the block we've allocated in the first
    // SIZE_T_SIZE bytes and we mark the block not free
    set_size(p, size);
    mark_not_free(p, size);

    // Then, we return a pointer to the rest of the block of memory,
    // which is at least size bytes long.  We have to cast to uint8_t
    // before we try any pointer arithmetic because voids have no size
    // and so the compiler doesn't know how far to move the pointer.
    // Since a uint8_t is always one byte, adding SIZE_T_SIZE after
    // casting advances the pointer by SIZE_T_SIZE bytes.
    return (void *) p;
  }
}


// frees the block pointed to by ptr and adds it to the free list bin
void my_free(void *ptr) {
  ptr = coalesce(ptr);
  int size = get_size(ptr) + SIZE_T_SIZE;
  int bin_index = get_bin(size);

  free_list_t *node = ptr;
  node->next = NULL;
  node->prev = NULL;

  free_list_t *head = bin[bin_index];
  if (head == NULL) {
    bin[bin_index] = node;
  } else {
    head->prev = node;
    node->next = head;
    bin[bin_index] = node;
  }
}

// realloc - reallocates a space of size size, and copies the minimum of get_size(ptr) and size amounts 
//of memory from ptr to the new space
void *my_realloc(void *ptr, size_t size) {
  void *newptr;
  uint32_t copy_size;
  size = align(size);
  int aligned_size = size + SIZE_T_SIZE;

  int curr_size = get_size(ptr);
  int curr_aligned_size = curr_size + SIZE_T_SIZE;

  if (size == 0 || ptr == NULL) {
     if (ptr != NULL)
        my_free(ptr);
     return NULL;
  }


  if (curr_size >= size) {
    int remain_size = curr_size - size;
    if (remain_size >= SMALLEST_BLOCK_SIZE) {
      split_free_list(aligned_size, (free_list_t *) ptr, curr_size + SIZE_T_SIZE);
      // ptr = coalesce(ptr);
      // mark_not_free(ptr, size);
      // set_size(ptr, size);
    }
    return ptr;
  }


  // Get the size of the old block of memory.  Take a peek at my_malloc(),
  // where we stashed this in the SIZE_T_SIZE bytes directly before the
  // address we returned.  Now we can back up by that many bytes and read
  // the size.
  copy_size = get_size(ptr);


  if (my_heap_hi() > (ptr + curr_size + SIZE_T_SIZE) && is_free_forward(ptr)) {
    int next_total_size = get_size((void *) ((uint64_t) ptr + curr_aligned_size)) + SIZE_T_SIZE;
    int remain_size = next_total_size + curr_size - size;
    if (remain_size >= 0) {
      delete_node((free_list_t *) ((uint64_t) ptr + curr_aligned_size), get_bin(next_total_size));
      if (remain_size >= SMALLEST_BLOCK_SIZE) {
        split_free_list(aligned_size, (free_list_t *) ptr, curr_aligned_size + next_total_size);
      } else {
          size = curr_size + next_total_size;
      }
      mark_not_free(ptr, size);
      set_size(ptr, size);
      return ptr;
    }
  }

  
  if (ptr + curr_size + SIZE_T_SIZE - 1 == my_heap_hi()) {
     mem_sbrk(size - curr_size);
     set_size(ptr, size);
     mark_not_free(ptr, size);
     return ptr;
  }

  // Allocate a new chunk of memory, and fail if that allocation fails.
  newptr = my_malloc(size);
  if (NULL == newptr) {
    return NULL;
  }

  // This is a standard library call that performs a simple memory copy.
  memcpy(newptr, ptr, copy_size);

  // Release the old block.
  my_free(ptr);

  // Return a pointer to the new block.
  return newptr;
}

