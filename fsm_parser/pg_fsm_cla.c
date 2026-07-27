/*
 *   -H               include page header fields (pd_lsn, pd_lower, ...,
 *                    fp_next_slot) per node
 *   -i               include the raw internal fan-out byte array (dump only)
 *   -s               include the full per-slot leaf table (dump only)
 *   -a               shorthand for -H -i -s
 *   -q               suppress the summary/statistics block at the end
 *   --range A-B      only print FSM pages with id in [A,B] (children
 *                    outside the range are still traversed, just not
 *                    printed - the SUMMARY block always covers the
 *                    whole file)
 *   --page N         shorthand for --range N-N
 *   --heap-page N    dump only: resolve which FSM leaf page/slot covers
 *                    heap page N and print just that answer, skipping
 *                    the full tree
 *   --min-avail N    dump only, together with -s: only print leaf slots
 *   --max-avail N    whose avail_bytes falls in [min-avail, max-avail]
 *   --only-changed   diff only
 */

 //! сокращенный по умолчанию (полный по флагу)
 //! сумма по странице
 //! версии постгрес


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLCKSZ 8192
#define PAGE_HEADER_SIZE 24
#define FP_NEXT_SLOT_SIZE 4

#define NODES_PER_PAGE \
  (BLCKSZ - PAGE_HEADER_SIZE - FP_NEXT_SLOT_SIZE)                     
#define NONLEAF_NODES_PER_PAGE (BLCKSZ / 2 - 1)                       
#define LEAF_NODES_PER_PAGE (NODES_PER_PAGE - NONLEAF_NODES_PER_PAGE) 
                                                                       

#define FSM_CATEGORIES 256
#define FSM_CAT_STEP (BLCKSZ / FSM_CATEGORIES) 

///Users/dariakorsun/pg/src/include/storage/bufpage.h

typedef struct {
  uint64_t pd_lsn;
  uint16_t pd_checksum;
  uint16_t pd_flags;
  uint16_t pd_lower;
  uint16_t pd_upper;
  uint16_t pd_special;
  uint16_t pd_pagesize_version;
  uint32_t pd_prune_xid;
} PageHeader;

static void parse_header(const uint8_t *buf, PageHeader *h) {
  memcpy(&h->pd_lsn, buf, 8);
  memcpy(&h->pd_checksum, buf + 8, 2);
  memcpy(&h->pd_flags, buf + 10, 2);
  memcpy(&h->pd_lower, buf + 12, 2);
  memcpy(&h->pd_upper, buf + 14, 2);
  memcpy(&h->pd_special, buf + 16, 2);
  memcpy(&h->pd_pagesize_version, buf + 18, 2);
  memcpy(&h->pd_prune_xid, buf + 20, 4);
}

static uint32_t parse_fp_next_slot(const uint8_t *buf) {
  uint32_t v;
  memcpy(&v, buf + PAGE_HEADER_SIZE, 4);
  return v;
}

static int page_is_all_zero(const uint8_t *buf) {
  for (int i = 0; i < BLCKSZ; i++)
    if (buf[i] != 0) return 0;
  return 1;
}

static int header_looks_valid(const PageHeader *h) {
  uint16_t pagesize = h->pd_pagesize_version & 0xFF00;
  uint16_t version = h->pd_pagesize_version & 0x00FF;
  if (pagesize != BLCKSZ) return 0;
  if (version == 0 || version > 10) return 0;
  if (h->pd_special > BLCKSZ) return 0;
  if (h->pd_lower > h->pd_upper) return 0;
  if (h->pd_upper > h->pd_special) return 0;
  return 1;
}

static unsigned cat_to_bytes(uint8_t cat) {
  return cat == 0 ? 0 : (unsigned)(cat - 1) * FSM_CAT_STEP;
}

static void classify_page(long p, int *level, long *parent_id,
                           long *logpageno) {
  if (p == 0) {
    *level = 0;
    *parent_id = -1;
    *logpageno = 0;
    return;
  }
  long group_size = LEAF_NODES_PER_PAGE + 1;
  long idx = p - 1;
  long group = idx / group_size;
  long offset = idx % group_size;

  *level = (offset == 0) ? 1 : 2;
  *parent_id = (*level == 1) ? 0 : (1 + group * group_size);
  *logpageno =
      (*level == 1) ? group : (group * LEAF_NODES_PER_PAGE + offset - 1);
}

static void heap_page_to_fsm_slot(long heap_page, long *out_fsm_page,
                                   long *out_slot) {
  long leaf_logpageno = heap_page / LEAF_NODES_PER_PAGE;
  long group_size = LEAF_NODES_PER_PAGE + 1;
  long group = leaf_logpageno / LEAF_NODES_PER_PAGE;
  long offset = (leaf_logpageno % LEAF_NODES_PER_PAGE) + 1;

  *out_fsm_page = group * group_size + offset + 1;
  *out_slot = heap_page % LEAF_NODES_PER_PAGE;
}

typedef struct {
  long page;
  int level; 
  long parent_id;
  long logpageno;
  int valid;  
  int allzero; 
  PageHeader hdr;
  uint32_t fp_next_slot;
  uint8_t nodes[NODES_PER_PAGE];
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
  uint8_t buf[BLCKSZ];
  for (long p = 0; p < total_pages; p++) {
    if (fread(buf, 1, BLCKSZ, f) != BLCKSZ) {
      //?
      fprintf(stderr, "warning: short read at page %ld, treating as zero\n", p);
      memset(buf, 0, BLCKSZ);
    }
    PageInfo *pi = &pages[p];
    pi->page = p;
    classify_page(p, &pi->level, &pi->parent_id, &pi->logpageno);
    pi->allzero = page_is_all_zero(buf);
    parse_header(buf, &pi->hdr);
    pi->fp_next_slot = parse_fp_next_slot(buf);
    pi->valid = !pi->allzero && header_looks_valid(&pi->hdr);
    if (pi->valid)
      memcpy(pi->nodes, buf + PAGE_HEADER_SIZE + FP_NEXT_SLOT_SIZE,
             NODES_PER_PAGE);
    else //?
      memset(pi->nodes, 0, NODES_PER_PAGE);
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

static LongVec *build_children(long total_pages) {
  LongVec *by_parent = calloc(total_pages, sizeof(LongVec));
  for (long p = 0; p < total_pages; p++) {
    int level;
    long parent_id, logpageno;
    classify_page(p, &level, &parent_id, &logpageno);
    if (parent_id >= 0 && parent_id < total_pages)
      lv_push(&by_parent[parent_id], p);
  }
  return by_parent;
}

static void free_children(LongVec *by_parent, long total_pages) {
  for (long p = 0; p < total_pages; p++) free(by_parent[p].items);
  free(by_parent);
}

typedef struct {
  int show_headers;  /* -H */
  int show_internal; /* -i */
  int show_slots;    /* -s */
  int stats;         /* on by default, -q turns off */

  int has_range; /* --range A-B / --page N */
  long range_lo, range_hi;

  int has_heap_page; /* --heap-page N */
  long heap_page_query;

  int has_min_avail; /* --min-avail N  */
  long min_avail;
  int has_max_avail; /* --max-avail N  */
  long max_avail;

  int only_changed; /* --only-changed */
} Options;

static int in_page_range(const Options *opts, long id) {
  if (!opts->has_range) return 1;
  return id >= opts->range_lo && id <= opts->range_hi;
}

static int in_avail_range(const Options *opts, unsigned avail_bytes) {
  if (opts->has_min_avail && avail_bytes < (unsigned)opts->min_avail) return 0;
  if (opts->has_max_avail && avail_bytes > (unsigned)opts->max_avail) return 0;
  return 1;
}

static void print_header_line(FILE *out, const char *indent,
                               const PageInfo *pi) {
  fprintf(out,
          "%s  header: pd_lsn=%llu pd_checksum=%u pd_flags=0x%x "
          "pd_lower=%u pd_upper=%u pd_special=%u "
          "pagesize=%u layout_version=%u pd_prune_xid=%u fp_next_slot=%u\n",
          indent, (unsigned long long)pi->hdr.pd_lsn, pi->hdr.pd_checksum,
          pi->hdr.pd_flags, pi->hdr.pd_lower, pi->hdr.pd_upper,
          pi->hdr.pd_special, (unsigned)(pi->hdr.pd_pagesize_version & 0xFF00),
          (unsigned)(pi->hdr.pd_pagesize_version & 0x00FF),
          pi->hdr.pd_prune_xid, pi->fp_next_slot);
}

static const char *depth_label_indent(int level) {
  switch (level) {
    case 0:
      return "    ";
    case 1:
      return "        ";
    default:
      return "            ";
  }
}

static void print_internal_raw(FILE *out, const char *indent,
                                const PageInfo *pi) {
  uint8_t max = 0;
  for (int i = 0; i < NONLEAF_NODES_PER_PAGE; i++)
    if (pi->nodes[i] > max) max = pi->nodes[i];
  fprintf(out, "%s  internal fan-out (%d bytes), max_category=%u (%u bytes):\n",
          indent, NONLEAF_NODES_PER_PAGE, max, cat_to_bytes(max));

  fprintf(out, "%s\\-- NONLEAF\n", depth_label_indent(pi->level));

  for (int i = 0; i < NODES_PER_PAGE; i++) {
    if (i % 32 == 0) fprintf(out, "  %s   [%4d] ", indent, i);
    fprintf(out, "  %3u ", pi->nodes[i]);
    if (i % 32 == 31) fprintf(out, "\n");

    if (i == NONLEAF_NODES_PER_PAGE)
      fprintf(out, "%s\\-- LEAF\n", depth_label_indent(pi->level));
  }

  fprintf(out, "\n");
}

static void print_leaf_slots(FILE *out, const char *indent,
                              const PageInfo *pi, const Options *opts) {
  fprintf(out, "%s\\--LEAF SLOTS\n", indent);
  fprintf(out, "%s   leaf slots (%d, one per heap page starting at %ld)",
          indent, LEAF_NODES_PER_PAGE, pi->logpageno * LEAF_NODES_PER_PAGE);
  if (opts->has_min_avail || opts->has_max_avail)
    fprintf(out, " [filtered by avail_bytes]");
  fprintf(out, ":\n");
  for (int s = 0; s < LEAF_NODES_PER_PAGE; s++) {
    uint8_t cat = pi->nodes[NONLEAF_NODES_PER_PAGE + s];
    unsigned avail = cat_to_bytes(cat);
    if (!in_avail_range(opts, avail)) continue;
    fprintf(out,
            "%s     slot %4d (heap page %6ld): category=%3u avail_bytes=%5u\n",
            indent, s, pi->logpageno * LEAF_NODES_PER_PAGE + s, cat, avail);
  }
}

typedef struct {
  long n_root, n_internal, n_leaf, n_zero, n_invalid;
  uint8_t global_max_cat;
  long global_max_cat_page, global_max_cat_slot;
  double avail_sum;
  long avail_count;
} DumpStats;

static void dump_node(FILE *out, PageInfo *pages, LongVec *children,
                       long total_pages, long id, const char *prefix,
                       int is_last, const Options *opts, DumpStats *st) {
  PageInfo *pi = &pages[id];
  const char *branch = "\\-- ";
  const char *label = pi->level == 0   ? "ROOT"
                       : pi->level == 1 ? "INTERNAL"
                                        : "LEAF";
 
  int show = in_page_range(opts, id);

  char child_indent[512];
  snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

  if (pi->level == 0)
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
      fprintf(out,
              "%s%s%ld fsm [%s] -- INVALID HEADER (unreliable data, shown raw)\n",
              prefix, branch, id, label);
      if (opts->show_headers) print_header_line(out, prefix, pi);
    }
  } else {
    if (show) {
      fprintf(out, "%s%s%ld fsm [%s]\n", prefix, branch, id, label);
      if (opts->show_headers) print_header_line(out, child_indent, pi);
      if (opts->show_internal) print_internal_raw(out, child_indent, pi);
    }
    if (pi->level == 2) {
      uint8_t page_max = 0;
      long page_max_slot = -1;
      for (int s = 0; s < LEAF_NODES_PER_PAGE; s++) {
        uint8_t cat = pi->nodes[NONLEAF_NODES_PER_PAGE + s];
        if (cat > page_max) {
          page_max = cat;
          page_max_slot = s;
        }
        st->avail_sum += cat_to_bytes(cat);
        st->avail_count++;
      }
      if (page_max > st->global_max_cat) {
        st->global_max_cat = page_max;
        st->global_max_cat_page = id;
        st->global_max_cat_slot = page_max_slot;
      }
      if (show) {
        fprintf(out, "%s  page max: category=%u (%u bytes) at slot %ld\n",
                child_indent, page_max, cat_to_bytes(page_max), page_max_slot);
        if (opts->show_slots) print_leaf_slots(out, child_indent, pi, opts);
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
                                     long total_pages, const Options *opts) {
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

  uint8_t cat = pi->nodes[NONLEAF_NODES_PER_PAGE + slot];
  fprintf(out, ": category=%u avail_bytes=%u\n", cat, cat_to_bytes(cat));
  if (opts->show_headers) print_header_line(out, "", pi);
}

static int do_dump(const char *in_path, const char *out_path,
                    const Options *opts) {
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

  LongVec *children = build_children(total_pages);

  fprintf(out, "=== pg_fsm dump: %s ===\n", in_path);
  fprintf(out, "file size: %ld bytes, total pages: %ld (%d bytes/page)\n",
          total_pages * BLCKSZ, total_pages, BLCKSZ);
  fprintf(out, "flags: headers=%s internal=%s slots=%s\n",
          opts->show_headers ? "on" : "off", opts->show_internal ? "on" : "off",
          opts->show_slots ? "on" : "off");
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
    if (st.avail_count > 0)
      fprintf(out, "average avail_bytes over all leaf slots: %.1f\n",
              st.avail_sum / st.avail_count);
    fprintf(out, "total leaf slots (== heap pages covered): %ld\n",
            st.avail_count);
  }

  fclose(out);
  free_children(children, total_pages);
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
  if (memcmp(a->nodes, b->nodes, NODES_PER_PAGE) != 0) return ST_CHANGED;
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
} DiffStats;

typedef struct {
  long heap_page;
  long delta_bytes; 
} HeapDelta;

typedef struct {
  HeapDelta *items;
  long count, cap;
} HeapDeltaVec;

static void hd_push(HeapDeltaVec *v, long heap_page, long delta_bytes) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(HeapDelta));
  }
  v->items[v->count].heap_page = heap_page;
  v->items[v->count].delta_bytes = delta_bytes;
  v->count++;
}

static int cmp_heapdelta_by_abs_desc(const void *pa, const void *pb) {
  const HeapDelta *a = pa, *b = pb;
  long ma = a->delta_bytes < 0 ? -a->delta_bytes : a->delta_bytes;
  long mb = b->delta_bytes < 0 ? -b->delta_bytes : b->delta_bytes;
  if (ma > mb) return -1;
  if (ma < mb) return 1;
  return 0;
}

static void diff_node(FILE *out, PageInfo *A, long totalA, PageInfo *B,
                       long totalB, LongVec *children, long id,
                       const char *prefix, int is_last, const Options *opts,
                       DiffStats *st, HeapDeltaVec *deltas) {
  PageInfo *a = id < totalA ? &A[id] : NULL;
  PageInfo *b = id < totalB ? &B[id] : NULL;
  Status status = page_status(a, b);
  const char *branch = "\\-- ";
  PageInfo *ref = (b && !b->allzero && b->valid) ? b : a; /* for level/label */
  const char *label = ref ? (ref->level == 0   ? "ROOT"
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
    int hdr_changed = memcmp(&a->hdr, &b->hdr, sizeof(PageHeader)) != 0 ||
                       a->fp_next_slot != b->fp_next_slot;

    int structural = a->hdr.pd_lower != b->hdr.pd_lower ||
                      a->hdr.pd_upper != b->hdr.pd_upper ||
                      a->hdr.pd_special != b->hdr.pd_special ||
                      a->hdr.pd_flags != b->hdr.pd_flags ||
                      a->hdr.pd_pagesize_version != b->hdr.pd_pagesize_version ||
                      a->fp_next_slot != b->fp_next_slot;
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
        fprintf(out, "%s  header changed (lsn/checksum/prune_xid only)\n",
                child_indent);
      }
    }
    for (int i = 0; i < NODES_PER_PAGE; i++) {
      if (a->nodes[i] == b->nodes[i]) continue;
      unsigned ob = cat_to_bytes(a->nodes[i]), nb = cat_to_bytes(b->nodes[i]);
      const char *arrow = nb > ob ? "UP  ->" : "DOWN->";
      if (i < NONLEAF_NODES_PER_PAGE) {
        if (show)
          fprintf(out, "%s  internal-slot %4d: %3u -> %3u (%5u -> %5u B) %s\n",
                  child_indent, i, a->nodes[i], b->nodes[i], ob, nb, arrow);
      } else {
        int slot = i - NONLEAF_NODES_PER_PAGE;
        long heap_page = ref->logpageno * LEAF_NODES_PER_PAGE + slot;
        if (show)
          fprintf(out,
                  "%s  leaf-slot %4d (heap page %6ld): %3u -> %3u (%5u -> %5u B) "
                  "%s\n",
                  child_indent, slot, heap_page, a->nodes[i], b->nodes[i], ob,
                  nb, arrow);
        hd_push(deltas, heap_page, (long)nb - (long)ob);
      }
      st->slots_changed++;
      if (nb > ob)
        st->slots_up++;
      else
        st->slots_down++;
    }
  }

  long total_max = totalA > totalB ? totalA : totalB;
  if (id >= total_max) return;
  LongVec *kids = &children[id];
  for (long i = 0; i < kids->count; i++)
    diff_node(out, A, totalA, B, totalB, children, kids->items[i], child_indent,
              i == kids->count - 1, opts, st, deltas);
}

static int do_diff(const char *old_path, const char *new_path,
                    const char *out_path, const Options *opts) {
  long totalA, totalB;
  PageInfo *A = load_fsm(old_path, &totalA);
  if (!A) return 1;
  PageInfo *B = load_fsm(new_path, &totalB);
  if (!B) {
    free(A);
    return 1;
  }

  long total_max = totalA > totalB ? totalA : totalB;
  LongVec *children = build_children(total_max);

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(A);
    free(B);
    free_children(children, total_max);
    return 1;
  }

  fprintf(out,
          "=== pg_fsm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n",
          old_path, totalA, new_path, totalB);
  if (opts->only_changed)
    fprintf(out, "(--only-changed: SAME/empty subtrees are hidden)\n");
  if (opts->has_range)
    fprintf(out, "printing only pages [%ld, %ld]\n", opts->range_lo,
            opts->range_hi);
  fprintf(out, "\n");
  if (totalB > totalA)
    fprintf(out, "file GREW by %ld page(s)\n\n", totalB - totalA);
  else if (totalA > totalB)
    fprintf(out, "file SHRANK by %ld page(s)\n\n", totalA - totalB);

  DiffStats st = {0};
  HeapDeltaVec deltas = {0};
  diff_node(out, A, totalA, B, totalB, children, 0, "", 1, opts, &st, &deltas);

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
            "node slots changed: %ld (gained space: %ld, lost space: %ld)\n",
            st.slots_changed, st.slots_up, st.slots_down);

    if (deltas.count > 0) {
      qsort(deltas.items, deltas.count, sizeof(HeapDelta),
            cmp_heapdelta_by_abs_desc);
      long top_n = deltas.count < 10 ? deltas.count : 10;
      fprintf(out, "\ntop %ld changed heap page(s) by |delta bytes|:\n", top_n);
      for (long i = 0; i < top_n; i++)
        fprintf(out, "  heap page %6ld: %+ld byte(s)\n",
                deltas.items[i].heap_page, deltas.items[i].delta_bytes);
    }
  }

  fclose(out);
  free(deltas.items);
  free_children(children, total_max);
  free(A);
  free(B);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

static void usage(const char *prog) {
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
          "  --heap-page N     dump only: locate heap page N in the FSM "
          "tree\n"
          "  --min-avail N     dump -s only: filter leaf slots by "
          "avail_bytes\n"
          "  --max-avail N\n"
          "  --only-changed    diff only: hide SAME/empty subtrees\n",
          prog, prog);
}

static void parse_flags(int argc, char **argv, int start, Options *opts,
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

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 1;
  }

  Options opts = {0};
  opts.stats = 1;
  char *pos[8];
  int npos = 0;

  if (strcmp(argv[1], "dump") == 0) {
    parse_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 2) {
      usage(argv[0]);
      return 1;
    }
    return do_dump(pos[0], pos[1], &opts);
  } else if (strcmp(argv[1], "diff") == 0) {
    parse_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 3) {
      usage(argv[0]);
      return 1;
    }
    return do_diff(pos[0], pos[1], pos[2], &opts);
  }

  usage(argv[0]);
  return 1;
}