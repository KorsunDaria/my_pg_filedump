#ifndef PG_VM_H
#define PG_VM_H

#include "postgres.h"

typedef struct 
{
  int show_headers;
  int stats;

  int has_range;
  long range_lo, range_hi;

  int has_heap_page;
  long heap_page_query;
  int expand;

  int only_not_visible;
  int only_not_frozen;
  int only_changed;
} VmOptions;

int vm_main(int argc, char **argv);

#endif