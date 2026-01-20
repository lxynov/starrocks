---
title: StarRocks Storage Engine
---

## 1. Columnar Storage: The Foundation of Analytical Performance
**Topics:**
- Why columnar storage for analytics
- How StarRocks organizes data in columns
- Column encoding techniques (Dictionary, RLE, Delta)
- Compression strategies
- Benefits: compression ratio, I/O reduction, cache efficiency

**Key Files to Reference:**
- `be/src/storage/rowset/column_writer.h`
- `be/src/storage/rowset/column_reader.h`
- `be/src/column/` - Column data structures
- `docs/en/best_practices/table_clustering.md`

## 2. Storage Hierarchy: Table → Partition → Tablet → Rowset → Segment
**Topics:**
- Multi-level storage organization
- Table and partition concepts
- Tablet: the unit of data distribution and replication
- Rowset: immutable data bundles
- Segment: self-contained columnar files (~512MB)
- How data is distributed across BEs

**Key Files to Reference:**
- `be/src/storage/tablet.cpp`
- `be/src/storage/rowset/`
- `be/src/storage/olap_table.cpp`
- `docs/en/best_practices/table_clustering.md`

## 3. Segment Structure: Understanding the On-Disk Format
**Topics:**
- Segment file layout: data pages, indexes, footer
- Column data pages (64KB blocks)
- Ordinal index: row ordinal → page offset mapping
- Zone-map index: min/max per page for pruning
- Short-key (prefix) index: sparse index for point/range seeks
- Footer and magic number for integrity

**Key Files to Reference:**
- `be/src/storage/rowset/segment_iterator.cpp`
- `be/src/storage/rowset/segment.h`
- `docs/en/best_practices/table_clustering.md`

## 4. Write Path: How Data Gets Into StarRocks
**Topics:**
- Data ingestion overview: Stream Load, Broker Load, Routine Load
- MemTable: in-memory buffer for writes
- Flush: MemTable → Segment
- Compaction: merging segments for query performance
- ACID guarantees and transaction handling
- Write-optimized vs read-optimized storage

**Key Files to Reference:**
- `be/src/runtime/stream_load/`
- `be/src/storage/memtable.h`
- `be/src/storage/compaction/`
- `fe/fe-core/src/main/java/com/starrocks/load/`

## 5. Read Path: How Queries Access Data Efficiently
**Topics:**
- Partition pruning at planner time
- Tablet selection and routing
- Segment-level pruning using zone-maps
- Page-level access using ordinal index
- Short-key index for point queries
- Column projection: reading only needed columns

**Key Files to Reference:**
- `be/src/exec/olap_scan_node.cpp`
- `be/src/storage/rowset/segment_iterator.cpp`
- `be/src/storage/tablet_reader.cpp`

## 6. Primary Key Tables and Upsert Support
**Topics:**
- Primary key encoding and indexing
- Delete-and-insert implementation for upserts
- Persistent index for primary key lookups
- How updates are handled efficiently
- Comparison with append-only tables

**Key Files to Reference:**
- `be/src/storage/primary_key_encoder.h`
- `be/src/storage/persistent_index/`
- `be/src/storage/kv_store.cpp`
