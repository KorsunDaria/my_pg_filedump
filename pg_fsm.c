/*
 *
 * Flags (short flags combinable, e.g. -Hs):
 *   -H                headers (incl. fp_next_slot)
 *   -i                internal array (dump only)
 *   -s                leaf slots (dump only)
 *   -a                all of the above
 *   -q                suppress summary
 *   --range A-B       only print pages in [A,B] (children still traversed)
 *   --page N          shorthand for --range N-N
 *   --expand          full per-slot/per-node listing instead of compressed
 *                     ranges - requires --range/--page
 *   --heap-page N     dump only: locate heap page N in the FSM tree
 *   --min-avail N     dump -s only: filter leaf slot RANGES by avail_bytes
 *   --max-avail N
 *   --only-changed    diff only
 *
 */

#include "postgres.h"
#include "pg_fsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access/htup_details.h"
#include "storage/bufpage.h"
#include "storage/fsm_internals.h"

#define FSM_CATEGORIES 256
#define FSM_CAT_STEP (BLCKSZ / FSM_CATEGORIES)

static unsigned cat_to_bytes(uint8 cat) {
  if (cat == 255) return (unsigned)MaxHeapTupleSize;
  return (unsigned)cat * FSM_CAT_STEP;
}

static int fsm_page_is_all_zero(const uint8 *buf) {
  for (int i = 0; i < BLCKSZ; i++)
    if (buf[i] != 0) return 0;
  return 1;
}

static int fsm_header_looks_valid(PageHeader ph) {
  uint16 pagesize = ph->pd_pagesize_version & 0xFF00;
  uint16 version = ph->pd_pagesize_version & 0x00FF;
  if (pagesize != BLCKSZ) return 0;
  if (version == 0 || version > PG_PAGE_LAYOUT_VERSION) return 0;
  if (ph->pd_special > BLCKSZ) return 0;
  if (ph->pd_lower > ph->pd_upper) return 0;
  if (ph->pd_upper > ph->pd_special) return 0;
  return 1;
}

static void classify_fsm_page(long p, int *level, long *parent_id,
                              long *logpageno) {
  if (p == 0) {
    *level = 2;
    *parent_id = -1;
    *logpageno = 0;
    return;
  }
  long group_size = LeafNodesPerPage + 1;
  long idx = p - 1;
  long group = idx / group_size;
  long offset = idx % group_size;

  *level = (offset == 0) ? 1 : 0;
  *parent_id = (*level == 1) ? 0 : (1 + group * group_size);
  *logpageno = (*level == 1) ? group : (group * LeafNodesPerPage + offset - 1);
}

static void heap_page_to_fsm_slot(long heap_page, long *out_fsm_page,
                                  long *out_slot) {
  long leaf_logpageno = heap_page / LeafNodesPerPage;
  long group_size = LeafNodesPerPage + 1;
  long group = leaf_logpageno / LeafNodesPerPage;
  long offset = (leaf_logpageno % LeafNodesPerPage) + 1;

  *out_fsm_page = group * group_size + offset + 1;
  *out_slot = heap_page % LeafNodesPerPage;
}

typedef struct {
  long page;
  int level;
  long parent_id;
  long logpageno;
  int valid;
  int allzero;
  uint16 pd_flags, pd_checksum;
  uint64 lsn;
  LocationIndex pd_lower, pd_upper, pd_special;
  uint16 pd_pagesize_version;
  int fp_next_slot;
  uint8 nodes[NodesPerPage];
} PageInfo;

static PageInfo *load_fsm(const char *path, long *out_total_pages) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long filesize = ftell(f);
  fseek(f, 0, SEEK_SET);
  long total_pages = filesize / BLCKSZ;
  if (total_pages <= 0) {
    fprintf(stderr, "not a valid fsm file (empty or truncated): %s\n", path);
    fclose(f);
    return NULL;
  }

  PageInfo *pages = calloc(total_pages, sizeof(PageInfo));
  uint8 buf[BLCKSZ];
  for (long p = 0; p < total_pages; p++) {
    if (fread(buf, 1, BLCKSZ, f) != (size_t)BLCKSZ) {
      fprintf(stderr, "warning: short read at page %ld, treating as zero\n", p);
      memset(buf, 0, BLCKSZ);
    }
    PageInfo *pi = &pages[p];
    pi->page = p;
    classify_fsm_page(p, &pi->level, &pi->parent_id, &pi->logpageno);
    pi->allzero = fsm_page_is_all_zero(buf);

    PageHeader ph = (PageHeader)buf;
    pi->valid = !pi->allzero && fsm_header_looks_valid(ph);
    pi->pd_flags = ph->pd_flags;
    pi->pd_checksum = ph->pd_checksum;
    pi->lsn = PageGetLSN((Page)buf);
    pi->pd_lower = ph->pd_lower;
    pi->pd_upper = ph->pd_upper;
    pi->pd_special = ph->pd_special;
    pi->pd_pagesize_version = ph->pd_pagesize_version;

    /* FSMPageData starts exactly where the generic page header ends. */
    FSMPage fp = (FSMPage)PageGetContents((Page)buf);
    pi->fp_next_slot = fp->fp_next_slot;
    if (pi->valid)
      memcpy(pi->nodes, fp->fp_nodes, NodesPerPage);
    else
      memset(pi->nodes, 0, NodesPerPage);
  }
  fclose(f);
  *out_total_pages = total_pages;
  return pages;
}

typedef struct {
  long *items;
  long count, cap;
} LongVec;

static void lv_push(LongVec *v, long x) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 4;
    v->items = realloc(v->items, v->cap * sizeof(long));
  }
  v->items[v->count++] = x;
}

static LongVec *build_fsm_children(long total_pages) {
  LongVec *by_parent = calloc(total_pages, sizeof(LongVec));
  for (long p = 0; p < total_pages; p++) {
    int level;
    long parent_id, logpageno;
    classify_fsm_page(p, &level, &parent_id, &logpageno);
    if (parent_id >= 0 && parent_id < total_pages)
      lv_push(&by_parent[parent_id], p);
  }
  return by_parent;
}

static void free_fsm_children(LongVec *by_parent, long total_pages) {
  for (long p = 0; p < total_pages; p++) free(by_parent[p].items);
  free(by_parent);
}

typedef struct {
  long start, end;
  uint8 value;
} ByteRun;

typedef struct {
  ByteRun *items;
  long count, cap;
} ByteRunVec;

static void byterun_push(ByteRunVec *v, long start, long end, uint8 value) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(ByteRun));
  }
  v->items[v->count++] = (ByteRun){start, end, value};
}

static ByteRunVec compute_byte_runs(const uint8 *arr, long n) {
  ByteRunVec runs = {0};
  if (n <= 0) return runs;
  long start = 0;
  uint8 val = arr[0];
  for (long i = 1; i < n; i++) {
    if (arr[i] != val) {
      byterun_push(&runs, start, i - 1, val);
      start = i;
      val = arr[i];
    }
  }
  byterun_push(&runs, start, n - 1, val);
  return runs;
}

typedef struct {
  long start, end;
  uint8 old_value, new_value;
} ByteDiffRun;

typedef struct {
  ByteDiffRun *items;
  long count, cap;
} ByteDiffRunVec;

static void bdrun_push(ByteDiffRunVec *v, long start, long end, uint8 ov,
                       uint8 nv) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(ByteDiffRun));
  }
  v->items[v->count++] = (ByteDiffRun){start, end, ov, nv};
}

static ByteDiffRunVec compute_byte_diff_runs(const uint8 *a, const uint8 *b,
                                             long n) {
  ByteDiffRunVec runs = {0};
  if (n <= 0) return runs;
  long start = 0;
  uint8 ov = a[0], nv = b[0];
  for (long i = 1; i < n; i++) {
    if (a[i] != ov || b[i] != nv) {
      bdrun_push(&runs, start, i - 1, ov, nv);
      start = i;
      ov = a[i];
      nv = b[i];
    }
  }
  bdrun_push(&runs, start, n - 1, ov, nv);
  return runs;
}

typedef struct {
  uint8 max_cat;
  long max_slot;
  double total_avail;
} LeafPageAgg;

static LeafPageAgg compute_leaf_agg(const uint8 *leaf_nodes, long n) {
  LeafPageAgg agg = {0, -1, 0.0};
  for (long i = 0; i < n; i++) {
    uint8 c = leaf_nodes[i];
    if (c > agg.max_cat) {
      agg.max_cat = c;
      agg.max_slot = i;
    }
    agg.total_avail += cat_to_bytes(c);
  }
  return agg;
}


static int in_page_range(const FsmOptions *opts, long id) {
  if (!opts->has_range) return 1;
  return id >= opts->range_lo && id <= opts->range_hi;
}

static int in_avail_range(const FsmOptions *opts, unsigned avail_bytes) {
  if (opts->has_min_avail && avail_bytes < (unsigned)opts->min_avail) return 0;
  if (opts->has_max_avail && avail_bytes > (unsigned)opts->max_avail) return 0;
  return 1;
}

static void print_header_line(FILE *out, const char *indent,
                              const PageInfo *pi) {
  fprintf(out,
          "%s  header: pd_lsn=%llX pd_checksum=%u pd_flags=0x%x "
          "pd_lower=%u pd_upper=%u pd_special=%u "
          "pagesize=%u layout_version=%u fp_next_slot=%d\n",
          indent, (unsigned long long)pi->lsn, pi->pd_checksum, pi->pd_flags,
          pi->pd_lower, pi->pd_upper, pi->pd_special,
          (unsigned)(pi->pd_pagesize_version & 0xFF00),
          (unsigned)(pi->pd_pagesize_version & 0x00FF), pi->fp_next_slot);
}

static const char *depth_label_indent(int level) {
  switch (level) {
    case 2:
      return "    ";
    case 1:
      return "        ";
    default:
      return "            ";
  }
}

static void print_internal_compressed(FILE *out, const char *indent,
                                      const PageInfo *pi) {
ByteRunVec runs = compute_byte_runs(pi->nodes, NonLeafNodesPerPage);
  
  fprintf(out, "%s  internal fan-out (%d node(s), %ld run(s)):\n", indent,
          (int)NonLeafNodesPerPage, runs.count);

  for (long i = 0; i < runs.count; i++) {
    const ByteRun *r = &runs.items[i];
    char range_str[32];

    /* Формируем диапазон в скобках [A-B] или [A] */
    if (r->start == r->end)
      snprintf(range_str, sizeof(range_str), "[%ld]", r->start);
    else
      snprintf(range_str, sizeof(range_str), "[%ld-%ld]", r->start, r->end);

    /* Выводим с фиксированным отступом колонки в 20 символов */
    fprintf(out, "%s    %-20s %3u\n", indent, range_str, r->value);
  }

  free(runs.items);
}

static void print_internal_expanded(FILE *out, const char *indent,
                                    const PageInfo *pi) {
  fprintf(out, "%s  internal fan-out (%d node(s), expanded):\n", indent,
          (int)NonLeafNodesPerPage);
  for (int i = 0; i < NonLeafNodesPerPage; i++) {
    if (i % 32 == 0) fprintf(out, "%s    [%4d] ", indent, i);
    fprintf(out, "%3u ", pi->nodes[i]);
    if (i % 32 == 31) fprintf(out, "\n");
  }
  if (NonLeafNodesPerPage % 32 != 0) fprintf(out, "\n");
}

static void print_leaf_compressed(FILE *out, const char *indent,
                                  const PageInfo *pi, const FsmOptions *opts) {
  const uint8 *leaf = pi->nodes + NonLeafNodesPerPage;
  ByteRunVec runs = compute_byte_runs(leaf, LeafNodesPerPage);
  fprintf(out,
          "%s  leaf slots (%d, one per heap page starting at %ld, %ld run(s))",
          indent, (int)LeafNodesPerPage, pi->logpageno * LeafNodesPerPage,
          runs.count);
  if (opts->has_min_avail || opts->has_max_avail)
    fprintf(out, " [filtered by avail_bytes]");
  fprintf(out, ":\n");
  for (long i = 0; i < runs.count; i++) {
    const ByteRun *r = &runs.items[i];
    unsigned avail = cat_to_bytes(r->value);
    if (!in_avail_range(opts, avail)) continue;
    long hp_start = pi->logpageno * LeafNodesPerPage + r->start;
    long hp_end = pi->logpageno * LeafNodesPerPage + r->end;
    if (r->start == r->end)
      fprintf(out,
              "%s    heap page %8ld         : category= %-3u avail_bytes=%-5u\n",
              indent, hp_start, r->value, avail);
    else
      fprintf(out,
              "%s    heap page %8ld-%-8ld: category= %-3u avail_bytes=%-5u "
              "(%ld page(s))\n",
              indent, hp_start, hp_end, r->value, avail, hp_end - hp_start + 1);
  }
  free(runs.items);
}

static void print_leaf_expanded(FILE *out, const char *indent,
                                const PageInfo *pi, const FsmOptions *opts) {
  const uint8 *leaf = pi->nodes + NonLeafNodesPerPage;
  fprintf(out,
          "%s  leaf slots (%d, one per heap page starting at %ld, expanded)",
          indent, (int)LeafNodesPerPage, pi->logpageno * LeafNodesPerPage);
  if (opts->has_min_avail || opts->has_max_avail)
    fprintf(out, " [filtered by avail_bytes]");
  fprintf(out, ":\n");
  for (long s = 0; s < LeafNodesPerPage; s++) {
    unsigned avail = cat_to_bytes(leaf[s]);
    if (!in_avail_range(opts, avail)) continue;
    fprintf(out,
            "%s    slot %4ld (heap page %8ld): category=%3u avail_bytes=%5u\n",
            indent, s, pi->logpageno * LeafNodesPerPage + s, leaf[s], avail);
  }
}

typedef struct {
  long n_root, n_internal, n_leaf, n_zero, n_invalid;
  uint8 global_max_cat;
  long global_max_cat_page, global_max_cat_slot;
  double total_avail;
  long total_slots;
} DumpStats;

static void dump_node(FILE *out, PageInfo *pages, LongVec *children,
                      long total_pages, long id, const char *prefix,
                      int is_last, const FsmOptions *opts, DumpStats *st) {
  PageInfo *pi = &pages[id];
  const char *branch = "\\-- ";
  const char *label = pi->level == 2   ? "ROOT"
                      : pi->level == 1 ? "INTERNAL"
                                       : "LEAF";
  int show = in_page_range(opts, id);

  char child_indent[512];
  snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

  if (pi->level == 2)
    st->n_root++;
  else if (pi->level == 1)
    st->n_internal++;
  else
    st->n_leaf++;

  if (pi->allzero) {
    st->n_zero++;
    if (show)
      fprintf(out, "%s%s%ld fsm [%s] -- empty (uninitialized, all-zero page)\n",
              prefix, branch, id, label);
  } else if (!pi->valid) {
    st->n_invalid++;
    if (show) {
      fprintf(
          out,
          "%s%s%ld fsm [%s] -- INVALID HEADER (unreliable data, shown raw)\n",
          prefix, branch, id, label);
      if (opts->show_headers) print_header_line(out, prefix, pi);
    }
  } else {
    if (show) {
      fprintf(out, "%s%s%ld fsm [%s]\n", prefix, branch, id, label);
      if (opts->show_headers) print_header_line(out, child_indent, pi);
      if (opts->show_internal) {
        if (opts->expand)
          print_internal_expanded(out, child_indent, pi);
        else
          print_internal_compressed(out, child_indent, pi);
      }
    }
    if (pi->level == 0) {
      const uint8 *leaf = pi->nodes + NonLeafNodesPerPage;
      LeafPageAgg agg = compute_leaf_agg(leaf, LeafNodesPerPage);
      st->total_avail += agg.total_avail;
      st->total_slots += LeafNodesPerPage;
      if (agg.max_cat > st->global_max_cat) {
        st->global_max_cat = agg.max_cat;
        st->global_max_cat_page = id;
        st->global_max_cat_slot = agg.max_slot;
      }
      if (show) {
        fprintf(out, "%s  page max: category=%u (%u bytes) at slot %ld\n",
                child_indent, agg.max_cat, cat_to_bytes(agg.max_cat),
                agg.max_slot);
        fprintf(out, "%s  Free space on page: %.0f (sum over %d slots)\n",
                child_indent, agg.total_avail, (int)LeafNodesPerPage);
        if (opts->show_slots) {
          if (opts->expand)
            print_leaf_expanded(out, child_indent, pi, opts);
          else
            print_leaf_compressed(out, child_indent, pi, opts);
        }
      }
    }
  }

  if (id >= total_pages) return;
  LongVec *kids = &children[id];
  for (long i = 0; i < kids->count; i++)
    dump_node(out, pages, children, total_pages, kids->items[i], child_indent,
              i == kids->count - 1, opts, st);
}

static void report_heap_page_lookup(FILE *out, PageInfo *pages,
                                    long total_pages, const FsmOptions *opts) {
  long fsm_page, slot;
  heap_page_to_fsm_slot(opts->heap_page_query, &fsm_page, &slot);

  fprintf(out, "heap page %ld -> fsm page %ld, slot %ld", opts->heap_page_query,
          fsm_page, slot);

  if (fsm_page >= total_pages) {
    fprintf(out, " - file only has %ld page(s), not covered yet\n",
            total_pages);
    return;
  }
  PageInfo *pi = &pages[fsm_page];
  if (pi->allzero) {
    fprintf(out, " - page is empty (uninitialized, all-zero)\n");
    return;
  }
  if (!pi->valid) {
    fprintf(out, " - page has an INVALID HEADER, data unreliable\n");
    return;
  }
  uint8 cat = pi->nodes[NonLeafNodesPerPage + slot];
  fprintf(out, ": category=%u avail_bytes=%u\n", cat, cat_to_bytes(cat));
  if (opts->show_headers) print_header_line(out, "", pi);
}

static int do_fsm_dump(const char *in_path, const char *out_path,
                       const FsmOptions *opts) {
  long total_pages;
  PageInfo *pages = load_fsm(in_path, &total_pages);
  if (!pages) return 1;

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(pages);
    return 1;
  }

  if (opts->has_heap_page) {
    fprintf(out, "=== pg_fsm dump: %s (--heap-page %ld lookup) ===\n", in_path,
            opts->heap_page_query);
    report_heap_page_lookup(out, pages, total_pages, opts);
    fclose(out);
    free(pages);
    fprintf(stderr, "wrote %s\n", out_path);
    return 0;
  }

 

  LongVec *children = build_fsm_children(total_pages);

  fprintf(out, "=== pg_fsm dump: %s ===\n", in_path);
  fprintf(out,
          "file size: %ld bytes, total pages: %ld (%d bytes/page, "
          "%d nodes/page = %d internal + %d leaf)\n",
          total_pages * BLCKSZ, total_pages, BLCKSZ, (int)NodesPerPage,
          (int)NonLeafNodesPerPage, (int)LeafNodesPerPage);
  fprintf(out, "flags: headers=%s internal=%s slots=%s expand=%s\n",
          opts->show_headers ? "on" : "off", opts->show_internal ? "on" : "off",
          opts->show_slots ? "on" : "off", opts->expand ? "on" : "off");
  if (opts->has_range)
    fprintf(out,
            "printing only pages [%ld, %ld] (SUMMARY below still covers the "
            "whole file)\n",
            opts->range_lo, opts->range_hi);
  fprintf(out, "\n");

  DumpStats st = {0};
  dump_node(out, pages, children, total_pages, 0, "", 1, opts, &st);

  if (opts->stats) {
    fprintf(
        out,
        "\n--------------------------------------------------------------\n");
    fprintf(out, "SUMMARY\n");
    fprintf(out,
            "--------------------------------------------------------------\n");
    fprintf(out, "root pages: %ld  internal pages: %ld  leaf pages: %ld\n",
            st.n_root, st.n_internal, st.n_leaf);
    fprintf(out, "zero/hole pages: %ld  invalid-header pages: %ld\n", st.n_zero,
            st.n_invalid);
    fprintf(
        out,
        "global max free-space category: %u (%u bytes), page %ld slot %ld\n",
        st.global_max_cat, cat_to_bytes(st.global_max_cat),
        st.global_max_cat_page, st.global_max_cat_slot);
    if (st.total_slots > 0)
      fprintf(out, "average avail_bytes over all leaf slots: %.1f\n",
              st.total_avail / st.total_slots);
    fprintf(out, "total avail_bytes across all leaf pages: %.0f\n",
            st.total_avail);
    fprintf(out, "total leaf slots (== heap pages covered): %ld\n",
            st.total_slots);
  }

  fclose(out);
  free_fsm_children(children, total_pages);
  free(pages);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

typedef enum { ST_SAME, ST_CHANGED, ST_ADDED, ST_REMOVED, ST_EMPTY } Status;

static Status page_status(const PageInfo *a, const PageInfo *b) {
  int aOk = a && !a->allzero && a->valid;
  int bOk = b && !b->allzero && b->valid;
  if (!aOk && bOk) return ST_ADDED;
  if (aOk && !bOk) return ST_REMOVED;
  if (!aOk && !bOk) return ST_EMPTY;
  if (memcmp(a->nodes, b->nodes, NodesPerPage) != 0) return ST_CHANGED;
  return ST_SAME;
}

static const char *status_name(Status s) {
  switch (s) {
    case ST_SAME:
      return "same";
    case ST_CHANGED:
      return "CHANGED";
    case ST_ADDED:
      return "ADDED";
    case ST_REMOVED:
      return "REMOVED";
    default:
      return "empty";
  }
}

typedef struct {
  long pages_changed, headers_changed, headers_structural_changed,
      slots_changed, slots_up, slots_down, pages_added, pages_removed;
  double old_total_avail, new_total_avail;
} DiffStats;

typedef struct {
  long heap_start, heap_end;
  long delta_bytes_per_page;
} LeafDelta;

typedef struct {
  LeafDelta *items;
  long count, cap;
} LeafDeltaVec;

static void ld_push(LeafDeltaVec *v, long start, long end, long delta) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(LeafDelta));
  }
  v->items[v->count++] = (LeafDelta){start, end, delta};
}

static int cmp_leafdelta_by_total_abs_desc(const void *pa, const void *pb) {
  const LeafDelta *a = pa, *b = pb;
  long na = a->heap_end - a->heap_start + 1,
       nb = b->heap_end - b->heap_start + 1;
  long ta = (a->delta_bytes_per_page < 0 ? -a->delta_bytes_per_page
                                         : a->delta_bytes_per_page) *
            na;
  long tb = (b->delta_bytes_per_page < 0 ? -b->delta_bytes_per_page
                                         : b->delta_bytes_per_page) *
            nb;
  if (ta > tb) return -1;
  if (ta < tb) return 1;
  return 0;
}

static void print_internal_diff(FILE *out, int do_print, const char *indent,
                                const PageInfo *a, const PageInfo *b,
                                const FsmOptions *opts, DiffStats *st) {
  ByteDiffRunVec runs =
      compute_byte_diff_runs(a->nodes, b->nodes, NonLeafNodesPerPage);
  for (long i = 0; i < runs.count; i++) {
    const ByteDiffRun *r = &runs.items[i];
    if (r->old_value == r->new_value) continue;
    st->slots_changed += r->end - r->start + 1;
    if (r->new_value > r->old_value)
      st->slots_up += r->end - r->start + 1;
    else
      st->slots_down += r->end - r->start + 1;
    if (!do_print) continue;
    if (r->start == r->end)
      fprintf(out, "%s  internal-node %4ld           : %3u -> %3u\n", indent,
              r->start, r->old_value, r->new_value);
    else
      fprintf(out,
              "%s  internal-nodes %4ld-%-4ld    : %3u -> %3u (%ld node(s))\n",
              indent, r->start, r->end, r->old_value, r->new_value,
              r->end - r->start + 1);
  }
  free(runs.items);
}

static void print_leaf_diff(FILE *out, int do_print, const char *indent,
                            const PageInfo *a, const PageInfo *b,
                            const FsmOptions *opts, DiffStats *st,
                            LeafDeltaVec *deltas) {
  const uint8 *la = a->nodes + NonLeafNodesPerPage;
  const uint8 *lb = b->nodes + NonLeafNodesPerPage;
  ByteDiffRunVec runs = compute_byte_diff_runs(la, lb, LeafNodesPerPage);
  for (long i = 0; i < runs.count; i++) {
    const ByteDiffRun *r = &runs.items[i];
    long n = r->end - r->start + 1;
    long hp_start = a->logpageno * LeafNodesPerPage + r->start;
    long hp_end = a->logpageno * LeafNodesPerPage + r->end;
    if (r->old_value == r->new_value) continue;
    unsigned ob = cat_to_bytes(r->old_value), nb = cat_to_bytes(r->new_value);
    st->slots_changed += n;
    if (nb > ob)
      st->slots_up += n;
    else
      st->slots_down += n;
    ld_push(deltas, hp_start, hp_end, (long)nb - (long)ob);
    if (!do_print) continue;
    if (hp_start == hp_end)
      fprintf(out,
              "%s  leaf-slot heap page %8ld           : %3u -> %3u (%5u -> %5u "
              "B)\n",
              indent, hp_start, r->old_value, r->new_value, ob, nb);
    else
      fprintf(out,
              "%s  leaf-slots heap pages %8ld-%-8ld: %3u -> %3u (%5u -> "
              "%5u B) (%ld page(s))\n",
              indent, hp_start, hp_end, r->old_value, r->new_value, ob, nb, n);
  }
  free(runs.items);
}

static void diff_node(FILE *out, PageInfo *A, long totalA, PageInfo *B,
                      long totalB, LongVec *children, long id,
                      const char *prefix, int is_last, const FsmOptions *opts,
                      DiffStats *st, LeafDeltaVec *deltas) {
  PageInfo *a = id < totalA ? &A[id] : NULL;
  PageInfo *b = id < totalB ? &B[id] : NULL;
  Status status = page_status(a, b);
  const char *branch = "\\-- ";
  PageInfo *ref = (b && !b->allzero && b->valid) ? b : a;
  const char *label = ref ? (ref->level == 2   ? "ROOT"
                             : ref->level == 1 ? "INTERNAL"
                                               : "LEAF")
                          : "?";

  int show = in_page_range(opts, id) &&
             !(opts->only_changed && (status == ST_SAME || status == ST_EMPTY));

  if (show)
    fprintf(out, "%s%s%ld fsm [%s] %s\n", prefix, branch, id, label,
            status_name(status));

  char child_indent[512];
  snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

  if (status == ST_ADDED)
    st->pages_added++;
  else if (status == ST_REMOVED)
    st->pages_removed++;
  else if (status == ST_CHANGED) {
    st->pages_changed++;
    int structural = a->pd_lower != b->pd_lower || a->pd_upper != b->pd_upper ||
                     a->pd_special != b->pd_special ||
                     a->pd_flags != b->pd_flags ||
                     a->pd_pagesize_version != b->pd_pagesize_version ||
                     a->fp_next_slot != b->fp_next_slot;
    int hdr_changed =
        structural || a->lsn != b->lsn || a->pd_checksum != b->pd_checksum;
    if (hdr_changed) {
      st->headers_changed++;
      if (structural) {
        st->headers_structural_changed++;
        if (show)
          fprintf(out,
                  "%s  header changed (structural: lower/upper/special/"
                  "flags/version/fp_next_slot)\n",
                  child_indent);
      } else if (show && opts->show_headers) {
        fprintf(out, "%s  header changed (lsn/checksum only)\n", child_indent);
      }
    }
    if (opts->show_internal)
      print_internal_diff(out, show, child_indent, a, b, opts, st);
    print_leaf_diff(out, show, child_indent, a, b, opts, st, deltas);
  }

  if (id >= (totalA > totalB ? totalA : totalB)) return;
  LongVec *kids = &children[id];
  for (long i = 0; i < kids->count; i++)
    diff_node(out, A, totalA, B, totalB, children, kids->items[i], child_indent,
              i == kids->count - 1, opts, st, deltas);
}

static int do_fsm_diff(const char *old_path, const char *new_path,
                   const char *out_path, const FsmOptions *opts) {
  long totalA, totalB;
  PageInfo *A = load_fsm(old_path, &totalA);
  if (!A) return 1;
  PageInfo *B = load_fsm(new_path, &totalB);
  if (!B) {
    free(A);
    return 1;
  }

  if (opts->expand && !opts->has_range) {
    fprintf(stderr,
            "--expand needs --range A-B / --page N (refusing to expand "
            "every printed page's full node array)\n");
    free(A);
    free(B);
    return 1;
  }

  long total_max = totalA > totalB ? totalA : totalB;
  LongVec *children = build_fsm_children(total_max);

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(A);
    free(B);
    free_fsm_children(children, total_max);
    return 1;
  }

  fprintf(out,
          "=== pg_fsm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n",
          old_path, totalA, new_path, totalB);
  if (opts->only_changed)
    fprintf(out,
            "(--only-changed: SAME/empty subtrees and unchanged runs are "
            "hidden)\n");
  if (opts->has_range)
    fprintf(out, "printing only pages [%ld, %ld]\n", opts->range_lo,
            opts->range_hi);
  fprintf(out, "\n");
  if (totalB > totalA)
    fprintf(out, "file GREW by %ld page(s)\n\n", totalB - totalA);
  else if (totalA > totalB)
    fprintf(out, "file SHRANK by %ld page(s)\n\n", totalA - totalB);

  DiffStats st = {0};
  LeafDeltaVec deltas = {0};
  diff_node(out, A, totalA, B, totalB, children, 0, "", 1, opts, &st, &deltas);

  for (long p = 0; p < totalA; p++)
    if (A[p].level == 0 && (A[p].allzero || A[p].valid)) {
      const uint8 *leaf = A[p].nodes + NonLeafNodesPerPage;
      st.old_total_avail +=
          compute_leaf_agg(leaf, LeafNodesPerPage).total_avail;
    }
  for (long p = 0; p < totalB; p++)
    if (B[p].level == 0 && (B[p].allzero || B[p].valid)) {
      const uint8 *leaf = B[p].nodes + NonLeafNodesPerPage;
      st.new_total_avail +=
          compute_leaf_agg(leaf, LeafNodesPerPage).total_avail;
    }

  if (opts->stats) {
    fprintf(
        out,
        "\n--------------------------------------------------------------\n");
    fprintf(out, "SUMMARY\n");
    fprintf(out,
            "--------------------------------------------------------------\n");
    fprintf(out,
            "pages changed: %ld  added: %ld  removed: %ld  headers changed: "
            "%ld (structural: %ld)\n",
            st.pages_changed, st.pages_added, st.pages_removed,
            st.headers_changed, st.headers_structural_changed);
    fprintf(out,
            "node/slots changed: %ld (gained space: %ld, lost space: %ld)\n",
            st.slots_changed, st.slots_up, st.slots_down);
    fprintf(out, "total avail_bytes: old=%.0f new=%.0f (delta %+.0f)\n",
            st.old_total_avail, st.new_total_avail,
            st.new_total_avail - st.old_total_avail);

    if (deltas.count > 0) {
      qsort(deltas.items, deltas.count, sizeof(LeafDelta),
            cmp_leafdelta_by_total_abs_desc);
      long top_n = deltas.count < 10 ? deltas.count : 10;
      fprintf(out,
              "\ntop %ld changed heap-page range(s) by total |delta bytes|:\n",
              top_n);
      for (long i = 0; i < top_n; i++) {
        const LeafDelta *d = &deltas.items[i];
        long n = d->heap_end - d->heap_start + 1;
        if (d->heap_start == d->heap_end)
          fprintf(out, "  heap page %8ld           : %+ld byte(s)\n",
                  d->heap_start, d->delta_bytes_per_page);
        else
          fprintf(out,
                  "  heap pages %8ld-%-8ld: %+ld byte(s)/page, %+ld total "
                  "(%ld page(s))\n",
                  d->heap_start, d->heap_end, d->delta_bytes_per_page,
                  d->delta_bytes_per_page * n, n);
      }
    }
  }

  fclose(out);
  free(deltas.items);
  free_fsm_children(children, total_max);
  free(A);
  free(B);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

static void fsm_usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s dump [flags] <relfilenode_fsm> <out.txt>\n"
          "  %s diff [flags] <old_fsm> <new_fsm> <out.txt>\n"
          "flags:\n"
          "  -H                headers (incl. fp_next_slot)  -i internal "
          "array (dump only)\n"
          "  -s                leaf slots (dump only)        -a all of "
          "the above\n"
          "  -q                suppress summary\n"
          "  --range A-B       only print pages in [A,B] (children still "
          "traversed)\n"
          "  --page N          shorthand for --range N-N\n"
          "  --expand          full per-slot/per-node listing instead of "
          "compressed ranges (requires --range/--page)\n"
          "  --heap-page N     dump only: locate heap page N in the FSM "
          "tree\n"
          "  --min-avail N     dump -s only: filter leaf slot ranges by "
          "avail_bytes\n"
          "  --max-avail N\n"
          "  --only-changed    diff only: hide SAME/empty subtrees and "
          "unchanged runs\n",
          prog, prog);
}

static void parse_fsm_flags(int argc, char **argv, int start, FsmOptions *opts,
                        char **pos, int *npos) {
  *npos = 0;
  for (int i = start; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "--range") == 0 && i + 1 < argc) {
      long lo, hi;
      if (sscanf(argv[++i], "%ld-%ld", &lo, &hi) == 2) {
        opts->has_range = 1;
        opts->range_lo = lo;
        opts->range_hi = hi;
      } else {
        fprintf(stderr, "bad --range value %s, expected A-B (ignored)\n",
                argv[i]);
      }
    } else if (strcmp(a, "--page") == 0 && i + 1 < argc) {
      opts->has_range = 1;
      opts->range_lo = opts->range_hi = atol(argv[++i]);
    } else if (strcmp(a, "--expand") == 0) {
      opts->expand = 1;
    } else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc) {
      opts->has_heap_page = 1;
      opts->heap_page_query = atol(argv[++i]);
    } else if (strcmp(a, "--min-avail") == 0 && i + 1 < argc) {
      opts->has_min_avail = 1;
      opts->min_avail = atol(argv[++i]);
    } else if (strcmp(a, "--max-avail") == 0 && i + 1 < argc) {
      opts->has_max_avail = 1;
      opts->max_avail = atol(argv[++i]);
    } else if (strcmp(a, "--only-changed") == 0) {
      opts->only_changed = 1;
    } else if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
      for (const char *c = a + 1; *c; c++) {
        switch (*c) {
          case 'H':
            opts->show_headers = 1;
            break;
          case 'i':
            opts->show_internal = 1;
            break;
          case 's':
            opts->show_slots = 1;
            break;
          case 'a':
            opts->show_headers = opts->show_internal = opts->show_slots = 1;
            break;
          case 'q':
            opts->stats = 0;
            break;
          default:
            fprintf(stderr, "unknown flag -%c (ignored)\n", *c);
        }
      }
    } else {
      pos[(*npos)++] = argv[i];
    }
  }
}

int fsm_main(int argc, char **argv) {
  if (argc < 2) {
    fsm_usage(argv[0]);
    return 1;
  }

  FsmOptions opts = {0};
  opts.stats = 1;
  char *pos[8];
  int npos = 0;

  if (strcmp(argv[1], "dump") == 0) {
    parse_fsm_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 2) {
      fsm_usage(argv[0]);
      return 1;
    }
    return do_fsm_dump(pos[0], pos[1], &opts);
  } else if (strcmp(argv[1], "diff") == 0) {
    parse_fsm_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 3) {
      fsm_usage(argv[0]);
      return 1;
    }
    return do_fsm_diff(pos[0], pos[1], pos[2], &opts);
  }

  fsm_usage(argv[0]);
  return 1;
}