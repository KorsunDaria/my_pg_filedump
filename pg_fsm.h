#ifndef PG_FSM_H
#define PG_FSM_H

#include "postgres.h"

typedef struct {
  int show_headers;
  int show_internal;
  int show_slots;
  int stats;
  int has_range;
  long range_lo, range_hi;
  int expand;
  int has_heap_page;
  long heap_page_query;
  int has_min_avail;
  long min_avail;
  int has_max_avail;
  long max_avail;
  int only_changed;
} FsmOptions;

int fsm_main(int argc, char **argv);

#endif