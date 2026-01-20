---
title: StarRocks Architecture and Concepts
---

## 1. Overview

StarRocks is a distributed analytical database utilizing a Java-based Frontend (FE) and a C++-based Backend (BE). This decoupled dual-language architecture allows StarRocks to leverage strengths from both languages, enabling it to deliver high performance while maintaining developer productivity and ease of integration.

## 2. Architecture

There are two main types of StarRocks servers: **Frontends (FE)** and **Backends (BE)**. In the disaggregated storage-compute architecture, **Compute Nodes (CN)** replace BEs for stateless computation.

### 2.1. Cluster

A StarRocks cluster consists of one or more FE nodes and one or more BE/CN nodes. Users connect to FE nodes using MySQL-compatible clients. The FE nodes coordinate query planning and scheduling, while BE/CN nodes execute queries and store data (in the case of BEs).

Processing each query is a distributed operation. The workload is orchestrated by the FE nodes and distributed in parallel across all BE/CN nodes in the cluster. Each node runs StarRocks in a single process (JVM for FE, native process for BE/CN), and processing is further parallelized using threads and vectorized execution.

### 2.2. Node

Any StarRocks server in a specific StarRocks cluster is considered a **node** of the cluster. Technically, this refers to the process running the StarRocks program (Java process for FE, C++ process for BE/CN), but "node" is often used to refer to the computer running the process, as it's recommended to run only one StarRocks process per machine.

### 2.3. Frontend (FE)

The Frontend is responsible for:

- **Metadata Management**: Managing database schemas, table definitions, partition/tablet information, and user privileges
- **Client Interface**: Handling client connections via MySQL protocol (port 9030), or HTTP/ArrowFlight protocols
- **Query Planning**: Parsing SQL statements, validating semantics, and generating physical execution plans with rules-based and cost-based optimizers
- **Query Scheduling**: Deploying query fragments to backend nodes and managing distributed execution across the cluster

Every StarRocks installation must have at least one FE node. For high availability, multiple FE nodes are recommended. FE nodes use BDB JE (Berkeley DB Java Edition) which implements a Paxos-compliant consensus algorithm.

### 2.4. Backend (BE)

The Backend is responsible for:

- **Data Storage**: Storing data locally in columnar format, transforming ingested data into the required format, and generating indexes to accelerate queries
- **Query Execution**: Executing physical query plans received from FE nodes using a fully vectorized pipeline execution engine

BE nodes store data locally and execute queries on the data where it resides, minimizing data transmission and achieving high query performance. Data is sharded into **Tablets** (logical units) that are distributed and replicated across BE nodes to ensure fault tolerance and load balancing.

### 2.5. Compute Node (CN)

In the shared-data architecture, the BE role is adapted into a stateless Compute Node (CN). CNs do not manage persistent storage but execute query fragments and cache hot data from remote object storage (S3, GCS, HDFS, etc.). This architecture allows for elastic scaling of compute resources independently from storage.

## 3. Architecture Models

StarRocks supports two deployment architectures:

### 3.1. Shared-Nothing (Coupled Storage-Compute)

In this traditional MPP architecture, each BE node manages its own local storage.

- **Data Locality**: Query fragments are scheduled on nodes where data resides, minimizing network overhead
- **Replication**: Data availability is guaranteed through multi-replica mechanisms (typically 3 replicas)
- **Performance**: Provides optimal query latency for real-time queries

### 3.2. Shared-Data (Disaggregated Storage-Compute)

This architecture decouples storage from compute, introduced in StarRocks 3.0.

- **Global Storage**: All persistent data resides in remote object storage (S3, GCS, HDFS, etc.)
- **Stateless Compute**: CNs are stateless and can scale elastically
- **Local Caching**: CNs utilize local disk and memory to cache hot data segments to accelerate query performance

## 4. Data Organization

### 4.1. Table

A table is a collection of data organized into rows and columns. StarRocks supports four table types.

#### 4.1.1. Primary Key Table

Supports updates and deletes with primary key indexing. Best performance for point queries and updates.

#### 4.1.2. Duplicate Key Table

Stores all data rows without deduplication. Suitable for detailed data analysis.

#### 4.1.3. Aggregate Table

Pre-aggregates data during ingestion using aggregate functions (SUM, COUNT, MAX, etc.).

#### 4.1.4. Unique Key Table

Deduplicates data based on unique keys, keeping only the most recent record.

### 4.2. Partition

A **partition** is a logical division of a table's data, typically based on time (e.g., daily or monthly partitions). Partitions enable partition pruning, easy data management (dropping old partitions), and parallel processing.

StarRocks supports range partitioning, list partitioning, and expression partitioning. Partitions are further divided into tablets via bucketing for physical distribution across BE nodes.

### 4.3. Bucketing

**Bucketing** (also called "distributed by") divides each partition into multiple **buckets**, which become **tablets**—the physical distribution units. StarRocks supports two bucketing strategies:

#### 4.3.1. Hash Bucketing

Data is distributed based on a hash function applied to one or more bucketing columns. Enables bucket pruning and colocate joins. Requires high-cardinality columns to avoid data skew.

#### 4.3.2. Random Bucketing

Data is randomly distributed using round-robin. Naturally avoids data skew but doesn't support bucket pruning or colocate joins. Supported only in Duplicate Key tables.

Bucket count can be manually specified or auto-determined. Typical bucket size: 1-10 GB.

### 4.4. Tablet

A **tablet** is the fundamental unit of physical data distribution and replication in StarRocks. Each tablet:

- Is created by bucketing (each bucket becomes a tablet)
- Contains a subset of a partition's data
- Is replicated across multiple BE nodes (typically 3 replicas for high availability)
- Is the unit of parallel query execution

Tablets are distributed across BE nodes to ensure load balancing and fault tolerance. The FE schedules query fragments to execute on BE nodes containing the relevant tablets.

### 4.5. Rowset

A **rowset** is an immutable collection of data segments representing a batch of writes to a tablet. Rowsets are created during data ingestion and merged through compaction. Each rowset is versioned and can be overlapped (for updates) or non-overlapped (for append-only data).

### 4.6. Segment

A **segment** is a self-contained columnar file (typically ~512MB) that stores a portion of a rowset's data. Segments contain column data, indexes, and metadata, optimized for columnar reads and vectorized execution.

### 4.7. Sort Key

A **sort key** determines how data is sorted and stored on disk within segments. It enables query acceleration, generates prefix indexes, and improves compression. Sort key requirements vary by table type:

- **Duplicate Key Table**: Any combination of columns (via `ORDER BY`)
- **Aggregate Table**: Must contain aggregate key columns
- **Unique Key Table**: Must contain unique key columns
- **Primary Key Table**: Can differ from primary key (via `ORDER BY`)

Best practices: place frequently filtered columns first, prefer high-cardinality columns, and include timestamps for time-series data.

### 4.8. Data Block

A **data block** is a logical unit containing 1024 rows of data. It is the granularity for prefix index entries and zone-map statistics, enabling StarRocks to skip large portions of data during query execution.

### 4.9. Built-in Indexes

StarRocks provides several built-in indexes automatically created and maintained:

#### 4.9.1. Prefix Index

Sparse index on sort key columns (one entry per 1024-row data block). Automatically generated from the sort key during data writing. Stored in memory (typically 1024x smaller than data). Enables fast point and range queries via binary search. Limited to 36 bytes; only prefix columns within the limit are indexed.

#### 4.9.2. Ordinal Index

Maps row ordinals to physical page offsets. Each column data page (64KB) has an ordinal index entry. Used by other indexes to locate physical data.

#### 4.9.3. Zone-map Index

Stores min/max values and null statistics for each data page. Enables efficient data pruning at the page level.

These indexes work together: the prefix index locates relevant data blocks, the zone-map index prunes pages within blocks, and the ordinal index provides physical addresses for data access.

### 4.10. Manually Created Indexes

When query conditions involve columns that are not part of the prefix index, users can manually create indexes to improve query efficiency. StarRocks supports several types of manually created indexes:

#### 4.10.1. Bitmap Index

A **bitmap index** is suitable for queries on high cardinality columns or combinations of multiple low cardinality columns. It can exhibit ideal filtering performance, filtering out at least 999 out of 1000 rows. Bitmap indexes are particularly effective for equality predicates and can significantly reduce the amount of data scanned during query execution.

#### 4.10.2. Bloom Filter Index

A **bloom filter index** is suitable for columns with relatively high cardinality, such as ID columns. It uses probabilistic data structures to quickly determine whether a value might exist in a data block, enabling efficient data pruning. However, bloom filters may incur a certain rate of false positives (misjudgment), meaning they may indicate a value exists when it does not, but will never indicate a value does not exist when it actually does.

#### 4.10.3. N-Gram Bloom Filter Index

An **n-gram bloom filter index** is a special type of Bloom filter index, typically used to speed up `LIKE` queries or the operations of the `ngram_search` and `ngram_search_case_insensitive` functions. It breaks text into n-gram sequences and indexes them, making it efficient for pattern matching queries.

#### 4.10.4. Full-Text Inverted Index

A **full-text inverted index** can quickly locate data rows that match keywords, thereby speeding up full-text searches. It indexes the words or terms in text columns, enabling fast keyword-based queries and text search operations.

#### 4.10.5. Vector Index

A **vector index** allows you to perform an approximate nearest neighbor search (ANNS) with StarRocks. It is designed for similarity search on vector data, enabling efficient queries to find vectors that are most similar to a given query vector. This is particularly useful for machine learning applications, recommendation systems, and semantic search.

Unlike built-in indexes that are automatically created and maintained, manually created indexes require explicit creation via SQL statements and should be chosen based on the characteristics of the data and the query patterns.

## 5. Query Execution Model

StarRocks executes SQL statements and converts them into distributed queries that run across the cluster. A **statement** is the textual SQL representation, while a **query** is the instantiated execution plan with logical/physical plans, plan fragments, and coordination across BE/CN nodes.

### 5.1. Plan Fragment

A **plan fragment** is a portion of a distributed query plan executed independently on a single BE/CN node. Fragments form a tree: root fragments aggregate final results, intermediate fragments process and exchange data, and leaf fragments scan data from tablets. Fragments are connected via **Exchange Nodes** for data shuffling. The FE schedules fragments to appropriate nodes based on data locality and load.

### 5.2. Pipeline

A **pipeline** is the execution model within a fragment. StarRocks uses a fully vectorized pipeline execution engine where operators process data in streaming fashion, data is processed in chunks (columns) rather than row-by-row, and multiple pipelines execute in parallel within a fragment.

### 5.3. Operator

An **operator** is a processing unit within a pipeline that consumes, transforms, and produces data. Examples include Scan (reads from tablets), Filter (applies predicates), Aggregation (SUM, COUNT, etc.), Join, and Exchange (shuffles data between fragments). Operators are vectorized, processing data in chunks (typically 4096 rows) to maximize CPU efficiency and leverage SIMD instructions.

### 5.4. Driver

A **driver** is a sequence of operator instances executing together in a pipeline, representing the lowest level of parallelism. Drivers process data in chunks (vectorized) and run in a thread pool for parallel execution. Multiple drivers execute in parallel within a fragment, each processing different data partitions.

### 5.5. Chunk

A **chunk** is a columnar data structure containing a collection of column vectors and schema metadata, representing a batch of rows (typically up to 4096 rows, configurable via `chunk_size`). Chunks are the fundamental unit of data transfer in the vectorized execution engine, flowing between operators via `push_chunk()` and `pull_chunk()` methods. This columnar, batch-oriented processing enables vectorized operations on entire columns at once, maximizing CPU cache efficiency and leveraging SIMD instructions for high performance.

## 6. Query Optimization

StarRocks employs sophisticated query optimization techniques to achieve high performance.

### 6.1. Cost-Based Optimizer (CBO)

The **Cost-Based Optimizer** is a Cascades-style optimizer that generates multiple execution plans, estimates costs based on statistics (row counts, column cardinality, data distribution), and selects the optimal plan. The CBO supports multi-stage join reordering, predicate/projection pushdown, subquery transformation, and runtime filter propagation.

### 6.2. Runtime Filter

A **runtime filter** is a dynamic filter created during query execution to reduce data transfer in distributed joins. Types include Bloom Filter (membership testing), Min/Max Filter (range filters), and In Filter (exact value matching). Runtime filters are built on the smaller (build) side of a hash join and broadcast to the larger (probe) side to filter out non-matching rows early, reducing network shuffle overhead.

### 6.3. Materialized View

A **materialized view** is a pre-computed result set stored as a physical table. StarRocks supports synchronous materialized views (rollups, automatically updated during ingestion, single-table aggregations) and asynchronous materialized views (scheduled updates, multi-table joins, complex aggregations, external catalogs). The CBO automatically rewrites queries to use materialized views when mathematically equivalent, transparently accelerating performance.

## 7. Data Ingestion

StarRocks supports multiple data ingestion methods:

### 7.1. Stream Load

Synchronous loading via HTTP POST, returns immediately upon completion, supports CSV/JSON formats, suitable for real-time or small-batch loading.

### 7.2. Broker Load

Asynchronous loading from external storage (HDFS, S3, etc.), runs in background, supports large-scale batch loading.

### 7.3. Routine Load

Continuous loading from message queues (Kafka, Pulsar, etc.), automatically handles offsets and failures, provides near-real-time ingestion with exactly-once semantics.

## 8. Replication and High Availability

StarRocks ensures data reliability and service availability through replication. A **replica** is a copy of a tablet stored on a BE node. StarRocks typically maintains 3 replicas of each tablet for high availability (service continues if one BE node fails), load balancing (queries routed to any healthy replica), and consistency (replicas kept in sync through a consensus protocol). Replicas can be in states: NORMAL (healthy), DECOMMISSION (being removed), CLONE (being created/repaired), or BAD (unhealthy, needs repair). The FE monitors replica health and triggers repair operations when needed.

## 9. Metadata

StarRocks maintains metadata to manage the cluster and enable query planning.

### 9.1. Catalog

A **catalog** is a collection of databases and tables that provides a unified interface for accessing data. StarRocks supports two types:

#### 9.1.1. Internal Catalog (default_catalog)

Manages data stored within StarRocks. Each cluster has exactly one internal catalog. Data is stored and managed by StarRocks, supports all features (updates, materialized views, etc.), and provides full control over data layout, partitioning, and indexing.

#### 9.1.2. External Catalog

Links to externally managed metastores, granting direct access to external data sources without data loading. Supports Hive, Iceberg, Hudi, Delta Lake, JDBC, Elasticsearch (v3.1+), Paimon (v3.1+), and Unified Catalog (v3.2+). FEs access metadata from external metastores (Hive Metastore, AWS Glue, etc.) to generate query plans, while BEs/CNs scan data files directly from HDFS or object storage (S3, GCS, etc.) in parallel. Enables zero data migration, unified SQL query interface, and cross-catalog queries using `catalog_name.database_name.table_name` format.

### 9.2. Schema

A **schema** (also called a database) is a namespace that contains tables, views, and other objects, providing logical organization of related tables.

### 9.3. Metadata Storage

FE nodes store metadata in BDB JE (Berkeley DB Java Edition): kept in memory for fast access, persisted to disk for durability, replicated across FE nodes for high availability, with a Raft-like protocol ensuring consistency.
