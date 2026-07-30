/*
 *   -H                 print header        
 *   -q                 add summary block at the end
 *   --heap-range A-B   heap pages [A,B] 
 *   --heap-page N      dump only: status of one heap page
 *   --extra            print one line per heap page 
 *   --only-not-visible dump: only print ranges where ALL_VISIBLE 
 *   --only-not-frozen  dump: only print ranges where ALL_FROZEN 
 * 
 *   --notF
 *   --notV
 * 
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

typedef enum {
  HEADER_OK,
  HEADER_WRONG_PAGESIZE,
  HEADER_INVALID
} HeaderStatus;


typedef struct {
  long page;

  int valid;
  int allzero;

  HeaderStatus header_status;
  const char *invalid_reason;

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

static HeaderStatus fsm_header_status(PageHeader page_header, const char **reason) {
  uint16 pagesize = page_header->pd_pagesize_version & 0xFF00;
  uint16 version = page_header->pd_pagesize_version & 0x00FF;

  if (pagesize != BLCKSZ) {
    *reason =
        "page size in header does not match BLCKSZ this binary was built with";
    return HEADER_WRONG_PAGESIZE;
  }
  if (version == 0 || version > PG_PAGE_LAYOUT_VERSION) {
    *reason = "bad page layout version";
    return HEADER_INVALID;
  }
  if (page_header->pd_special > BLCKSZ) {
    *reason = "pd_special is larger than the page";
    return HEADER_INVALID;
  }
  if (page_header->pd_lower > page_header->pd_upper) {
    *reason = "pd_lower is greater than pd_upper";
    return HEADER_INVALID;
  }
  if (page_header->pd_upper > page_header->pd_special) {
    *reason = "pd_upper is greater than pd_special";
    return HEADER_INVALID;
  }
  *reason = NULL;
  return HEADER_OK;
}

// static int vm_header_looks_valid(PageHeader page_header) {
//   uint16 pagesize = page_header->pd_pagesize_version & 0xFF00;
//   uint16 version = page_header->pd_pagesize_version & 0x00FF;
//   if (pagesize != BLCKSZ) return 0;
//   if (version == 0 || version > PG_PAGE_LAYOUT_VERSION) return 0;
//   if (page_header->pd_special > BLCKSZ) return 0;
//   if (page_header->pd_lower > page_header->pd_upper) return 0;
//   if (page_header->pd_upper > page_header->pd_special) return 0;
//   return 1;
// }

static void fill_fsm_page_info(const uint8 *buf, long page_index,
                               VmPageInfo *page_info) {
                               
  page_info->page = page_index;
  page_info->page = page_index;
  page_info->allzero = vm_page_is_all_zero(buf);

  PageHeader page_header = (PageHeader)buf;
  const char *reason = NULL;
  page_info->header_status = page_info->allzero
                                ? HEADER_OK
                                : fsm_header_status(page_header, &reason);
  page_info->invalid_reason = reason;
  page_info->valid =
    !page_info->allzero && page_info->header_status == HEADER_OK;

  page_info->pd_flags = page_header->pd_flags;
  page_info->pd_checksum = page_header->pd_checksum;
  page_info->lsn = PageGetLSN((Page)buf);
  page_info->pd_lower = page_header->pd_lower;
  page_info->pd_upper = page_header->pd_upper;
  page_info->pd_special = page_header->pd_special;
  page_info->pd_pagesize_version = page_header->pd_pagesize_version;

  if (page_info->allzero || page_info->valid)
    memcpy(page_info->bitmap, PageGetContents((Page)buf), MAP_SIZE);
  else
    memset(page_info->bitmap, 0, MAP_SIZE); 


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
  for (long page_index = 0; page_index < total_pages; page_index++) {
    if (fread(buf, 1, BLCKSZ, f) != (size_t)BLCKSZ) {
      fprintf(stderr, "warning: short read at page %ld, treating as zero\n", page_index);
      memset(buf, 0, BLCKSZ);
    }
    fill_fsm_page_info(buf, page_index, &pages[page_index]);
  }
  fclose(f);
  *out_total_pages = total_pages;
  return pages;
}

static int heap_page_status(const VmPageInfo *pages, long total_pages,
                            long heap_page) {
  long vm_page = heap_page / HEAPBLOCKS_PER_PAGE;
  if (vm_page >= total_pages) return VM_STATUS_OUT_OF_FILE;

  const VmPageInfo *page_info = &pages[vm_page];
  if (!page_info->allzero && !page_info->valid) return VM_STATUS_CORRUPT;

  long offset = heap_page % HEAPBLOCKS_PER_PAGE;
  long byte_idx = offset / HEAPBLOCKS_PER_BYTE;
  int bit_shift = (int)(offset % HEAPBLOCKS_PER_BYTE) * BITS_PER_HEAPBLOCK;
  return (page_info->bitmap[byte_idx] >> bit_shift) & VISIBILITYMAP_VALID_BITS;
}

typedef struct {
  long heap_start, heap_end;
  int status;
} VmRun;

typedef struct {
  VmRun *items;
  long count, cap;
} VmRunVec;

static void compressed_push(VmRunVec *v, long start, long end, int status) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(VmRun));
  }
  v->items[v->count++] = (VmRun){start, end, status};
}

static VmRunVec compute_compressed(const VmPageInfo *pages, long total_pages,
                             long from, long to) {
  VmRunVec compressed = {0};
  if (to < from) return compressed;

  long run_start = from;
  int run_status = heap_page_status(pages, total_pages, from);
  for (long hp = from + 1; hp <= to; hp++) {
    int status = heap_page_status(pages, total_pages, hp);
    if (status != run_status) {
      compressed_push(&compressed, run_start, hp - 1, run_status);
      run_start = hp;
      run_status = status;
    }
  }
  compressed_push(&compressed, run_start, to, run_status);
  return compressed;
}

typedef struct {
  long heap_start, heap_end;
  int old_status, new_status;
} VmDiffRun;

typedef struct {
  VmDiffRun *items;
  long count, cap;
} VmDiffRunVec;

static void diffcompressed_push(VmDiffRunVec *v, long start, long end, int os,
                         int ns) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 16;
    v->items = realloc(v->items, v->cap * sizeof(VmDiffRun));
  }
  v->items[v->count++] = (VmDiffRun){start, end, os, ns};
}

static VmDiffRunVec compute_diff_compressed(const VmPageInfo *A, long totalA,
                                      const VmPageInfo *B, long totalB,
                                      long from, long to) {
  VmDiffRunVec compressed = {0};
  if (to < from) return compressed;

  long run_start = from;
  int old_st = heap_page_status(A, totalA, from);
  int new_st = heap_page_status(B, totalB, from);
  for (long hp = from + 1; hp <= to; hp++) {
    int os = heap_page_status(A, totalA, hp);
    int ns = heap_page_status(B, totalB, hp);
    if (os != old_st || ns != new_st) {
      diffcompressed_push(&compressed, run_start, hp - 1, old_st, new_st);
      run_start = hp;
      old_st = os;
      new_st = ns;
    }
  }
  diffcompressed_push(&compressed, run_start, to, old_st, new_st);
  return compressed;
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

static void print_page_headers(FILE *out, const VmPageInfo *pages,
                                 long total_pages) {
  fprintf(out, "\n-- page_headerysical page headers (-H) --\n");
  for (long page_index = 0; page_index < total_pages; page_index++) {
    const VmPageInfo *page_info = &pages[page_index];
    if (page_info->allzero) {
      fprintf(out, "vm page %ld: empty (all-zero)\n", page_index);
      continue;
    }
    if (page_info->header_status == HEADER_INVALID) {
      fprintf(out, "vm page %ld: header valid: no (%s)\n", page_index, page_info->invalid_reason);
      continue;
    }
    fprintf(out,
            "vm page %ld: lsn=%llX checksum=%u flags=0x%x lower=%u upper=%u "
            "special=%u\n",
            page_index, (unsigned long long)page_info->lsn, page_info->pd_checksum, page_info->pd_flags,
            page_info->pd_lower, page_info->pd_upper, page_info->pd_special);

    }
}

static void print_compressed(FILE *out, const VmRunVec *compressed, const VmOptions *options) {
  for (long i = 0; i < compressed->count; i++) {
    const VmRun *r = &compressed->items[i];
    if (options->only_not_visible && status_has_visible(r->status)) continue;
    if (options->only_not_frozen && status_has_frozen(r->status)) continue;
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
                           long from, long to, const VmOptions *options) {
  for (long hp = from; hp <= to; hp++) {
    int status = heap_page_status(pages, total_pages, hp);
    if (options->only_not_visible && status_has_visible(status)) continue;
    if (options->only_not_frozen && status_has_frozen(status)) continue;
    fprintf(out, "heap page %8ld: %s\n", hp, status_label(status));
  }
}

static void print_diff_compressed(FILE *out, const VmDiffRunVec *compressed,
                            const VmOptions *options) {
  for (long i = 0; i < compressed->count; i++) {
    const VmDiffRun *r = &compressed->items[i];
    if (options->only_changed && r->old_status == r->new_status) continue;
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
                                long to, const VmOptions *options) {
  for (long hp = from; hp <= to; hp++) {
    int os = heap_page_status(A, totalA, hp);
    int ns = heap_page_status(B, totalB, hp);
    if (options->only_changed && os == ns) continue;
    fprintf(out, "heap page %8ld: %s -> %s  [%s]\n", hp, status_label(os),
            status_label(ns), os == ns ? "same" : "CHANGED");
  }
}

static long default_heap_to(long total_pages) {
  return total_pages * (long)HEAPBLOCKS_PER_PAGE - 1;
}

static int do_vm_dump(const char *in_path, const char *out_path,
                       const VmOptions *options) {
  long total_pages;
  VmPageInfo *pages = load_vm(in_path, &total_pages);
  if (!pages) return 1;

  FILE *out = fopen(out_path, "w");
  if (!out) {
    perror(out_path);
    free(pages);
    return 1;
  }

  if (options->has_heap_page) {
    int status = heap_page_status(pages, total_pages, options->heap_page_query);
    fprintf(out, "=== pg_vm dump: %s (--heap-page %ld lookup) ===\n", in_path,
            options->heap_page_query);
    fprintf(out, "heap page %ld: %s\n", options->heap_page_query,
            status_label(status));
    fclose(out);
    free(pages);
    fprintf(stderr, "wrote %s\n", out_path);
    return 0;
  }


  long from = options->has_heap_range ? options->heap_from : 0;
  long to = options->has_heap_range ? options->heap_to : default_heap_to(total_pages);

  fprintf(out, "=== pg_vm dump: %s ===\n", in_path);
  fprintf(out,
          "file size: %ld bytes, total pages: %ld, "
          "%d bytes/page\n",
          total_pages * BLCKSZ, total_pages, BLCKSZ);
  fprintf(out, "heap page range covered: [%ld, %ld]\n", from, to);
  fprintf(out, "\n");

  if (options->show_headers) print_page_headers(out, pages, total_pages);

  fprintf(out, "\n-- heap page status%s --\n",
          options->expand ? " (expanded)"
                       : " (compressed)");
  if (options->expand) {
    print_expanded(out, pages, total_pages, from, to, options);
  } else {
    VmRunVec compressed = compute_compressed(pages, total_pages, from, to);
    print_compressed(out, &compressed, options);
    if (options->stats) {
      long visible = 0, frozen = 0, corrupt_compressed = 0;
      for (long i = 0; i < compressed.count; i++) {
        long n = compressed.items[i].heap_end - compressed.items[i].heap_start + 1;
        if (status_has_visible(compressed.items[i].status)) visible += n;
        if (status_has_frozen(compressed.items[i].status)) frozen += n;
        if (compressed.items[i].status == VM_STATUS_CORRUPT) corrupt_compressed++;
      }
      fprintf(out,
              "\n--------------------------------------------------------------"
              "\nSUMMARY\n--------------------------------------------------"
              "------------\n");
      fprintf(out, "heap pages in range: %ld\n",
              to - from + 1);
      fprintf(out, "all-visible: %ld  all-frozen: %ld  error page(s): %ld\n",
              visible, frozen, corrupt_compressed);
    }
    free(compressed.items);
  }

  fclose(out);
  free(pages);
  fprintf(stderr, "wrote %s\n", out_path);
  return 0;
}

static int do_vm_diff(const char *old_path, const char *new_path,
                   const char *out_path, const VmOptions *options) {
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


  long total_max = totalA > totalB ? totalA : totalB;
  long from = options->has_heap_range ? options->heap_from : 0;
  long to = options->has_heap_range ? options->heap_to : default_heap_to(total_max);

  fprintf(out,
          "=== pg_vm diff ===\nold: %s (%ld pages)\nnew: %s (%ld pages)\n"
          "heap page range covered: [%ld, %ld]\n\n",
          old_path, totalA, new_path, totalB, from, to);

  if (options->expand) {
    print_expanded_diff(out, A, totalA, B, totalB, from, to, options);
  } else {
    VmDiffRunVec compressed = compute_diff_compressed(A, totalA, B, totalB, from, to);
    print_diff_compressed(out, &compressed, options);
    if (options->stats) {
      long changed_pages = 0, gained_visible = 0, lost_visible = 0,
           gained_frozen = 0, lost_frozen = 0;
      for (long i = 0; i < compressed.count; i++) {
        const VmDiffRun *r = &compressed.items[i];
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
              to - from + 1, compressed.count);
      fprintf(out, "changed heap pages: %ld\n", changed_pages);
      fprintf(out, "+ all-visible: %ld  - all-visible: %ld\n",
              gained_visible, lost_visible);
      fprintf(out, "+ all-frozen: %ld  - all-frozen: %ld\n",
              gained_frozen, lost_frozen);
    }
    free(compressed.items);
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
          "  -H                  per-page_headerysical-page header inventory\n"
          "  -q                  add summary\n"
          "  --heap-range A-B    process heap pages [A,B]\n"
          "  --heap-page N       dump only: status of one heap "
          "page\n"
          "  --extra             one line per heap page \n"
          "  --only-not-visible  dump: only ranges missing ALL_VISIBLE\n"
          "  --only-not-frozen   dump: only ranges missing ALL_FROZEN\n"
          "  --only-changed      diff: only ranges whose status changed\n",
          prog, prog);
}

static void parse_vm_flags(int argc, char **argv, int start, VmOptions *options,
                        char **pos, int *npos) {
  *npos = 0;
  for (int i = start; i < argc; i++) {
    const char *a = argv[i];

    if (strcmp(a, "--heap-range") == 0 && i + 1 < argc) {
      long lo, hi;
      if (sscanf(argv[++i], "%ld-%ld", &lo, &hi) == 2) {
        options->has_heap_range = 1;
        options->heap_from = lo;
        options->heap_to = hi;
      } else {
        fprintf(stderr, "bad --heap-range value %s, expected A-B (ignored)\n",
                argv[i]);
      }
    } else if (strcmp(a, "--heap-page") == 0 && i + 1 < argc) {
      options->has_heap_page = 1;
      options->heap_page_query = atol(argv[++i]);
    } else if (strcmp(a, "--extra") == 0) {
      options->expand = 1;
    } else if (strcmp(a, "--only-not-visible") == 0) {
      options->only_not_visible = 1;
    } else if (strcmp(a, "--notV") == 0) {
      options->only_not_visible = 1;
    } else if (strcmp(a, "--only-not-frozen") == 0) {
      options->only_not_frozen = 1;
      } else if (strcmp(a, "--notF") == 0) {
      options->only_not_frozen = 1;
    } else if (strcmp(a, "--only-changed") == 0) {
      options->only_changed = 1;
    } else if (a[0] == '-' && a[1] != '\0' && a[1] != '-') {
      for (const char *c = a + 1; *c; c++) {
        switch (*c) {
          case 'H':
            options->show_headers = 1;
            break;
          case 'q':
            options->stats = 1;
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

  VmOptions options = {0};
  options.stats = 0;
  char *pos[8];
  int npos = 0;

  if (strcmp(argv[1], "dump") == 0) {
    parse_vm_flags(argc, argv, 2, &options, pos, &npos);
    if (npos != 2) {
      vm_usage(argv[0]);
      return 1;
    }
    return do_vm_dump(pos[0], pos[1], &options);
  } else if (strcmp(argv[1], "diff") == 0) {
    parse_vm_flags(argc, argv, 2, &options, pos, &npos);
    if (npos != 3) {
      vm_usage(argv[0]);
      return 1;
    }
    return do_vm_diff(pos[0], pos[1], pos[2], &options);
  }

  vm_usage(argv[0]);
  return 1;
}