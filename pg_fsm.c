/*
 *
 *   -H                headers (incl. fp_next_slot)
 *   -i                internal array (dump only)
 *   -s                leaf slots (dump only)
 *   -a                all of the above
 *   -q                add summary
 *   --range A-B       only print pages in [A,B] (children still traversed)
 *   --page N          shorthand for --range N-N
 *   --extra           full per-slot/per-node listing instead of compressed
 *                     ranges - requires --range/--page
 *   --heap-page N     dump only: locate heap page N in the FSM tree
 *   --min N     dump -s only: filter leaf slot RANGES by avail_bytes
 *   --max N
 *   --only-changed    diff only
 *
 */

//! добавить что корректно не валидно в вывод
//! количество страницы по категориям от 0 до 255

//! (пропускать те кт у кого нет страниц)
//! анализ насколько полетели на реплике

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

typedef enum {
  HEADER_OK,
  HEADER_WRONG_PAGESIZE,
  HEADER_INVALID
} HeaderStatus;

static HeaderStatus fsm_header_status(PageHeader ph, const char **reason) {
  uint16 pagesize = ph->pd_pagesize_version & 0xFF00;
  uint16 version = ph->pd_pagesize_version & 0x00FF;

  if (pagesize != BLCKSZ) {
    *reason =
        "page size in header does not match BLCKSZ this binary was built with";
    return HEADER_WRONG_PAGESIZE;
  }
  if (version == 0 || version > PG_PAGE_LAYOUT_VERSION) {
    *reason = "bad page layout version";
    return HEADER_INVALID;
  }
  if (ph->pd_special > BLCKSZ) {
    *reason = "pd_special is larger than the page";
    return HEADER_INVALID;
  }
  if (ph->pd_lower > ph->pd_upper) {
    *reason = "pd_lower is greater than pd_upper";
    return HEADER_INVALID;
  }
  if (ph->pd_upper > ph->pd_special) {
    *reason = "pd_upper is greater than pd_special";
    return HEADER_INVALID;
  }
  *reason = NULL;
  return HEADER_OK;
}

static void classify_fsm_page(long page_index, int *level, long *parent_id,
                              long *logpageno) {
  if (page_index == 0) {
    *level = 0;
    *parent_id = -1;
    *logpageno = 0;
    return;
  }
  long group_size = LeafNodesPerPage + 1;
  long idx = page_index - 1;
  long group = idx / group_size;
  long offset = idx % group_size;

  *level = (offset == 0) ? 1 : 2;
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

  HeaderStatus header_status;
  const char *invalid_reason;

  PageHeaderData header;

  int fp_next_slot;
  uint8 nodes[NodesPerPage];
} FsmPageInfo;


static void fill_fsm_page_info(const uint8 *buf, long page_index,
                               FsmPageInfo *page_info) {
  page_info->page = page_index;
  classify_fsm_page(page_index, &page_info->level, &page_info->parent_id,
                    &page_info->logpageno);
  page_info->allzero = fsm_page_is_all_zero(buf);

  PageHeader page_header = (PageHeader)buf;
  const char *reason = NULL;
  page_info->header_status = page_info->allzero
                                  ? HEADER_OK
                                  : fsm_header_status(page_header, &reason);
  page_info->invalid_reason = reason;
  page_info->valid =
      !page_info->allzero && page_info->header_status == HEADER_OK;
  page_info->header = *page_header;

  FSMPage fsm_page = (FSMPage)PageGetContents((Page)buf);
  page_info->fp_next_slot = fsm_page->fp_next_slot;
  if (page_info->valid)
    memcpy(page_info->nodes, fsm_page->fp_nodes, NodesPerPage);
  else
    memset(page_info->nodes, 0, NodesPerPage);
}


static FsmPageInfo *load_fsm(const char *path, long *out_total_pages) {
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
    fprintf(stderr, "%s: empty or truncated\n", path);
    fclose(f);
    return NULL;
  }

  FsmPageInfo *pages = calloc(total_pages, sizeof(FsmPageInfo));
  uint8 buf[BLCKSZ];
  for (long page_index = 0; page_index < total_pages; page_index++) {
    if (fread(buf, 1, BLCKSZ, f) != (size_t)BLCKSZ) {
      fprintf(stderr, "%s: short read at page %ld, treating as zero\n", path,
              page_index);
      memset(buf, 0, BLCKSZ);
    }
    fill_fsm_page_info(buf, page_index, &pages[page_index]);
  }
  fclose(f);
  *out_total_pages = total_pages;
  return pages;
}

typedef struct {
  long *items;
  long count, cap;
} LongVec;


static void long_vec_push(LongVec *v, long x) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 4;
    v->items = realloc(v->items, v->cap * sizeof(long));
  }
  v->items[v->count++] = x;
}

static LongVec *build_fsm_children(long total_pages) {
  LongVec *by_parent = calloc(total_pages, sizeof(LongVec));
  for (long page_index = 0; page_index < total_pages; page_index++) {
    int level;
    long parent_id, logpageno;
    classify_fsm_page(page_index, &level, &parent_id, &logpageno);
    if (parent_id >= 0 && parent_id < total_pages)
      long_vec_push(&by_parent[parent_id], page_index);
  }
  return by_parent;
}

static void free_fsm_children(LongVec *by_parent, long total_pages) {
  for (long page_index = 0; page_index < total_pages; page_index++)
    free(by_parent[page_index].items);
  free(by_parent);
}

typedef struct {
  long start, end;
  uint8 value;
} ByteCompressedRange;

typedef struct {
  ByteCompressedRange *items;
  long count, cap;
} ByteCompressedRangeVec;

static void byte_compressed_range_push(ByteCompressedRangeVec *v, long start,
                                      long end, uint8 value) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(ByteCompressedRange));
  }
  v->items[v->count++] = (ByteCompressedRange){start, end, value};
}

static ByteCompressedRangeVec compute_byte_compressed_ranges(const uint8 *arr,
                                                              long n) {
  ByteCompressedRangeVec compressed = {0};
  if (n <= 0) return compressed;
  long start = 0;
  uint8 val = arr[0];
  for (long i = 1; i < n; i++) {
    if (arr[i] != val) {
      byte_compressed_range_push(&compressed, start, i - 1, val);
      start = i;
      val = arr[i];
    }
  }
  byte_compressed_range_push(&compressed, start, n - 1, val);
  return compressed;
}

typedef struct {
  long start, end;
  uint8 old_value, new_value;
} ByteDiffCompressedRange;

typedef struct {
  ByteDiffCompressedRange *items;
  long count, cap;
} ByteDiffCompressedRangeVec;

static void byte_diff_compressed_range_push(ByteDiffCompressedRangeVec *v,
                                           long start, long end, uint8 ov,
                                           uint8 nv) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(ByteDiffCompressedRange));
  }
  v->items[v->count++] = (ByteDiffCompressedRange){start, end, ov, nv};
}

static ByteDiffCompressedRangeVec compute_byte_diff_compressed_ranges(
    const uint8 *a, const uint8 *b, long n) {
  ByteDiffCompressedRangeVec compressed = {0};
  if (n <= 0) return compressed;
  long start = 0;
  uint8 ov = a[0], nv = b[0];
  for (long i = 1; i < n; i++) {
    if (a[i] != ov || b[i] != nv) {
      byte_diff_compressed_range_push(&compressed, start, i - 1, ov, nv);
      start = i;
      ov = a[i];
      nv = b[i];
    }
  }
  byte_diff_compressed_range_push(&compressed, start, n - 1, ov, nv);
  return compressed;
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


static int in_page_range(const FsmOptions *options, long id) {
  if (!options->has_range) return 1;
  return id >= options->range_lo && id <= options->range_hi;
}

static int in_avail_range(const FsmOptions *options, unsigned avail_bytes) {
  if (options->has_min_avail && avail_bytes < (unsigned)options->min_avail)
    return 0;
  if (options->has_max_avail && avail_bytes > (unsigned)options->max_avail)
    return 0;
  return 1;
}

static void print_header_line(FILE *out, const char *indent,
                              const FsmPageInfo *page_info) {
  fprintf(out,
          "%s  header: pd_lsn=%llX pd_checksum=%u pd_flags=0x%x "
          "pd_lower=%u pd_upper=%u pd_special=%u "
          "pagesize=%u layout_version=%u fp_next_slot=%d\n",
          indent, (unsigned long long)((uint64) page_info->header.pd_lsn.xlogid << 32 | page_info->header.pd_lsn.xrecoff),
          page_info->header.pd_checksum, page_info->header.pd_flags,
          page_info->header.pd_lower, page_info->header.pd_upper,
          page_info->header.pd_special,
          (unsigned)(page_info->header.pd_pagesize_version & 0xFF00),
          (unsigned)(page_info->header.pd_pagesize_version & 0x00FF),
          page_info->fp_next_slot);
  if (page_info->header_status == HEADER_INVALID)
    fprintf(out, "%s  header valid: no (%s)\n", indent,
            page_info->invalid_reason);
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
                                      const FsmPageInfo *page_info) {
  struct { const char *section_name; long start, len; } sec[2] = {
    {"NONLEAF", 0, NonLeafNodesPerPage},
    {"LEAF", NonLeafNodesPerPage, LeafNodesPerPage},
  };

  for (int s = 0; s < 2; s++) {
    ByteCompressedRangeVec compressed = compute_byte_compressed_ranges(
        page_info->nodes + sec[s].start, sec[s].len);
    fprintf(out, "%s  %s (%ld nodes):\n", indent, sec[s].section_name,
            sec[s].len);

    for (long i = 0; i < compressed.count; i++) {
      const ByteCompressedRange *r = &compressed.items[i];
      char range_str[32];

      if (r->start == r->end)
        snprintf(range_str, sizeof(range_str), "[%ld]", r->start);
      else
        snprintf(range_str, sizeof(range_str), "[%ld-%ld]", r->start, r->end);

      fprintf(out, "%s      %-20s %3u\n", indent, range_str, r->value);
    }

    free(compressed.items);
  }
}

static void print_internal_expanded(FILE *out, const char *indent,
                                    const FsmPageInfo *page_info) {
  struct { const char *section_name; long start, len; } sec[2] = {
    {"NONLEAF", 0, NonLeafNodesPerPage},
    {"LEAF", NonLeafNodesPerPage, LeafNodesPerPage},
  };

  for (int s = 0; s < 2; s++) {
    fprintf(out, "%s  %s (%ld node(s), expanded):\n", indent,
            sec[s].section_name, sec[s].len);
    long end = sec[s].start + sec[s].len;
    for (long i = sec[s].start; i < end; i++) {
      long local_i = i - sec[s].start;
      if (local_i % 32 == 0) fprintf(out, "%s      [%4ld] ", indent, local_i);
      fprintf(out, "%3u ", page_info->nodes[i]);
      if (local_i % 32 == 31) fprintf(out, "\n");
    }
    if (sec[s].len % 32 != 0) fprintf(out, "\n");
  }
}


static void print_leaf_compressed(FILE *out, const char *indent,
                                  const FsmPageInfo *page_info,
                                  const FsmOptions *options) {
  const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
  ByteCompressedRangeVec compressed =
      compute_byte_compressed_ranges(leaf, LeafNodesPerPage);
  fprintf(out,
          "%s  leaf slots (%d, starting at %ld)",
          indent, (int)LeafNodesPerPage,
          page_info->logpageno * LeafNodesPerPage);
  if (options->has_min_avail || options->has_max_avail)
    fprintf(out, " [filtered by avail_bytes]");
  fprintf(out, ":\n");
  for (long i = 0; i < compressed.count; i++) {
    const ByteCompressedRange *r = &compressed.items[i];
    unsigned avail = cat_to_bytes(r->value);
    if (!in_avail_range(options, avail)) continue;
    long hp_start = page_info->logpageno * LeafNodesPerPage + r->start;
    long hp_end = page_info->logpageno * LeafNodesPerPage + r->end;
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
  free(compressed.items);
}

static void print_leaf_expanded(FILE *out, const char *indent,
                                const FsmPageInfo *page_info,
                                const FsmOptions *options) {
  const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
  fprintf(out,
          "%s  leaf slots (%d, one per heap page starting at %ld, expanded)",
          indent, (int)LeafNodesPerPage,
          page_info->logpageno * LeafNodesPerPage);
  if (options->has_min_avail || options->has_max_avail)
    fprintf(out, " [filtered by avail_bytes]");
  fprintf(out, ":\n");
  for (long s = 0; s < LeafNodesPerPage; s++) {
    unsigned avail = cat_to_bytes(leaf[s]);
    if (!in_avail_range(options, avail)) continue;
    fprintf(out,
            "%s    slot %4ld (heap page %8ld): category=%3u avail_bytes=%5u\n",
            indent, s, page_info->logpageno * LeafNodesPerPage + s, leaf[s],
            avail);
  }
}

typedef struct {
  long n_root, n_internal, n_leaf, n_zero, n_invalid;
  uint8 global_max_cat;
  long global_max_cat_page, global_max_cat_slot;
  double total_avail;
  long total_slots;
  long cat_count[FSM_CATEGORIES];
} DumpStats;

static void dump_node(FILE *out, FsmPageInfo *pages, LongVec *children,
                      long total_pages, long id, const char *prefix,
                      int is_last, const FsmOptions *options,
                      DumpStats *stats) {
  FsmPageInfo *page_info = &pages[id];
  const char *branch = "\\-- ";
  const char *kind_name = page_info->level == 0   ? "ROOT"
                          : page_info->level == 1 ? "INTERNAL"
                                                  : "LEAF";
  int show = in_page_range(options, id);

  char child_indent[512];
  snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

  if (page_info->level == 0)
    stats->n_root++;
  else if (page_info->level == 1)
    stats->n_internal++;
  else
    stats->n_leaf++;

  if (page_info->allzero) {
    stats->n_zero++;
    if (show) {
      fprintf(out, "%s%s%ld fsm [%s] -- empty (uninitialized, all-zero page)\n",
              prefix, branch, id, kind_name);
      if (page_info->level == 2) {
        long hp_start = page_info->logpageno * LeafNodesPerPage;
        long hp_end = hp_start + LeafNodesPerPage - 1;
        fprintf(out, "%s  covers heap pages %ld-%ld\n\n", child_indent,
                hp_start, hp_end);
      }
    }
  } else if (page_info->header_status == HEADER_WRONG_PAGESIZE) {
    stats->n_invalid++;
    if (show) {
      fprintf(out,
              "%s%s%ld fsm [%s] -- WRONG PAGE SIZE, rebuild pg_fsm with the "
              "server's --with-blocksize\n",
              prefix, branch, id, kind_name);
      if (options->show_headers) print_header_line(out, prefix, page_info);
    }
  } else if (!page_info->valid) {
    stats->n_invalid++;
    if (show) {
      fprintf(out, "%s%s%ld fsm [%s] -- INVALID HEADER (%s)\n", prefix,
              branch, id, kind_name, page_info->invalid_reason);
      if (options->show_headers) print_header_line(out, prefix, page_info);
    }
  } else {
    if (show) {
      fprintf(out, "%s%s%ld fsm [%s]\n", prefix, branch, id, kind_name);
      if (options->show_headers) print_header_line(out, child_indent, page_info);
      if (options->show_internal) {
        if (options->expand)
          print_internal_expanded(out, child_indent, page_info);
        else
          print_internal_compressed(out, child_indent, page_info);
      }
    }
    if (page_info->level == 2) {
      const uint8 *leaf = page_info->nodes + NonLeafNodesPerPage;
      LeafPageAgg agg = compute_leaf_agg(leaf, LeafNodesPerPage);
      stats->total_avail += agg.total_avail;
      stats->total_slots += LeafNodesPerPage;
      for (long slot = 0; slot < LeafNodesPerPage; slot++)
        stats->cat_count[leaf[slot]]++;
      if (agg.max_cat > stats->global_max_cat) {
        stats->global_max_cat = agg.max_cat;
        stats->global_max_cat_page = id;
        stats->global_max_cat_slot = agg.max_slot;
      }
      if (show) {
        fprintf(out, "%s  page max: category=%u (%u bytes) at slot %ld\n",
                child_indent, agg.max_cat, cat_to_bytes(agg.max_cat),
                agg.max_slot);
        fprintf(out, "%s  Free space on page: %.0f \n\n",
                child_indent, agg.total_avail);
        if (options->show_slots) {
          if (options->expand)
            print_leaf_expanded(out, child_indent, page_info, options);
          else
            print_leaf_compressed(out, child_indent, page_info, options);
        }
      }
    }
  }

  if (id >= total_pages) return;
  LongVec *kids = &children[id];
  for (long i = 0; i < kids->count; i++)
    dump_node(out, pages, children, total_pages, kids->items[i], child_indent,
              i == kids->count - 1, options, stats);
}

static void report_heap_page_lookup(FILE *out, FsmPageInfo *pages,
                                    long total_pages,
                                    const FsmOptions *options) {
  long fsm_page, slot;
  heap_page_to_fsm_slot(options->heap_page_query, &fsm_page, &slot);

  fprintf(out, "heap page %ld -> fsm page %ld, slot %ld",
          options->heap_page_query, fsm_page, slot);

  if (fsm_page >= total_pages) {
    fprintf(out, " - file only has %ld page(s), not covered yet\n",
            total_pages);
    return;
  }
  FsmPageInfo *page_info = &pages[fsm_page];
  if (page_info->allzero) {
    fprintf(out, " - page is empty (uninitialized, all-zero)\n");
    return;
  }
  if (!page_info->valid) {
    fprintf(out, " - page header is bad (%s)\n", page_info->invalid_reason);
    return;
  }
  uint8 cat = page_info->nodes[NonLeafNodesPerPage + slot];
  fprintf(out, ": category=%u avail_bytes=%u\n", cat, cat_to_bytes(cat));
  if (options->show_headers) print_header_line(out, "", page_info);
}

static int do_fsm_dump(const char *in_path, const char *out_path,
                       const FsmOptions *options) {
  long total_pages;
  FsmPageInfo *pages = load_fsm(in_path, &total_pages);
  if (!pages) return 1;

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(pages);
    return 1;
  }

  if (options->has_heap_page) {
    fprintf(out, "=== pg_fsm dump: %s (--heap-page %ld lookup) ===\n",
            in_path, options->heap_page_query);
    report_heap_page_lookup(out, pages, total_pages, options);
    fclose(out);
    free(pages);
    fprintf(stderr, "wrote %s\n", out_path);
    return 0;
  }



  LongVec *children = build_fsm_children(total_pages);

  fprintf(out, "=== pg_fsm dump: %s ===\n", in_path);
  fprintf(out,
          "file size: %ld bytes, total pages: %ld (%d bytes/page)\n",
          total_pages * BLCKSZ, total_pages, BLCKSZ);

  fprintf(out, "flags: headers=%s internal=%s slots=%s expand=%s\n",
          options->show_headers ? "on" : "off",
          options->show_internal ? "on" : "off",
          options->show_slots ? "on" : "off", options->expand ? "on" : "off");
  if (options->has_range)
    fprintf(out,
            "printing only pages [%ld, %ld] (SUMMARY below still covers the "
            "whole file)\n",
            options->range_lo, options->range_hi);
  fprintf(out, "\n");

  DumpStats stats = {0};
  dump_node(out, pages, children, total_pages, 0, "", 1, options, &stats);

  if (options->stats) {
    fprintf(
        out,
        "\n--------------------------------------------------------------\n");
    fprintf(out, "SUMMARY\n");
    fprintf(out,
            "--------------------------------------------------------------\n");
    fprintf(out, "root pages: %ld  internal pages: %ld  leaf pages: %ld\n",
            stats.n_root, stats.n_internal, stats.n_leaf);
    fprintf(out, "zero/hole pages: %ld  invalid-header pages: %ld\n",
            stats.n_zero, stats.n_invalid);
    fprintf(
        out,
        "\nMax free-space category: %u (%u bytes), page %ld slot %ld\n",
        stats.global_max_cat, cat_to_bytes(stats.global_max_cat),
        stats.global_max_cat_page, stats.global_max_cat_slot);
    fprintf(out, "\nTotal avail_bytes : %.0f\n", stats.total_avail);

    fprintf(out, "\nheap pages by category:\n");
    for (int cat = 0; cat < FSM_CATEGORIES; cat++) {
      if (stats.cat_count[cat] == 0) continue;
      fprintf(out, "  category %3d (%5u bytes): %ld page(s)\n", cat,
              cat_to_bytes((uint8)cat), stats.cat_count[cat]);
    }
  }

  fclose(out);
  free_fsm_children(children, total_pages);
  free(pages);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

typedef enum { ST_SAME, ST_CHANGED, ST_ADDED, ST_REMOVED, ST_EMPTY } Status;

static Status page_status(const FsmPageInfo *old_page,
                          const FsmPageInfo *new_page) {
  int old_ok = old_page && !old_page->allzero && old_page->valid;
  int new_ok = new_page && !new_page->allzero && new_page->valid;
  if (!old_ok && new_ok) return ST_ADDED;
  if (old_ok && !new_ok) return ST_REMOVED;
  if (!old_ok && !new_ok) return ST_EMPTY;
  if (memcmp(old_page->nodes, new_page->nodes, NodesPerPage) != 0)
    return ST_CHANGED;
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

static void leaf_delta_push(LeafDeltaVec *v, long start, long end, long delta) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(LeafDelta));
  }
  v->items[v->count++] = (LeafDelta){start, end, delta};
}

static void print_internal_diff(FILE *out, int do_print, const char *indent,
                                const FsmPageInfo *old_page,
                                const FsmPageInfo *new_page,
                                const FsmOptions *options, DiffStats *stats) {
  ByteDiffCompressedRangeVec compressed = compute_byte_diff_compressed_ranges(
      old_page->nodes, new_page->nodes, NonLeafNodesPerPage);
  for (long i = 0; i < compressed.count; i++) {
    const ByteDiffCompressedRange *r = &compressed.items[i];
    if (r->old_value == r->new_value) continue;
    stats->slots_changed += r->end - r->start + 1;
    if (r->new_value > r->old_value)
      stats->slots_up += r->end - r->start + 1;
    else
      stats->slots_down += r->end - r->start + 1;
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
  free(compressed.items);
}

static void print_leaf_diff(FILE *out, int do_print, const char *indent,
                            const FsmPageInfo *old_page,
                            const FsmPageInfo *new_page,
                            const FsmOptions *options, DiffStats *stats,
                            LeafDeltaVec *deltas) {
  const uint8 *old_leaf = old_page->nodes + NonLeafNodesPerPage;
  const uint8 *new_leaf = new_page->nodes + NonLeafNodesPerPage;
  ByteDiffCompressedRangeVec compressed =
      compute_byte_diff_compressed_ranges(old_leaf, new_leaf, LeafNodesPerPage);
  for (long i = 0; i < compressed.count; i++) {
    const ByteDiffCompressedRange *r = &compressed.items[i];
    long n = r->end - r->start + 1;
    long hp_start = old_page->logpageno * LeafNodesPerPage + r->start;
    long hp_end = old_page->logpageno * LeafNodesPerPage + r->end;
    if (r->old_value == r->new_value) continue;
    unsigned ob = cat_to_bytes(r->old_value), nb = cat_to_bytes(r->new_value);
    stats->slots_changed += n;
    if (nb > ob)
      stats->slots_up += n;
    else
      stats->slots_down += n;
    leaf_delta_push(deltas, hp_start, hp_end, (long)nb - (long)ob);
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
  free(compressed.items);
}

static void diff_node(FILE *out, FsmPageInfo *A, long totalA, FsmPageInfo *B,
                      long totalB, LongVec *children, long id,
                      const char *prefix, int is_last,
                      const FsmOptions *options, DiffStats *stats,
                      LeafDeltaVec *deltas) {
  FsmPageInfo *old_page = id < totalA ? &A[id] : NULL;
  FsmPageInfo *new_page = id < totalB ? &B[id] : NULL;
  Status status = page_status(old_page, new_page);
  const char *branch = "\\-- ";
  FsmPageInfo *ref =
      (new_page && !new_page->allzero && new_page->valid) ? new_page : old_page;
  const char *kind_name = ref ? (ref->level == 0   ? "ROOT"
                             : ref->level == 1 ? "INTERNAL"
                                               : "LEAF")
                          : "?";

  int show = in_page_range(options, id) &&
             !(options->only_changed && (status == ST_SAME || status == ST_EMPTY));

  if (show)
    fprintf(out, "%s%s%ld fsm [%s] %s\n", prefix, branch, id, kind_name,
            status_name(status));

  char child_indent[512];
  snprintf(child_indent, sizeof(child_indent), "%s    ", prefix);

  if (status == ST_ADDED)
    stats->pages_added++;
  else if (status == ST_REMOVED)
    stats->pages_removed++;
  else if (status == ST_CHANGED) {
    stats->pages_changed++;
    int structural = old_page->header.pd_lower != new_page->header.pd_lower ||
                     old_page->header.pd_upper != new_page->header.pd_upper ||
                     old_page->header.pd_special != new_page->header.pd_special ||
                     old_page->header.pd_flags != new_page->header.pd_flags ||
                     old_page->header.pd_pagesize_version != new_page->header.pd_pagesize_version ||
                     old_page->fp_next_slot != new_page->fp_next_slot;
    int hdr_changed = structural ||
                       ((uint64) old_page->header.pd_lsn.xlogid << 32 | old_page->header.pd_lsn.xrecoff)
                       !=
                        ((uint64) new_page->header.pd_lsn.xlogid << 32 | new_page->header.pd_lsn.xrecoff) ||
                       old_page->header.pd_checksum != new_page->header.pd_checksum;
    if (hdr_changed) {
      stats->headers_changed++;
      if (structural) {
        stats->headers_structural_changed++;
        if (show)
          fprintf(out,
                  "%s  header changed (structural: lower/upper/special/"
                  "flags/version/fp_next_slot)\n",
                  child_indent);
      } else if (show && options->show_headers) {
        fprintf(out, "%s  header changed (lsn/checksum only)\n", child_indent);
      }
    }
    if (options->show_internal)
      print_internal_diff(out, show, child_indent, old_page, new_page,
                          options, stats);
    print_leaf_diff(out, show, child_indent, old_page, new_page, options,
                    stats, deltas);
  }

  if (id >= (totalA > totalB ? totalA : totalB)) return;
  LongVec *kids = &children[id];
  for (long i = 0; i < kids->count; i++)
    diff_node(out, A, totalA, B, totalB, children, kids->items[i], child_indent,
              i == kids->count - 1, options, stats, deltas);
}

static int do_fsm_diff(const char *old_path, const char *new_path,
                   const char *out_path, const FsmOptions *options) {
  long totalA, totalB;
  FsmPageInfo *A = load_fsm(old_path, &totalA);
  if (!A) return 1;
  FsmPageInfo *B = load_fsm(new_path, &totalB);
  if (!B) {
    free(A);
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
  if (options->only_changed)
    fprintf(out,
            "(--only-changed: SAME/empty subtrees and unchanged runs are "
            "hidden)\n");
  if (options->has_range)
    fprintf(out, "printing only pages [%ld, %ld]\n", options->range_lo,
            options->range_hi);
  fprintf(out, "\n");
  if (totalB > totalA)
    fprintf(out, "file GREW by %ld page(s)\n\n", totalB - totalA);
  else if (totalA > totalB)
    fprintf(out, "file SHRANK by %ld page(s)\n\n", totalA - totalB);

  DiffStats stats = {0};
  LeafDeltaVec deltas = {0};
  diff_node(out, A, totalA, B, totalB, children, 0, "", 1, options, &stats,
            &deltas);

  for (long page_index = 0; page_index < totalA; page_index++)
    if (A[page_index].level == 0 &&
        (A[page_index].allzero || A[page_index].valid)) {
      const uint8 *leaf = A[page_index].nodes + NonLeafNodesPerPage;
      stats.old_total_avail +=
          compute_leaf_agg(leaf, LeafNodesPerPage).total_avail;
    }
  for (long page_index = 0; page_index < totalB; page_index++)
    if (B[page_index].level == 0 &&
        (B[page_index].allzero || B[page_index].valid)) {
      const uint8 *leaf = B[page_index].nodes + NonLeafNodesPerPage;
      stats.new_total_avail +=
          compute_leaf_agg(leaf, LeafNodesPerPage).total_avail;
    }

  if (options->stats) {
    fprintf(
        out,
        "\n--------------------------------------------------------------\n");
    fprintf(out, "SUMMARY\n");
    fprintf(out,
            "--------------------------------------------------------------\n");
    fprintf(out,
            "pages changed: %ld  added: %ld  removed: %ld  headers changed: "
            "%ld \n",
            stats.pages_changed, stats.pages_added, stats.pages_removed,
            stats.headers_changed);
    fprintf(out,
            "node/slots changed: %ld (gained space: %ld, lost space: %ld)\n",
            stats.slots_changed, stats.slots_up, stats.slots_down);
    fprintf(out, "total avail_bytes: old=%.0f new=%.0f (delta %+.0f)\n",
            stats.old_total_avail, stats.new_total_avail,
            stats.new_total_avail - stats.old_total_avail);
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
          "  %s -F dump [flags] <relfilenode_fsm> <out.txt>\n"
          "  %s -F diff [flags] <old_fsm> <new_fsm> <out.txt>\n"
          "flags:\n"
          "  -H                headers (incl. fp_next_slot)  -i internal "
          "array (dump only)\n"
          "  -s                leaf slots (dump only)        -a all of "
          "the above\n"
          "  -q                add summary\n"
          "  --range A-B       only print pages in [A,B] (children still "
          "traversed)\n"
          "  --page N          shorthand for --range N-N\n"
          "  --extra          full per-slot/per-node listing instead of "
          "compressed ranges (requires --range/--page)\n"
          "  --heap-page N     dump only: locate heap page N in the FSM "
          "tree\n"
          "  --min N     dump -s only: filter leaf slot ranges by "
          "avail_bytes\n"
          "  --max N\n"
          "  --only-changed    diff only: hide SAME/empty subtrees and "
          "unchanged runs\n",
          prog, prog);
}

static void parse_fsm_flags(int argc, char **argv, int start,
                            FsmOptions *options, char **pos, int *npos) {
  *npos = 0;
  for (int i = start; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "--range") == 0 && i + 1 < argc) {
      long lo, hi;
      if (sscanf(argv[++i], "%ld-%ld", &lo, &hi) == 2) {
        options->has_range = 1;
        options->range_lo = lo;
        options->range_hi = hi;
      } else {
        fprintf(stderr, "bad --range %s, want A-B\n", argv[i]);
      }
    } else if (strcmp(a, "--page") == 0 && i + 1 < argc) {
      options->has_range = 1;
      options->range_lo = options->range_hi = atol(argv[++i]);
    } else if (strcmp(a, "--extra") == 0) {
      options->expand = 1;
    } else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc) {
      options->has_heap_page = 1;
      options->heap_page_query = atol(argv[++i]);
    } else if (strcmp(a, "--min") == 0 && i + 1 < argc) {
      options->has_min_avail = 1;
      options->min_avail = atol(argv[++i]);
    } else if (strcmp(a, "--max") == 0 && i + 1 < argc) {
      options->has_max_avail = 1;
      options->max_avail = atol(argv[++i]);
    } else if (strcmp(a, "--only-changed") == 0) {
      options->only_changed = 1;
    } else if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
      for (const char *c = a + 1; *c; c++) {
        switch (*c) {
          case 'H':
            options->show_headers = 1;
            break;
          case 'i':
            options->show_internal = 1;
            break;
          case 's':
            options->show_slots = 1;
            break;
          case 'a':
            options->show_headers = options->show_internal =
                options->show_slots = 1;
            break;
          case 'q':
            options->stats = 1;
            break;
          default:
            fprintf(stderr, "unknown flag -%c\n", *c);
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

  FsmOptions options = {0};
  options.stats = 0;
  char *pos[8];
  int npos = 0;

  if (strcmp(argv[1], "dump") == 0) {
    parse_fsm_flags(argc, argv, 2, &options, pos, &npos);
    if (npos != 2) {
      fsm_usage(argv[0]);
      return 1;
    }
    return do_fsm_dump(pos[0], pos[1], &options);
  } else if (strcmp(argv[1], "diff") == 0) {
    parse_fsm_flags(argc, argv, 2, &options, pos, &npos);
    if (npos != 3) {
      fsm_usage(argv[0]);
      return 1;
    }
    return do_fsm_diff(pos[0], pos[1], pos[2], &options);
  }

  fsm_usage(argv[0]);
  return 1;
}