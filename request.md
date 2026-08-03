# Add support for parsing `_fsm` and `_vm` forks

## Motivation

`pg_filedump` currently only parses the main fork of heap/index files; the `_vm` (Visibility Map) and `_fsm` (Free Space Map) forks have no human-readable dump support, so diagnosing them means reading raw bytes by hand.

In practice, this matters when master and replica versions of these files diverge (e.g. after VACUUM, crash recovery, or replication lag) — being able to dump and diff `_vm`/`_fsm` on both sides makes it possible to actually pinpoint what's inconsistent, rather than guessing from symptoms alone.

This also helps with debugging in general, since this capability didn't exist before, and it makes it possible to make PostgreSQL's core more perfomance.

1. The free space map can show how full pages are (on master and on replica).
2. Parsing the visibility map shows the visibility status of pages — helping explain why a particular query plan was chosen.

## What's added

New parsing modes for `_fsm` (tree traversal, per-page free-space category) and `_vm` (visible/frozen bits per heap page), plus a `diff` mode to compare two dumps of the same fork.

## Flags

### `pg_fsm`

| Flag                           | Purpose                                                                 |
| ------------------------------ | ----------------------------------------------------------------------- |
| `-H`                         | page headers, including`fp_next_slot`                                 |
| `-i`                         | internal tree nodes                                                     |
| `-s`                         | leaf slots + per-heap-page free-space category                          |
| `-a`                         | all of the above combined                                               |
| `-q`                         | summary: per-category (0–255) histogram of heap pages + stats          |
| `--range A-B` / `--page N` | page range                                                              |
| `--extra`                    | full per-slot/per-node listing instead of the default compressed ranges |
| `--heap-page N`              | locate a specific heap page                                             |
| `--min N` / `--max N`      | filter leaf slots by`avail_bytes` (dump `-s` only)                  |
| `--only-changed`             | diff mode: show only pages whose value actually changed                 |

### `pg_vm`

| Flag                                                              | Purpose                                                                          |
| ----------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| `-H`                                                            | page header dump                                                                 |
| `-q`                                                            | summary (counts of visible/frozen pages)                                         |
| `--heap-range A-B`                                              | a heap-page range                                                                |
| `--heap-page N`                                                 | look up a single heap page's VM bits                                             |
| `--expand`                                                      | full per-heap-page listing instead of run-length-compressed ranges               |
| `--only-not-visible` / `--only-not-frozen / --notF / --notV ` | dump mode: show only pages missing that bit                                      |
| `--only-changed`                                                | diff mode: show only pages where visible/frozen status differs between two dumps |

## Example usage

### `pg_fsm`

**`-H` — page headers**

```
$ pg_fsm dump -H /Users/dariakorsun/pgdata1/base/5/16399_fsm
=== pg_fsm dump: /Users/dariakorsun/pgdata1/base/5/16399_fsm ===
file size: 122880 bytes, total pages: 15 (8192 bytes/page)
flags: headers=on internal=off slots=off expand=off
\-- 0 fsm [ROOT]
      header: pd_lsn=0 pd_checksum=0 pd_flags=0x0 pd_lower=24 pd_upper=8192 pd_special=8192 pagesize=8192 layout_version=4 fp_next_slot=0
    \-- 1 fsm [INTERNAL]
          header: pd_lsn=0 pd_checksum=0 pd_flags=0x0 pd_lower=24 pd_upper=8192 pd_special=8192 pagesize=8192 layout_version=4 fp_next_slot=0
        \-- 2 fsm [LEAF]
              header: pd_lsn=0 pd_checksum=0 pd_flags=0x0 pd_lower=24 pd_upper=8192 pd_special=8192 pagesize=8192 layout_version=4 fp_next_slot=4069
              page max: category=0 (0 bytes) at slot -1
              Free space on page: 0 (sum over 4069 slots)
```

**`-i` — internal tree nodes**

```
\-- 14 fsm [LEAF]
              NONLEAF (4095 nodes):
                  [0-1]                 77
                  [2]                    0
                  [3]                   77
                  [4-6]                  0
                  [7]                   77
                  [8-15]                 0
                  [16]                  77
                  [17-33]                0
                  [34]                  77
                  [35-68]                0
                  [69]                  77
                  [70-139]               0
                  [140]                 77
                  [141-280]              0
                  [281]                 77
                  [282-562]              0
                  [563]                 77
                  [564-1126]             0
                  [1127]                77
                  [1128-2254]            0
                  [2255]                77
                  [2256-4094]            0
              LEAF (4069 nodes):
                  [0-415]                0
                  [416]                 77
                  [417-4068]             0
              page max: category=77 (2464 bytes) at slot 416
              Free space on page: 2464
```

**`-s` — leaf slots**

```
\-- 14 fsm [LEAF]
              page max: category=77 (2464 bytes) at slot 416
              Free space on page: 2464 

              leaf slots (4069, starting at 48828):
                heap page    48828-49243   : category=0   avail_bytes=0     (416 page(s))
                heap page    49244         : category=77  avail_bytes=2464 
                heap page    49245-52896   : category=0   avail_bytes=0     (3652 page(s))
```

**`-a` — combined flags -His**

**`-q` — summary**

```
--------------------------------------------------------------
SUMMARY
--------------------------------------------------------------
root pages: 1  internal pages: 1  leaf pages: 13
zero/hole pages: 1  invalid-header pages: 0

Max free-space category: 77 (2464 bytes), page 14 slot 416

Total avail_bytes : 2464

heap pages by category:
  category   0 (    0 bytes): 48827 page(s)
  category  77 ( 2464 bytes): 1 page(s)
```

**`--range A-B` / `--page N`- page in range**

**`--extra`**

```
\-- 14 fsm [LEAF]
      header: pd_lsn=0 pd_checksum=0 pd_flags=0x0 pd_lower=24 pd_upper=8192 pd_special=8192 pagesize=8192 layout_version=4 fp_next_slot=0
      NONLEAF (4095 node(s), expanded):
          [   0]  77  77   0  77   0   0   0  77   0   0   0   0   0   0   0   0  77   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0 
          [  32]   0   0  77   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0 
          [  64]   0   0   0   0   0  77   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0   0 
   .
   .
   .

      leaf slots (4069, one per heap page starting at 48828, expanded):
      	  slot    0 (heap page    48828): category=  0 avail_bytes=    0
      	  slot    1 (heap page    48829): category=  0 avail_bytes=    0
      	  slot    2 (heap page    48830): category=  0 avail_bytes=    0
      	  slot    3 (heap page    48831): category=  0 avail_bytes=    0
```

**`--heap-page N` - jnly one heap page**

**`--min N` / `--max N`**

```
 \-- 14 fsm [LEAF]
              header: pd_lsn=0 pd_checksum=0 pd_flags=0x0 pd_lower=24 pd_upper=8192 pd_special=8192 pagesize=8192 layout_version=4 fp_next_slot=0
              page max: category=77 (2464 bytes) at slot 416
              Free space on page: 2464 

              leaf slots (4069, starting at 48828) [filtered by avail_bytes]:
                heap page    49244         : category=77  avail_bytes=2464
```

**diff mode**

```
=== pg_fsm diff ===
old: /Users/dariakorsun/pgdata-dtrace/base/5/24576_2_fsm (3 pages)
new: /Users/dariakorsun/pgdata-dtrace/base/5/24576_fsm (3 pages)

\-- 0 fsm [ROOT] CHANGED
      internal-node    0-1       : 239 -> 218  [CHANGED] (2 node(s))
      internal-node    2         :   0 ->   0  [same]
      internal-node    3         : 239 -> 218  [CHANGED]
      internal-node    4-6       :   0 ->   0  [same] (3 node(s))
      internal-node    7
```

**`--only-changed` (diff mode)**

```
=== pg_fsm diff ===
old: /Users/dariakorsun/pgdata-dtrace/base/5/24576_2_fsm (3 pages)
new: /Users/dariakorsun/pgdata-dtrace/base/5/24576_fsm (3 pages)
(--only-changed: SAME/empty subtrees and unchanged runs are hidden)

\-- 0 fsm [ROOT] CHANGED
      leaf-slot heap page        0           : 239 -> 218 ( 7648 ->  6976 B)
    \-- 1 fsm [INTERNAL] CHANGED
          leaf-slot heap page        0           : 239 -> 218 ( 7648 ->  6976 B)
        \-- 2 fsm [LEAF] CHANGED
              leaf-slot heap page        0           : 239 -> 218 ( 7648 ->  6976 B)
```

### `pg_vm`

**`-H` — page header dump**

```
=== pg_vm dump: /Users/dariakorsun/pgdata-dtrace/base/5/24579_vm ===
file size: 8192 bytes, total pages: 1, 8192 bytes/page
heap page range covered: [0, 32671]


-- physical page inventory (-H) --
vm page 0: lsn=15F181E78 checksum=0 flags=0x0 lower=24 upper=8192 special=8192

-- heap page status (compressed) --
heap pages        0-862     : visible+frozen (863 page(s))
heap pages      863-32671   : none (31809 page(s))
```

**`-q` — summary**

```
--------------------------------------------------------------
SUMMARY
--------------------------------------------------------------
heap pages in range: 32672
all-visible: 863  all-frozen: 863  error page(s): 0
```

**`--heap-range A-B`**

**`--heap-page N`**

**`--expand`**

```
=== pg_vm dump: /Users/dariakorsun/pgdata-dtrace/base/5/24579_vm ===
file size: 8192 bytes, total pages: 1, 8192 bytes/page
heap page range covered: [0, 32671]


-- physical page inventory (-H) --
vm page 0: lsn=15F181E78 checksum=0 flags=0x0 lower=24 upper=8192 special=8192

-- heap page status (expanded) --
heap page        0: visible+frozen
heap page        1: visible+frozen
```

**`--only-not-visible` / (`--only-not-frozen)`**

```
=== pg_vm dump: /Users/dariakorsun/pgdata1/base/5/16399_vm ===
file size: 16384 bytes, total pages: 2, 8192 bytes/page
heap page range covered: [0, 65343]


-- page_headerysical page headers (-H) --
vm page 0: lsn=70304A50 checksum=0 flags=0x0 lower=24 upper=8192 special=8192
vm page 1: lsn=7043FA08 checksum=0 flags=0x0 lower=24 upper=8192 special=8192

-- heap page status (compressed) --
heap pages        0-49244   : visible (49245 page(s))
heap pages    49245-65343   : none (16099 page(s))
```

**(diff mode)**

```
=== pg_vm diff ===
old: /Users/dariakorsun/pgdata-dtrace/base/5/24579_2_vm (1 pages)
new: /Users/dariakorsun/pgdata-dtrace/base/5/24579_vm (1 pages)
heap page range covered: [0, 32671]

heap pages        0-862     : none -> visible+frozen  [CHANGED] (863 page(s))
heap pages      863-32671   : none -> none  [same] (31809 page(s))
```

**`--only-changed` (diff mode)**

```
=== pg_vm diff ===
old: /Users/dariakorsun/pgdata-dtrace/base/5/24579_2_vm (1 pages)
new: /Users/dariakorsun/pgdata-dtrace/base/5/24579_vm (1 pages)
heap page range covered: [0, 32671]

heap pages        0-862     : none -> visible+frozen  [CHANGED] (863 page(s))
```
