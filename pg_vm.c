/*
 * Flags:
 *   -H                 print a per-physical-page header inventory        
 *   -q                 add summary block at the end
 *   --heap-range A-B   only process heap pages [A,B] 
 *   --heap-page N      dump only: status of one heap page
 *   --expand           print one line per heap page 
 *   --only-not-visible dump: only print ranges where ALL_VISIBLE 
 *   --only-not-frozen  dump: only print ranges where ALL_FROZEN 
 *   --only-changed     diff: 
 */

#include "postgres.h"
#include "pg_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "access/visibilitymapdefs.h"
#include "storage/bufpage.h"

#define MAP_SIZE ((int)(BLCKSZ - MAXALIGN(SizeOfPageHeaderData)))
#define HEAPBLOCKS_PER_BYTE (8 / BITS_PER_HEAPBLOCK) /* 4 */
#define HEAPBLOCKS_PER_PAGE (MAP_SIZE * HEAPBLOCKS_PER_BYTE)

#define VM_STATUS_OUT_OF_FILE (-1)
#define VM_STATUS_CORRUPT (-2)

typedef struct {
  long page;
  int valid;
  int allzero;
  uint16 pd_flags;
  uint16 pd_checksum;
  uint64 lsn;
  LocationIndex pd_lower, pd_upper, pd_special;
  uint16 pd_pagesize_version;
  uint8 bitmap[MAP_SIZE];
} VmPageInfo;

static int vm_page_is_all_zero(const uint8 *buf) {
  for (int i = 0; i < BLCKSZ; i++)
    if (buf[i] != 0) return 0;
  return 1;
}

static int vm_header_looks_valid(PageHeader ph) {
  uint16 pagesize = ph->pd_pagesize_version & 0xFF00;
  uint16 version = ph->pd_pagesize_version & 0x00FF;
  if (pagesize != BLCKSZ) return 0;
  if (version == 0 || version > PG_PAGE_LAYOUT_VERSION) return 0;
  if (ph->pd_special > BLCKSZ) return 0;
  if (ph->pd_lower > ph->pd_upper) return 0;
  if (ph->pd_upper > ph->pd_special) return 0;
  return 1;
}

static VmPageInfo *load_vm(const char *path, long *out_total_pages) {
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
    fprintf(stderr, "not a valid vm file (empty or truncated): %s\n", path);
    fclose(f);
    return NULL;
  }

  VmPageInfo *pages = calloc(total_pages, sizeof(VmPageInfo));
  uint8 buf[BLCKSZ];
  for (long p = 0; p < total_pages; p++) {
    if (fread(buf, 1, BLCKSZ, f) != (size_t)BLCKSZ) {
      fprintf(stderr, "warning: short read at page %ld, treating as zero\n", p);
      memset(buf, 0, BLCKSZ);
    }
    VmPageInfo *pi = &pages[p];
    pi->page = p;
    pi->allzero = vm_page_is_all_zero(buf);

    PageHeader ph = (PageHeader)buf;
    pi->valid = !pi->allzero && vm_header_looks_valid(ph);
    pi->pd_flags = ph->pd_flags;
    pi->pd_checksum = ph->pd_checksum;
    pi->lsn = PageGetLSN((Page)buf);
    pi->pd_lower = ph->pd_lower;
    pi->pd_upper = ph->pd_upper;
    pi->pd_special = ph->pd_special;
    pi->pd_pagesize_version = ph->pd_pagesize_version;

    if (pi->allzero || pi->valid)
      memcpy(pi->bitmap, PageGetContents((Page)buf), MAP_SIZE);
    else
      memset(pi->bitmap, 0, MAP_SIZE); /* corrupt page: don't trust its bits */
  }
  fclose(f);
  *out_total_pages = total_pages;
  return pages;
}

static int heap_page_status(const VmPageInfo *pages, long total_pages,
                            long heap_page) {
  long vm_page = heap_page / HEAPBLOCKS_PER_PAGE;
  if (vm_page >= total_pages) return VM_STATUS_OUT_OF_FILE;

  const VmPageInfo *pi = &pages[vm_page];
  if (!pi->allzero && !pi->valid) return VM_STATUS_CORRUPT;

  long offset = heap_page % HEAPBLOCKS_PER_PAGE;
  long byte_idx = offset / HEAPBLOCKS_PER_BYTE;
  int bit_shift = (int)(offset % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK;
  return (pi->bitmap[byte_idx] >> bit_shift) & VISIBILITYMAP_VALID_BITS;
}

typedef struct {
  long heap_start, heap_end; /* inclusive */
  int status;
} VmRun;

typedef struct {
  VmRun *items;
  long count, cap;
} VmRunVec;

static void run_push(VmRunVec *v, long start, long end, int status) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(VmRun));
  }
  v->items[v->count++] = (VmRun){start, end, status};
}

static VmRunVec compute_runs(const VmPageInfo *pages, long total_pages,
                             long from, long to) {
  VmRunVec runs = {0};
  if (to < from) return runs;

  long run_start = from;
  int run_status = heap_page_status(pages, total_pages, from);
  for (long hp = from + 1; hp <= to; hp++) {
    int st = heap_page_status(pages, total_pages, hp);
    if (st != run_status) {
      run_push(&runs, run_start, hp - 1, run_status);
      run_start = hp;
      run_status = st;
    }
  }
  run_push(&runs, run_start, to, run_status);
  return runs;
}

typedef struct {
  long heap_start, heap_end;
  int old_status, new_status;
} VmDiffRun;

typedef struct {
  VmDiffRun *items;
  long count, cap;
} VmDiffRunVec;

static void diffrun_push(VmDiffRunVec *v, long start, long end, int os,
                         int ns) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(VmDiffRun));
  }
  v->items[v->count++] = (VmDiffRun){start, end, os, ns};
}

static VmDiffRunVec compute_diff_runs(const VmPageInfo *A, long totalA,
                                      const VmPageInfo *B, long totalB,
                                      long from, long to) {
  VmDiffRunVec runs = {0};
  if (to < from) return runs;

  long run_start = from;
  int old_st = heap_page_status(A, totalA, from);
  int new_st = heap_page_status(B, totalB, from);
  for (long hp = from + 1; hp <= to; hp++) {
    int os = heap_page_status(A, totalA, hp);
    int ns = heap_page_status(B, totalB, hp);
    if (os != old_st || ns != new_st) {
      diffrun_push(&runs, run_start, hp - 1, old_st, new_st);
      run_start = hp;
      old_st = os;
      new_st = ns;
    }
  }
  diffrun_push(&runs, run_start, to, old_st, new_st);
  return runs;
}

static const char *status_label(int status) {
  switch (status) {
    case 0:
      return "none";
    case VISIBILITYMAP_ALL_VISIBLE:
      return "visible";
    case VISIBILITYMAP_ALL_FROZEN:
      return "frozen-only(!)";
    case VISIBILITYMAP_ALL_VISIBLE | VISIBILITYMAP_ALL_FROZEN:
      return "visible+frozen";
    case VM_STATUS_OUT_OF_FILE:
      return "out-of-file";
    case VM_STATUS_CORRUPT:
      return "CORRUPT-HEADER";
    default:
      return "?";
  }
}

static int status_has_visible(int status) {
  return status > 0 && (status & VISIBILITYMAP_ALL_VISIBLE);
}
static int status_has_frozen(int status) {
  return status > 0 && (status & VISIBILITYMAP_ALL_FROZEN);
}

static void print_page_inventory(FILE *out, const VmPageInfo *pages,
                                 long total_pages) {
  fprintf(out, "\n-- physical page inventory (-H) --\n");
  for (long p = 0; p < total_pages; p++) {
    const VmPageInfo *pi = &pages[p];
    if (pi->allzero) {
      fprintf(out, "vm page %ld: empty (all-zero)\n", p);
      continue;
    }
    if (!pi->valid) {
      fprintf(out, "vm page %ld: INVALID HEADER\n", p);
      continue;
    }
    fprintf(out,
            "vm page %ld: lsn=%llX checksum=%u flags=0x%x lower=%u upper=%u "
            "special=%u\n",
            p, (unsigned long long)pi->lsn, pi->pd_checksum, pi->pd_flags,
            pi->pd_lower, pi->pd_upper, pi->pd_special);
  }
}

static void print_runs(FILE *out, const VmRunVec *runs, const VmOptions *opts) {
  for (long i = 0; i < runs->count; i++) {
    const VmRun *r = &runs->items[i];
    if (opts->only_not_visible && status_has_visible(r->status)) continue;
    if (opts->only_not_frozen && status_has_frozen(r->status)) continue;
    if (r->heap_start == r->heap_end)
      fprintf(out, "heap page %8ld           : %s\n", r->heap_start,
              status_label(r->status));
    else
      fprintf(out, "heap pages %8ld-%-8ld: %s (%ld page(s))\n", r->heap_start,
              r->heap_end, status_label(r->status),
              r->heap_end - r->heap_start + 1);
  }
}

static void print_expanded(FILE *out, const VmPageInfo *pages, long total_pages,
                           long from, long to, const VmOptions *opts) {
  for (long hp = from; hp <= to; hp++) {
    int st = heap_page_status(pages, total_pages, hp);
    if (opts->only_not_visible && status_has_visible(st)) continue;
    if (opts->only_not_frozen && status_has_frozen(st)) continue;
    fprintf(out, "heap page %8ld: %s\n", hp, status_label(st));
  }
}

static void print_diff_runs(FILE *out, const VmDiffRunVec *runs,
                            const VmOptions *opts) {
  for (long i = 0; i < runs->count; i++) {
    const VmDiffRun *r = &runs->items[i];
    if (opts->only_changed && r->old_status == r->new_status) continue;
    const char *tag = r->old_status == r->new_status ? "same" : "CHANGED";
    if (r->heap_start == r->heap_end)
      fprintf(out, "heap page %8ld           : %s -> %s  [%s]\n", r->heap_start,
              status_label(r->old_status), status_label(r->new_status), tag);
    else
      fprintf(out, "heap pages %8ld-%-8ld: %s -> %s  [%s] (%ld page(s))\n",
              r->heap_start, r->heap_end, status_label(r->old_status),
              status_label(r->new_status), tag,
              r->heap_end - r->heap_start + 1);
  }
}

static void print_expanded_diff(FILE *out, const VmPageInfo *A, long totalA,
                                const VmPageInfo *B, long totalB, long from,
                                long to, const VmOptions *opts) {
  for (long hp = from; hp <= to; hp++) {
    int os = heap_page_status(A, totalA, hp);
    int ns = heap_page_status(B, totalB, hp);
    if (opts->only_changed && os == ns) continue;
    fprintf(out, "heap page %8ld: %s -> %s  [%s]\n", hp, status_label(os),
            status_label(ns), os == ns ? "same" : "CHANGED");
  }
}

static long default_heap_to(long total_pages) {
  return total_pages * (long)HEAPBLOCKS_PER_PAGE - 1;
}

static int do_vm_dump(const char *in_path, const char *out_path,
                       const VmOptions *opts) {
  long total_pages;
  VmPageInfo *pages = load_vm(in_path, &total_pages);
  if (!pages) return 1;

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(pages);
    return 1;
  }

  if (opts->has_heap_page) {
    int st = heap_page_status(pages, total_pages, opts->heap_page_query);
    fprintf(out, "=== pg_vm dump: %s (--heap-page %ld lookup) ===\n", in_path,
            opts->heap_page_query);
    fprintf(out, "heap page %ld: %s\n", opts->heap_page_query,
            status_label(st));
    fclose(out);
    free(pages);
    fprintf(stderr, "wrote %s\n", out_path);
    return 0;
  }

  if (opts->expand && !opts->has_heap_range) {
    fprintf(stderr,
            "--expand needs --heap-range A-B (refusing to print every heap "
            "page of the whole file)\n");
    fclose(out);
    free(pages);
    return 1;
  }

  long from = opts->has_heap_range ? opts->heap_from : 0;
  long to = opts->has_heap_range ? opts->heap_to : default_heap_to(total_pages);

  fprintf(out, "=== pg_vm dump: %s ===\n", in_path);
  fprintf(out,
          "file size: %ld bytes, total pages: %ld, %d heap page(s)/vm page, "
          "%d bytes/page\n",
          total_pages * BLCKSZ, total_pages, HEAPBLOCKS_PER_PAGE, BLCKSZ);
  fprintf(out, "heap page range covered: [%ld, %ld]\n", from, to);
  fprintf(out, "\n");

  if (opts->show_headers) print_page_inventory(out, pages, total_pages);

  fprintf(out, "\n-- heap page status%s --\n",
          opts->expand ? " (expanded, one line per page)"
                       : " (run-length compressed)");
  if (opts->expand) {
    print_expanded(out, pages, total_pages, from, to, opts);
  } else {
    VmRunVec runs = compute_runs(pages, total_pages, from, to);
    print_runs(out, &runs, opts);
    if (opts->stats) {
      long visible = 0, frozen = 0, corrupt_runs = 0;
      for (long i = 0; i < runs.count; i++) {
        long n = runs.items[i].heap_end - runs.items[i].heap_start + 1;
        if (status_has_visible(runs.items[i].status)) visible += n;
        if (status_has_frozen(runs.items[i].status)) frozen += n;
        if (runs.items[i].status == VM_STATUS_CORRUPT) corrupt_runs++;
      }
      fprintf(out,
              "\n--------------------------------------------------------------"
              "\nSUMMARY\n--------------------------------------------------"
              "------------\n");
      fprintf(out, "heap pages in range: %ld, compressed into %ld run(s)\n",
              to - from + 1, runs.count);
      fprintf(out, "all-visible: %ld  all-frozen: %ld  corrupt page(s): %ld\n",
              visible, frozen, corrupt_runs);
    }
    free(runs.items);
  }

  fclose(out);
  free(pages);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

static int do_vm_diff(const char *old_path, const char *new_path,
                   const char *out_path, const VmOptions *opts) {
  long totalA, totalB;
  VmPageInfo *A = load_vm(old_path, &totalA);
  if (!A) return 1;
  VmPageInfo *B = load_vm(new_path, &totalB);
  if (!B) {
    free(A);
    return 1;
  }

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(A);
    free(B);
    return 1;
  }

  if (opts->expand && !opts->has_heap_range) {
    fprintf(stderr,
            "--expand needs --heap-range A-B (refusing to print every heap "
            "page of the whole file)\n");
    fclose(out);
    free(A);
    free(B);
    return 1;
  }

  long total_max = totalA > totalB ? totalA : totalB;
  long from = opts->has_heap_range ? opts->heap_from : 0;
  long to = opts->has_heap_range ? opts->heap_to : default_heap_to(total_max);

  fprintf(out,
          "=== pg_vm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n"
          "heap page range covered: [%ld, %ld]\n\n",
          old_path, totalA, new_path, totalB, from, to);

  if (opts->expand) {
    print_expanded_diff(out, A, totalA, B, totalB, from, to, opts);
  } else {
    VmDiffRunVec runs = compute_diff_runs(A, totalA, B, totalB, from, to);
    print_diff_runs(out, &runs, opts);
    if (opts->stats) {
      long changed_pages = 0, gained_visible = 0, lost_visible = 0,
           gained_frozen = 0, lost_frozen = 0;
      for (long i = 0; i < runs.count; i++) {
        const VmDiffRun *r = &runs.items[i];
        long n = r->heap_end - r->heap_start + 1;
        if (r->old_status == r->new_status) continue;
        changed_pages += n;
        int ov = status_has_visible(r->old_status),
            nv = status_has_visible(r->new_status);
        int of = status_has_frozen(r->old_status),
            nf = status_has_frozen(r->new_status);
        if (!ov && nv) gained_visible += n;
        if (ov && !nv) lost_visible += n;
        if (!of && nf) gained_frozen += n;
        if (of && !nf) lost_frozen += n;
      }
      fprintf(out,
              "\n--------------------------------------------------------------"
              "\nSUMMARY\n--------------------------------------------------"
              "------------\n");
      fprintf(out, "heap pages in range: %ld, compressed into %ld run(s)\n",
              to - from + 1, runs.count);
      fprintf(out, "changed heap pages: %ld\n", changed_pages);
      fprintf(out, "gained all-visible: %ld  lost all-visible: %ld\n",
              gained_visible, lost_visible);
      fprintf(out, "gained all-frozen: %ld  lost all-frozen: %ld\n",
              gained_frozen, lost_frozen);
    }
    free(runs.items);
  }

  fclose(out);
  free(A);
  free(B);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

static void vm_usage(const char *prog) {
  fprintf(stderr,
          "usage:\n"
          "  %s dump [flags] <relfilenode_vm> <out.txt>\n"
          "  %s diff [flags] <old_vm> <new_vm> <out.txt>\n"
          "flags:\n"
          "  -H                  per-physical-page header inventory\n"
          "  -q                  add summary\n"
          "  --heap-range A-B    restrict to heap pages [A,B] (default: "
          "whole file)\n"
          "  --heap-page N       dump only: status of one heap "
          "page\n"
          "  --expand            one line per heap page \n"
          "  --only-not-visible  dump: only ranges missing ALL_VISIBLE\n"
          "  --only-not-frozen   dump: only ranges missing ALL_FROZEN\n"
          "  --only-changed      diff: only ranges whose status changed\n",
          prog, prog);
}

static void parse_vm_flags(int argc, char **argv, int start, VmOptions *opts,
                        char **pos, int *npos) {
  *npos = 0;
  for (int i = start; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "--heap-range") == 0 && i + 1 < argc) {
      long lo, hi;
      if (sscanf(argv[++i], "%ld-%ld", &lo, &hi) == 2) {
        opts->has_heap_range = 1;
        opts->heap_from = lo;
        opts->heap_to = hi;
      } else {
        fprintf(stderr, "bad --heap-range value %s, expected A-B (ignored)\n",
                argv[i]);
      }
    } else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc) {
      opts->has_heap_page = 1;
      opts->heap_page_query = atol(argv[++i]);
    } else if (strcmp(a, "--expand") == 0) {
      opts->expand = 1;
    } else if (strcmp(a, "--only-not-visible") == 0) {
      opts->only_not_visible = 1;
    } else if (strcmp(a, "--only-not-frozen") == 0) {
      opts->only_not_frozen = 1;
    } else if (strcmp(a, "--only-changed") == 0) {
      opts->only_changed = 1;
    } else if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
      for (const char *c = a + 1; *c; c++) {
        switch (*c) {
          case 'H':
            opts->show_headers = 1;
            break;
          case 'q':
            opts->stats = 1;
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

int vm_main(int argc, char **argv) {
  if (argc < 2) {
    vm_usage(argv[0]);
    return 1;
  }

  VmOptions opts = {0};
  opts.stats = 0;
  char *pos[8];
  int npos = 0;

  if (strcmp(argv[1], "dump") == 0) {
    parse_vm_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 2) {
      vm_usage(argv[0]);
      return 1;
    }
    return do_vm_dump(pos[0], pos[1], &opts);
  } else if (strcmp(argv[1], "diff") == 0) {
    parse_vm_flags(argc, argv, 2, &opts, pos, &npos);
    if (npos != 3) {
      vm_usage(argv[0]);
      return 1;
    }
    return do_vm_diff(pos[0], pos[1], pos[2], &opts);
  }

  vm_usage(argv[0]);
  return 1;
}