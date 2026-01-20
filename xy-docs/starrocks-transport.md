---
title: StarRocks Transport Mechanisms
---

## 1. Overview

StarRocks uses multiple transport mechanisms optimized for different use cases to balance between usability and performance.

## 2. Client-Server Transport

### 2.1. MySQL Protocol

StarRocks implements the MySQL wire protocol to provide compatibility with standard MySQL clients, JDBC drivers, and database tools. It serves as the primary interface for SQL query execution and administrative operations.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         MySQL Client (JDBC/CLI)                         │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │ TCP Connection (Port 9030)
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                          MysqlServer                                    │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  XNIO Worker Threads (I/O Thread Pool)                            │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  AcceptListener                                             │  │  │
│  │  │  - Accepts new connections                                  │  │  │
│  │  │  - Creates ConnectContext                                   │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │
                          │ Per Connection
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        MysqlChannel                                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  - Low-level packet I/O (read/write)                              │  │
│  │  - Packet fragmentation/reassembly (>16MB)                        │  │
│  │  - Sequence ID management                                         │  │
│  │  - SSL/TLS support (optional)                                     │  │
│  └────────────────────────────┬──────────────────────────────────────┘  │
└───────────────────────────────┼─────────────────────────────────────────┘
                                │
                                │ MySQL Protocol Packets
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        Connection Lifecycle                             │
│                                                                         │
│  1. Handshake Phase:                                                    │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  MysqlProto.negotiate()                                        │  │
│     │  ├─> Send: MysqlHandshakePacket                                │  │
│     │  │   (protocol version, server capabilities, auth salt)        │  │
│     │  ├─> Recv: MysqlAuthPacket                                     │  │
│     │  │   (username, password hash, database, capabilities)         │  │
│     │  └─> Validate: Authentication                                  │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  2. Query Processing Phase:                                             │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  MySQLReadListener (Async I/O)                                 │  │
│     │  ├─> Reads raw bytes from channel                              │  │
│     │  ├─> MysqlPackageDecoder decodes packets                       │  │
│     │  │   (header parsing, sequence validation, fragmentation)      │  │
│     │  └─> Queues RequestPackage for processing                      │  │
│     └───────────────────────┬────────────────────────────────────────┘  │
│                             │                                           │
│                             ▼                                           │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  ConnectProcessor (Worker Thread Pool)                         │  │
│     │  ├─> Parses MySQL command (COM_QUERY, COM_STMT_EXECUTE, etc)   │  │
│     │  ├─> Extracts SQL string from packet                           │  │
│     │  ├─> Delegates to StmtExecutor for execution                   │  │
│     │  └─> Formats response (MysqlOkPacket, MysqlErrPacket, etc)     │  │
│     └───────────────────────┬────────────────────────────────────────┘  │
│                             │                                           │
│                             ▼                                           │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  Result Serialization                                          │  │
│     │  ├─> MysqlSerializer converts data to MySQL wire format        │  │
│     │  ├─> MysqlCodec encodes types (integers, strings, etc)         │  │
│     │  └─> MysqlChannel sends packets back to client                 │  │
│     └────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: FE nodes listen on port `9030` (`query_port`) for MySQL protocol connections.
- **TCP Server**: XNIO is used to implement the TCP server.
    - **Accept Listener**: `AcceptListener` handles connection establishment:
        - Listens on `AcceptingChannel<StreamConnection>`
        - Accepts the TCP connection, creates `ConnectContext`
        - Performs MySQL handshake and authentication
        - Calls `context.startAcceptQuery()` which installs `MySQLReadListener`
    - **Read Listener**: `MySQLReadListener` handles ongoing query processing:
        - Listens on `ConduitStreamSourceChannel`
        - Uses `ConnectProcessor` to process requests
    - **Sink Channel**: inlined in `MysqlChannel`.
- `ConnectContext` is created per connection. It stores session variables, current database, user identity, and query state.
- `ConnectProcessor` has the entry point for executing queries. See `ConnectProcessor::executeQueryAttempt`.

### 2.2. HTTP SQL API

StarRocks provides an HTTP SQL API for executing queries via RESTful HTTP requests. This protocol enables programmatic access to StarRocks without requiring MySQL client libraries, making it suitable for integration with web applications, scripting tools, and HTTP-based data pipelines.

- **Port Configuration**: FE nodes listen on port `8030` (`http_port`) for HTTP SQL API requests.
- **Endpoint**: `/api/v1/catalogs/{catalog_name}/databases/{database_name}/sql` or `/api/v1/catalogs/{catalog_name}/sql` for cross-database queries.
- **Authentication**: Basic authentication using username and password in HTTP headers (`Authorization: Basic <credentials>`).
- **Request Format**: POST requests with JSON body containing:
    - `query`: SQL statement string (required)
    - `sessionVariables`: Optional map of session variable key-value pairs
- **Response Format**: Newline-delimited JSON (NDJSON) stream containing:
    - Connection metadata (`connectionId`)
    - Result schema (`meta` with column names and types)
    - Data rows (`data` arrays)
    - Query statistics (`statistics` with scan rows, scan bytes, return rows)
- **Supported Statements**: SELECT, SHOW, EXPLAIN, DESC, and KILL. Multi-statement execution is not supported.
- **Implementation**: `ExecuteSqlAction` handles HTTP requests, validates JSON payloads, and creates `HttpConnectContext` for query processing. `HttpConnectProcessor` executes queries similar to MySQL protocol but serializes results as JSON instead of MySQL wire format. Results are streamed incrementally as NDJSON, enabling low-latency delivery for large result sets.
- **Use Cases**: HTTP SQL API is ideal for RESTful integrations, web applications, and scenarios where MySQL protocol is not suitable due to firewall restrictions or client library limitations.

### 2.3. Arrow Flight Protocol

StarRocks supports Apache Arrow Flight SQL protocol for high-performance, zero-copy columnar data transfer. This protocol eliminates serialization overhead by maintaining columnar data format from StarRocks' internal execution engine through network transmission to client applications, providing optimal performance for data science workflows, analytics platforms, and machine learning pipelines.

- **Port Configuration**: BE nodes listen on port specified by `arrow_flight_port` (default `-1` disables the service). The port must be explicitly configured to enable Arrow Flight SQL support.
- **Protocol**: Apache Arrow Flight SQL, built on gRPC and Apache Arrow columnar format.
- **Query Execution Flow**: 
    - Client sends SQL query to FE's Arrow Flight SQL service (`ArrowFlightSqlServiceImpl`)
    - FE parses, plans, and deploys query to BE nodes
    - FE returns `FlightInfo` containing BE endpoint location and query ticket
    - Client connects directly to BE node using the provided endpoint and ticket
    - BE streams results in Arrow columnar format directly to client
- **Authentication**: Authentication occurs at FE during query submission. BE validates query tickets embedded in Flight requests, ensuring only authorized clients can retrieve query results.
- **Prepared Statements**: Arrow Flight SQL supports prepared statements for query optimization and parameter binding. FE's `ArrowFlightSqlServiceImpl` manages prepared statement lifecycle, caching prepared statement IDs per connection.
- **Session Management**: `ArrowFlightSqlSessionManager` maintains session state per connection, enabling session variable management and connection lifecycle tracking. Sessions can be closed via `closeSession` RPC.
- **Result Format**: Results are transmitted as Arrow columnar batches (`VectorSchemaRoot`), preserving vectorization benefits and enabling zero-copy data access in client applications. This eliminates row-column conversions required by traditional JDBC/ODBC protocols.
- **Use Cases**: Arrow Flight SQL is optimized for:
    - Data science workflows using Pandas, Apache Arrow, or similar columnar data frameworks
    - High-throughput data extraction for analytics and reporting
    - Machine learning pipelines requiring fast data iteration
    - Real-time analytics platforms with low-latency requirements

## 3. Frontend-Backend Transport

Frontend nodes communicate with Backend nodes for query execution, cluster management, and data retrieval.

### 3.1. bRPC (with Thrift Attachment) for Query Execution
[bRPC](https://github.com/apache/brpc) serves as the primary RPC framework for the query execution aspect in FE-BE communication, 
handling query coordination and fragment execution operations such as `exec_plan_fragment`, `fetch_data`, and `cancel_plan_fragment`. 
It uses Thrift attachments for data payload -- messages such as `TExecPlanFragmentParams` are serialized using Thrift and 
attached to bRPC requests as attachments via `ThriftClientAttachmentHandler`.

**Implementation Diagram:**

```text
┌────────────────────────────────────────────────────────────────────────────────┐
│                          Frontend (FE)                                         │
│  ┌──────────────────────────────────────────────────────────────────────────┐  │
│  │  BrpcProxy (Connection Pool Manager)                                     │  │
│  │  ┌────────────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Stub caching                                                    │  │  │
│  │  │  - Connection pooling per BE endpoint                              │  │  │
│  │  │  - Async RPC via PBackendService                                   │  │  │
│  │  │  - jprotobuf for calling bRPC in Java-based FE                     │  │  │
│  │  │  - Serializes Thrift data (TExecPlanFragmentParams, etc.)          │  │  │
│  │  │  - Attaches Thrift payload to bRPC request                         │  │  │
│  │  └───────────────────┬────────────────────────────────────────────────┘  │  │
│  └──────────────────────┼───────────────────────────────────────────────────┘  │
└─────────────────────────┼──────────────────────────────────────────────────────┘
                          │ BRPC (Port 8060)
                          │ - Protobuf RPC envelope
                          │ - Thrift attachment (TExecPlanFragmentParams, etc.)
                          ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                        Backend (BE)                                             │
│  ┌───────────────────────────────────────────────────────────────────────────┐  │
│  │  PInternalService (generated protobuf interface)                          │  │
│  │     ↑ inherits                                                            │  │
│  │  PInternalServiceImplBase<PInternalService> (implements all RPC methods)  │  │
│  │     ↑ inherits                                                            │  │
│  │  BackendInternalServiceImpl<PInternalService> (adds tablet writer methods)│  │
│  │     - Extracts Thrift attachments from bRPC requests                      │  │
│  │     - Deserializes Thrift data (TExecPlanFragmentParams, etc.)            │  │
│  └──────────────────────┼────────────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                    Query Execution Pipeline                                     │
│  - Fragment execution                                                           │
│  - Result buffering                                                             │
│  - Profile collection                                                           │
│  - etc.                                                                         │
└─────────────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: BE nodes listen on port `8060` (configurable via `brpc_port`) for bRPC services.
- **Service Definition**: bRPC service is defined in [gensrc/proto/internal_service.proto](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/proto/internal_service.proto). Message payload format is defined in [gensrc/thrift/InternalService.thrift](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/thrift/InternalService.thrift). Some important RPC methods:
    - `exec_plan_fragment`: Deploys a plan fragment to a BE node for execution. The fragment parameters are serialized as a Thrift `TExecPlanFragmentParams` structure and attached to the bRPC request as an attachment. The bRPC framework uses Protobuf for the RPC method signature, while the actual fragment data is transmitted as a Thrift-serialized attachment. This is the primary mechanism for query fragment deployment.
    - `fetch_data`: Pulls query results from BE nodes back to FE for client delivery in pull-based query execution. FE's `ResultReceiver` repeatedly calls `fetch_data` until end-of-stream. BE maintains result buffers per query (`ResultBufferMgr`/`BufferControlBlock`), and query execution operators write results to these buffers as they are produced. Results are serialized as `TResultBatch` (Thrift) or JSON (for HTTP queries) and attached to bRPC responses using the Thrift attachment mechanism. Each buffer maintains sequence numbers for ordered delivery and supports 24-hour timeout for long-running queries. Buffer size is bounded to prevent memory exhaustion, and query execution may block when buffers are full, providing natural backpressure mechanisms. The pull-based model allows FE to control result consumption rate, preventing memory exhaustion from rate mismatches between result production and consumption.
    - `collect_query_statistics`: Collects query execution statistics (CPU cost, scan bytes/rows, memory usage, spill bytes) from BE nodes for query monitoring and optimization.
- **Hybrid Serialization Pattern**: Methods that transmit complex data structures (such as `exec_plan_fragment` with `TExecPlanFragmentParams`) use a hybrid approach: the RPC method signature is defined in Protobuf, but the actual data payload is serialized as Thrift and attached to the bRPC request. Similarly, response data (such as `TResultBatch` in `fetch_data`) is also transmitted as Thrift attachments to bRPC responses. Note that profile reporting is handled via `report_exec_status` (Thrift RPC, BE-initiated) rather than bRPC, and is documented in Section 6.1.

### 3.2. Thrift RPC for Administrative Operations
Thrift RPC serves as a primary mechanism for administrative operations, data loading, and management tasks between FE and BE nodes. 
It handles a wide range of operations including agent tasks for tablet lifecycle management, data loading operations (routine load, stream load), 
export operations, ETL tasks, tablet statistics collection, and external table scanning. While bRPC is used for query execution, 
Thrift RPC remains as the protocol for these administrative and data loading workflows.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                          Frontend (FE)                                  │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Operation Initiators (Multiple Components)                       │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  AgentBatchTask                                             │  │  │
│  │  │  ├─> Batches agent tasks per BE node                        │  │  │
│  │  │  └─> Converts AgentTask → TAgentTaskRequest                 │  │  │
│  │  │                                                             │  │  │
│  │  │  RoutineLoadTaskScheduler                                   │  │  │
│  │  │  ├─> Creates routine load tasks (Kafka, Pulsar)             │  │  │
│  │  │  └─> Converts to TRoutineLoadTask                           │  │  │
│  │  │                                                             │  │  │
│  │  │  ExportMgr                                                  │  │  │
│  │  │  ├─> Creates export tasks                                   │  │  │
│  │  │  └─> Converts to TExportTaskRequest                         │  │  │
│  │  │                                                             │  │  │
│  │  │  TabletStatMgr                                              │  │  │
│  │  │  └─> Periodically requests tablet statistics                │  │  │
│  │  │                                                             │  │  │
│  │  │  Other Components (ETL, External Scanning, etc.)            │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  ThriftRPCRequestExecutor (Shared).                               │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  ThriftConnectionPool.backendPool                           │  │  │
│  │  │  - Connection pooling per BE endpoint                       │  │  │
│  │  │  - Connection validation and retry logic                    │  │  │
│  │  │  - Shared across all operation types                        │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ Thrift RPC (Port 9060)
                          │ - Thrift binary protocol
                          │ - Various request types:
                          │   • TAgentTaskRequest[] (agent tasks)
                          │   • TRoutineLoadTask[] (routine load)
                          │   • TExportTaskRequest (export)
                          │   • TTabletStatRequest (statistics)
                          │   • TScanOpenParams (external scan)
                          │   • Other operation-specific types
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        Backend (BE)                                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  BackendService (Thrift Service Interface)                        │  │
│  │     ↑ implements                                                  │  │
│  │  BackendServiceBase (base implementation)                         │  │
│  │     ↑ inherits                                                    │  │
│  │  BackendServiceImpl (routes to appropriate handlers)              │  │
│  │     - Receives all Thrift RPC calls                               │  │
│  │     - Deserializes request payloads                               │  │
│  │     - Routes to operation-specific handlers                       │  │
│  └──────────────────────┬────────────────────────────────────────────┘  │
│                         │                                               │
│  ┌──────────────────────┼────────────────────────────────────────────┐  │
│  │  Operation Handlers (Per Operation Type)                          │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  AgentServer (Agent Tasks)                                  │  │  │
│  │  │  ├─> Task Worker Thread Pools (per task type)               │  │  │
│  │  │  │   ├─> CREATE: Create tablet replicas                     │  │  │
│  │  │  │   ├─> DROP: Drop tablet replicas                         │  │  │
│  │  │  │   ├─> ALTER: Alter tablet schemas                        │  │  │
│  │  │  │   ├─> CLONE: Clone tablet replicas                       │  │  │
│  │  │  │   ├─> PUSH: Push data to tablets                         │  │  │
│  │  │  │   └─> PUBLISH_VERSION: Publish data versions             │  │  │
│  │  │  └─> Returns TAgentResult                                   │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Routine Load Handler                                       │  │  │
│  │  │  ├─> Executes routine load tasks (Kafka, Pulsar)            │  │  │
│  │  │  ├─> Manages data ingestion from external sources           │  │  │
│  │  │  └─> Returns TStatus                                        │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Export Handler                                             │  │  │
│  │  │  ├─> Executes export tasks                                  │  │  │
│  │  │  ├─> Writes query results to external storage               │  │  │
│  │  │  └─> Returns TExportStatusResult                            │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Tablet Statistics Handler                                  │  │  │
│  │  │  ├─> Collects tablet statistics (size, rows, versions)      │  │  │
│  │  │  └─> Returns TTabletStatResult                              │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  External Scanner Handler                                   │  │  │
│  │  │  ├─> open_scanner: Creates scan context                     │  │  │
│  │  │  ├─> get_next: Retrieves data batches                       │  │  │
│  │  │  └─> close_scanner: Releases resources                      │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  ETL Handler                                                │  │  │
│  │  │  ├─> Executes ETL tasks                                     │  │  │
│  │  │  └─> Returns TAgentResult or TMiniLoadEtlStatusResult       │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Response & Status Reporting                          │
│  - Synchronous responses (TAgentResult, TStatus, etc.)                  │
│  - Via heartbeat responses (THeartbeatResult) for agent tasks           │
│  - Status tracking for long-running operations                          │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: BE Thrift server listens on port `9060` (`be_port`) for `BackendService` operations.
- **Service Definition**: Defined in [gensrc/thrift/BackendService.thrift](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/thrift/BackendService.thrift). Important RPC methods include:
    - **Agent Tasks (Tablet Lifecycle Management):**
        - `submit_tasks`: Submits a batch of agent tasks to a BE node for execution. Tasks are serialized as `TAgentTaskRequest` structures using Thrift binary protocol. FE's `AgentBatchTask` batches multiple tasks per BE node to reduce RPC overhead. BE's `AgentServer` receives the batch, divides tasks by type, and dispatches them to appropriate worker thread pools. Each task has a unique signature for idempotency and duplicate detection. Returns `TAgentResult` with execution status.
        - `make_snapshot`: Creates a snapshot of tablet data for backup or replication purposes. Used during tablet cloning and data recovery operations.
        - `release_snapshot`: Releases a previously created snapshot, freeing storage resources.
        - `publish_cluster_state`: Publishes cluster state information to BE nodes, synchronizing metadata and configuration changes.
    - **Data Loading Operations:**
        - `submit_routine_load_task`: Submits routine load tasks to BE nodes for continuous data ingestion from external systems (e.g., Kafka, Pulsar). FE's `RoutineLoadTaskScheduler` creates tasks and submits them via Thrift RPC. Each task includes source information (topic, partitions, offsets) and execution plan parameters.
        - `finish_stream_load_channel`: Completes a stream load channel, finalizing data ingestion for stream load operations.
    - **Export Operations:**
        - `submit_export_task`: Submits export tasks to BE nodes for exporting query results to external storage systems. Used by FE's `ExportMgr` to coordinate data export jobs.
        - `get_export_status`: Retrieves the status of an ongoing export task, enabling FE to track export progress.
        - `erase_export_task`: Cleans up completed or failed export tasks, freeing resources on BE nodes.
    - **ETL Operations:**
        - `submit_etl_task`: Submits ETL (Extract, Transform, Load) tasks for data preprocessing and transformation.
        - `get_etl_status`: Retrieves the execution status of ETL tasks.
        - `delete_etl_files`: Cleans up temporary ETL files after task completion or failure.
    - **Tablet Statistics:**
        - `get_tablet_stat`: Retrieves tablet statistics (data size, row count, version count) from BE nodes. FE's `TabletStatMgr` periodically calls this method to collect statistics for query optimization and cluster rebalancing decisions.
        - `get_tablets_info`: Retrieves detailed tablet information including metadata and configuration.
    - **External Table Scanning:**
        - `open_scanner`: Opens a scanner context for external table data access, enabling efficient scanning of external data sources.
        - `get_next`: Retrieves the next batch of data from an open scanner context.
        - `close_scanner`: Closes a scanner context and releases associated resources.
- **Protocol Serialization**: Uses Thrift binary protocol for all message serialization. Task requests (`TAgentTaskRequest`) contain task type, signature, priority, and type-specific request structures (e.g., `TCreateTabletReq`, `TDropTabletReq`, `TPushReq`).
- **Connection Management**: `ThriftConnectionPool.backendPool` provides connection pooling for Thrift clients, managing socket connections and protocol serialization. The pool maintains up to 128 idle connections per BE endpoint, with a minimum of 2 idle connections. Connections are validated before use and automatically reopened on failure. `ThriftRPCRequestExecutor` handles connection borrowing, retry logic, and connection return/invalidation, supporting configurable retry attempts and timeout settings.

### 3.3. Heartbeat Service

StarRocks uses a dedicated Thrift RPC service on port `9050` (`heartbeat_service_port`) for continuous cluster health monitoring and node state synchronization between FE and BE nodes. FE's `HeartbeatMgr` periodically sends `TMasterInfo` requests to all BE/CN nodes (default interval: 5 seconds), and BE's `HeartbeatServer` responds with `THeartbeatResult` containing node status, resource capacity, port configuration, version information, and tablet reports. The heartbeat protocol serves a dual purpose: in addition to health monitoring, BE piggybacks agent task status updates in `THeartbeatResult` responses, allowing asynchronous status reporting for tasks submitted via Thrift RPC (Section 3.2) without requiring separate status-reporting RPC calls. Failed heartbeats trigger node state transitions (alive → dead) after configurable retry thresholds, enabling automatic failure detection and cluster rebalancing decisions.

**Implementation Details:**

- **Port Configuration**: BE nodes respond to heartbeat requests on port `9050` (`heartbeat_service_port`), using the Thrift RPC service endpoint defined in [gensrc/thrift/BackendService.thrift](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/thrift/BackendService.thrift). The heartbeat response is serialized as `THeartbeatResult` using Thrift binary protocol.

- **Response Content**: `THeartbeatResult` contains:
    - Node status and health information (alive/dead state, last heartbeat time)
    - Resource capacity (CPU cores, available memory, disk space)
    - Port configuration (`be_port`, `http_port`, `brpc_port`, `arrow_flight_port`) for FE to discover BE service endpoints
    - Version information (StarRocks version, build information) for compatibility checks
    - Tablet reports containing tablet metadata, replica information, and data distribution statistics for cluster rebalancing decisions
    - Agent task status updates (piggybacked in heartbeat responses) for tasks submitted via Thrift RPC (Section 3.2), enabling asynchronous status reporting without separate RPC calls

- **Heartbeat Processing**: BE's `HeartbeatServer` processes incoming `TMasterInfo` requests from FE's `HeartbeatMgr` and collects node status information from various BE subsystems (resource manager, tablet manager, agent server). The collected information is serialized into `THeartbeatResult` and sent back to FE via Thrift RPC. FE processes heartbeat responses to update node state, trigger cluster rebalancing based on tablet reports, and track agent task completion status.

- **Agent Task Status Reporting**: BE's `AgentServer` executes agent tasks submitted via Thrift RPC (Section 3.2) and tracks task execution status. `HeartbeatServer` collects agent task status from `AgentServer` and includes it in `THeartbeatResult` responses. Agent task status includes completion status (success, failure, in-progress), error messages, execution results, progress information, and task signatures for idempotency and duplicate detection. This piggybacking approach eliminates the need for separate status-reporting RPC calls, reducing network overhead and simplifying the communication protocol. Task status is reported periodically through heartbeat responses (typically every 5 seconds) or upon task completion, enabling FE to track task progress in near real-time without additional RPC overhead.

### 3.4. HTTP-based Protocol

HTTP serves as a complementary protocol for FE-BE communication, providing RESTful interfaces for data loading, metrics collection, administrative operations, and direct client access to BE nodes. While bRPC and Thrift RPC handle the core query execution and administrative workflows, HTTP offers better firewall compatibility, easier debugging with standard HTTP tools, and direct client-to-BE access patterns that bypass FE for certain operations.

- **Port Configuration**: BE nodes expose HTTP services on port `8040` (`be_http_port`).
- **Overlapping Functionalities and Default Protocols:**
    - **Health Checks**: FE primarily uses the Thrift heartbeat service (Section 3.3, port `9050`) for cluster health monitoring by default. HTTP health check endpoints (e.g., `/api/health`) serve as an auxiliary mechanism for external monitoring tools and load balancers that require standard HTTP health probes.
    - **Metrics and Profiles**: Query execution profiles are primarily collected via Thrift RPC's `report_exec_status` (Section 6.1), where BE proactively reports execution status and profiles to FE. HTTP endpoints (`/api/metrics`, `/api/proc_profile`) provide RESTful access to the same information, enabling external monitoring systems, debugging tools, and FE's web UI to fetch metrics and profiles without requiring RPC client libraries.
- **Unique HTTP Capabilities:**
    - **Stream Load**: HTTP is the primary transport mechanism for Stream Load operations. Clients can send Stream Load requests directly to BE nodes via HTTP (e.g., `PUT /api/{db}/{table}/_stream_load`), bypassing FE for data ingestion. FE can also proxy Stream Load requests to BE nodes when clients connect through FE. This differs from routine load (Section 3.2), which uses Thrift RPC for task submission.
    - **Transaction-Based Stream Load**: HTTP provides transaction management endpoints (`/api/transaction/{txn_op}`) for multi-statement transaction support in Stream Load, enabling atomic batch loading operations.
    - **File Downloads**: HTTP endpoints enable downloading load error logs (`/api/_load_error_log`), data files (`/api/_download_load`), and tablet files (`/api/_tablet/_download`), which are essential for troubleshooting and data recovery operations.
    - **Administrative REST APIs**: HTTP provides RESTful interfaces for various administrative tasks that complement Thrift RPC operations:
        - Tablet management: `/api/reload_tablet`, `/api/restore_tablet`, `/api/snapshot`
        - Compaction control: `/api/compaction/show`, `/api/compact`
    - Configuration updates: `/api/update_config`
    - Cache management: `/api/query_cache/{action}`, `/api/runtime_filter_cache/{action}`, `/api/datacache/{action}`
    - **Performance Profiling**: HTTP exposes pprof-compatible endpoints (`/pprof/heap`, `/pprof/profile`, etc.) for CPU and memory profiling, enabling integration with standard profiling tools.
    - **Web UI and Human-Readable Interfaces**: HTTP serves web pages and human-readable diagnostic information (e.g., `/varz` for configuration viewing, `/greplog` for log searching), making it easier for operators to inspect BE state without specialized RPC clients.

HTTP coexists with bRPC and Thrift RPC because it addresses different use cases and access patterns. While RPC protocols are optimized for programmatic FE-BE communication with low overhead and strong typing, HTTP provides:

- **Direct Client Access**: Clients can interact with BE nodes directly without going through FE, enabling use cases like direct Stream Load and administrative operations.
- **Firewall Compatibility**: HTTP's ubiquity and standard port usage (80/443) make it easier to deploy in restricted network environments where custom RPC ports may be blocked.
- **Tooling Ecosystem**: HTTP's widespread support in monitoring tools, load balancers, and debugging utilities enables seamless integration with existing infrastructure.
- **Human-Friendly Interfaces**: Web-based UIs and RESTful APIs are more accessible for operators and external systems that prefer standard HTTP tooling over custom RPC clients.
- **Alternative Transport**: HTTP can serve as a fallback transport mechanism for data exchange (Section 4.3) when bRPC is restricted, though with higher protocol overhead.

## 4. Backend-Backend Transport

Backend nodes communicate with each other primarily for data exchange during query execution. This is the core data plane communication in StarRocks, enabling distributed query processing through efficient columnar data transmission. While FE-BE communication (Section 3) handles query coordination and administrative operations, BE-BE communication focuses on high-throughput data movement between execution nodes.

### 4.1. bRPC for Chunk Transmission

StarRocks transmits vectorized data between BE nodes using columnar chunks (`Chunk`), serialized as Protobuf `ChunkPB` messages. This is the primary mechanism for data exchange during distributed query execution, supporting hash partitioning, broadcast, and range partitioning patterns. The protocol leverages bRPC (Section 3.1) for efficient RPC communication, reusing the same bRPC infrastructure established for FE-BE query coordination.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node A (Sender)                              │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  DataStreamSender                                                 │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Accumulates chunks until threshold (16MB)                │  │  │
│  │  │  - Serializes Chunk → ChunkPB                               │  │  │
│  │  │  - Applies compression (LZ4/ZLIB/SNAPPY)                    │  │  │
│  │  │  - Batches multiple chunks per RPC                          │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  PTransmitChunkParams                                             │  │
│  │  - finst_id, node_id, sender_id                                   │  │
│  │  - sequence (for loss detection)                                  │  │
│  │  - eos (end-of-stream flag)                                       │  │
│  │  - ChunkPB[] (one or more chunks)                                 │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ BRPC transmit_chunk (Port 8060)
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node B (Receiver)                            │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  DataStreamMgr                                                    │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  DataStreamRecvr (Hash Table: 127 buckets)                  │  │  │
│  │  │  ├─> Key: (fragment_instance_id, node_id)                   │  │  │
│  │  │  ├─> Validates sequence numbers                             │  │  │
│  │  │  ├─> Enqueues chunks into receiver buffer                   │  │  │
│  │  │  └─> Supports merge (sorted) or separate queues (hash)      │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  ExchangeSourceOperator (Pipeline Driver)                         │  │
│  │  - Pulls chunks from receiver buffer                              │  │
│  │  - Deserializes ChunkPB → Chunk                                   │  │
│  │  - Decompresses if needed                                         │  │
│  │  - Feeds chunks to downstream operators                           │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Query Execution Continuation                         │
│  - Joins, aggregations, sorting                                         │
│  - Further data exchange if needed                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: BE nodes use port `8060` (`brpc_port`) for chunk transmission, sharing the same bRPC service endpoint used for FE-BE communication (Section 3.1). The bRPC framework multiplexes multiple RPC methods (`exec_plan_fragment`, `transmit_chunk`, `fetch_data`, etc.) over the same port.
- **Service Definition**: The `transmit_chunk` RPC method is defined in [gensrc/proto/internal_service.proto](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/proto/internal_service.proto), using the same `PInternalService` interface as FE-BE communication. The method accepts `PTransmitChunkParams` containing one or more `ChunkPB` messages, enabling efficient batching of multiple chunks in a single RPC call.
- **Protocol Details**:
    - `PTransmitChunkParams` carries one or more `ChunkPB` messages. Multiple chunks are batched in a single RPC to amortize per-call overhead and improve network utilization.
    - Each `PTransmitChunkParams` includes:
        - `finst_id`: Fragment instance ID identifying the query execution context
        - `node_id`: Destination plan node ID for routing chunks to the correct receiver
        - `sender_id`: Sender identifier for multi-sender scenarios (e.g., broadcast joins with multiple senders)
        - `sequence`: RPC sequence number for loss detection and ordering guarantees
        - `eos`: End-of-stream flag indicating no more data will be sent
    - The receiver validates sequence numbers to detect packet loss and triggers retransmission or query failure, ensuring data integrity during transmission.
- **Serialization**:
    - `DataStreamSender::serialize_chunk()` converts in-memory `Chunk` objects to `ChunkPB` format. Column data is serialized in columnar layout, preserving vectorization benefits and enabling efficient compression.
    - Compression is applied per-chunk using configurable codecs (`LZ4`, `ZLIB`, `SNAPPY`). Compression reduces network bandwidth at the cost of CPU cycles. `LZ4` provides a good balance for most workloads, offering fast compression with reasonable compression ratios.
    - The first chunk in a stream includes metadata (column schemas, types), while subsequent chunks only contain data payloads, minimizing overhead for large data transfers.
- **Batching Strategy**:
    - `DataStreamSender` accumulates chunks until reaching a threshold (`_request_bytes_threshold`, typically 16MB) before sending an RPC. This batching reduces RPC overhead and improves network utilization by amortizing per-call costs across larger payloads.
    - When `eos` is true, all buffered data is flushed immediately, ensuring timely query completion and preventing latency from delayed final chunks.
    - Batching is configurable via query options to balance latency and throughput based on workload characteristics.
- **Data Stream Management**:
    - **Receiver Creation**: `DataStreamMgr::create_recvr()` creates a `DataStreamRecvr` for a specific `(fragment_instance_id, node_id)` pair. The receiver maintains a queue of incoming chunks from multiple senders, enabling parallel data exchange patterns.
    - **Receiver Organization**: Receivers are organized in a hash table (127 buckets) keyed by fragment instance ID to enable O(1) lookup during data arrival, minimizing overhead in high-concurrency scenarios.
    - **Stream Merging**: `DataStreamRecvr` supports merging sorted streams from multiple senders (for merge joins) or maintaining separate queues per sender (for hash joins), adapting to different query execution patterns.
- **Threading Model**:
    - BRPC service threads handle incoming `transmit_chunk` RPCs and enqueue chunks into the appropriate receiver's buffer, decoupling network I/O from query execution.
    - Pipeline execution threads (drivers) pull chunks from receivers via `ExchangeSourceOperator`, enabling asynchronous producer-consumer data flow and preventing blocking between senders and receivers.
    - This decoupling allows senders and receivers to operate at different rates, handling backpressure gracefully and preventing memory exhaustion from rate mismatches.
- **Memory Management**:
    - Receiver buffers are bounded to prevent memory exhaustion. When buffers are full, senders block until space becomes available, providing natural backpressure mechanisms.
    - Query cancellation (`DataStreamMgr::cancel()`) immediately unblocks all waiting receivers and discards buffered data, enabling fast query termination and resource cleanup.
    - Memory is tracked via dedicated MemTrackers for data stream buffers, enabling accurate memory accounting and preventing query memory leaks.

### 4.2. Pass-Through Optimization

For fragments executing on the same BE node, StarRocks employs a zero-copy pass-through mechanism to avoid serialization and network overhead. This optimization is automatically enabled when source and destination fragments execute on the same node, eliminating unnecessary data movement and improving query performance for local operations.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Single Backend Node                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Fragment A (Source)                                              │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  ExchangeSinkOperator                                       │  │  │
│  │  │  ├─> Detects same-node destination                          │  │  │
│  │  │  ├─> Enables use_pass_through flag                          │  │  │
│  │  │  └─> Appends chunks to PassThroughChunkBuffer               │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  PassThroughChunkBuffer (Zero-Copy)                               │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Per-sender channels                                        │  │  │
│  │  │  - Stores ChunkUniquePtr (in-memory)                        │  │  │
│  │  │  - No serialization/deserialization                         │  │  │
│  │  │  - No network transmission                                  │  │  │
│  │  │  - Memory tracked via passthrough_mem_tracker               │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  Fragment B (Destination)                                         │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  ExchangeSourceOperator                                     │  │  │
│  │  │  ├─> Pulls chunks from PassThroughChunkBuffer               │  │  │
│  │  │  └─> Feeds chunks to downstream operators                   │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Mechanism**: When `use_pass_through` is enabled, `ExchangeSinkOperator` detects that the destination fragment executes on the same BE node and appends chunks directly to a local `PassThroughChunkBuffer` instead of serializing and sending via RPC (Section 4.1). This eliminates serialization overhead, network transmission, and deserialization costs for same-node data exchange.
- **Buffer Management**: `PassThroughChunkBuffer` maintains per-sender channels, storing chunks as in-memory `ChunkUniquePtr` objects. The buffer provides thread-safe enqueue and dequeue operations, enabling concurrent access from multiple senders and receivers.
- **Data Flow**: `ExchangeSourceOperator` pulls chunks from the local buffer, eliminating serialization, network transmission, and deserialization overhead. Chunks are transferred via pointer references, preserving zero-copy semantics throughout the data path.
- **Memory Tracking**: Pass-through chunks are tracked by a dedicated `passthrough_mem_tracker` to prevent memory leaks. Memory ownership is transferred from the sender's MemTracker to the pass-through tracker, then to the receiver's MemTracker, ensuring accurate memory accounting across fragment boundaries.
- **Use Cases**: Pass-through is automatically enabled when source and destination fragments execute on the same BE node. This optimization is particularly beneficial for broadcast joins (where broadcast data is reused across multiple receivers on the same node) and local aggregations (where intermediate results are exchanged between fragments on the same node).

### 4.3. Peer Cache Access Protocol

BE nodes can fetch cached data from peer nodes to improve query performance and reduce storage I/O. This enables distributed caching where nodes can access cached data from other nodes in the cluster, reducing redundant storage reads and improving cache hit rates across the cluster. The protocol leverages bRPC (Section 3.1) for efficient RPC communication, reusing the same bRPC infrastructure used for chunk transmission (Section 4.1).

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node A (Cache Requester)                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  DataCache                                                        │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Cache miss detected                                      │  │  │
│  │  │  - Checks peer nodes for cached data                        │  │  │
│  │  │  - Constructs PFetchDataCacheRequest                        │  │  │
│  │  │  - Sends bRPC request to peer node                          │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ BRPC fetch_datacache (Port 8060)
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node B (Cache Provider)                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  PInternalService                                                 │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Receives fetch_datacache RPC                             │  │  │
│  │  │  - Looks up cache key in local cache                        │  │  │
│  │  │  - Reads cached data from storage                           │  │  │
│  │  │  - Returns data in response attachment                      │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Cache Data Delivery                                  │
│  - Cached data returned in response attachment                          │
│  - Stored in requester's local cache for future use                     │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: BE nodes use port `8060` (`brpc_port`) for peer cache access, sharing the same bRPC service endpoint used for FE-BE communication (Section 3.1) and chunk transmission (Section 4.1).
- **Service Definition**: The `fetch_datacache` RPC method is defined in [gensrc/proto/internal_service.proto](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/proto/internal_service.proto), using the same `PInternalService` interface as other BE-BE operations. The method accepts `PFetchDataCacheRequest` containing cache key, offset, and size, and returns `PFetchDataCacheResponse` with cached data in the response attachment.
- **Protocol Details**:
    - `PFetchDataCacheRequest` contains:
        - Cache key identifying the cached data block
        - Offset and size specifying the data range to fetch
        - Optional metadata for cache validation
    - `PFetchDataCacheResponse` contains:
        - Status indicating cache hit or miss
        - Cached data in the response attachment (for cache hits)
        - Metadata for cache validation and consistency checks
    - The provider node looks up the cache key in its local cache, reads the cached data from storage if available, and returns it in the response attachment. If the cache key is not found, the response indicates a cache miss.
- **Use Cases**: When a BE node needs data that is cached on another BE node, it can fetch it via this RPC instead of reading from storage, reducing I/O and improving query performance. This is particularly beneficial for:
    - Frequently accessed data that is cached on multiple nodes
    - Data that is expensive to read from storage but has been recently accessed by another node
    - Distributed query patterns where multiple nodes access the same data blocks

### 4.4. HTTP-Based Protocol

StarRocks supports HTTP as an alternative transport mechanism for data transmission, useful in environments where bRPC is restricted by firewall rules or network policies. While HTTP transport is less efficient than bRPC due to higher protocol overhead, it provides better firewall compatibility and easier debugging with standard HTTP tools. This complements the HTTP capabilities described in Section 3.4, extending HTTP support to BE-BE data exchange.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node A (Sender)                              │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  DataStreamSender (HTTP Mode)                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Serializes Chunk → ChunkPB                               │  │  │
│  │  │  - Applies compression (LZ4/ZLIB/SNAPPY)                    │  │  │
│  │  │  - Encodes as HTTP request body                             │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ HTTP POST /api/transmit_chunk (Port 8040)
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend Node B (Receiver)                            │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  HTTP Handler                                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Receives HTTP request                                    │  │  │
│  │  │  - Extracts ChunkPB from request body                       │  │  │
│  │  │  - Routes to DataStreamMgr (same as bRPC path)              │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  DataStreamMgr (Shared with bRPC)                                 │  │
│  │  - Same receiver management as Section 4.1                        │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
```

- **Port Configuration**: BE nodes use port `8040` (`be_http_port`) for HTTP-based chunk transmission, sharing the same HTTP service endpoint used for FE-BE HTTP communication (Section 3.4) and other HTTP operations.
- **Service Definition**: HTTP endpoint `/api/transmit_chunk` accepts POST requests containing serialized `ChunkPB` data in the request body. The endpoint uses the same `DataStreamMgr` infrastructure as bRPC-based transmission (Section 4.1), ensuring consistent behavior regardless of transport mechanism.
- **Protocol Details**:
    - HTTP requests use standard HTTP/1.1 POST method with `ChunkPB` data serialized in the request body. The same compression codecs (`LZ4`, `ZLIB`, `SNAPPY`) are supported as in bRPC transmission.
    - Request headers include fragment instance ID, node ID, and sender ID for routing chunks to the correct receiver, mirroring the metadata carried in `PTransmitChunkParams` for bRPC.
    - The receiver validates request metadata and enqueues chunks into the same `DataStreamRecvr` buffers used by bRPC transmission, ensuring seamless integration with the query execution pipeline.
- **Trade-offs**: HTTP transport is less efficient than bRPC due to higher protocol overhead (HTTP headers, connection management) but provides better firewall compatibility and easier debugging with standard HTTP tools (curl, browser dev tools, HTTP proxies). HTTP is typically used as a fallback when bRPC ports are restricted, with bRPC remaining the preferred transport for optimal performance.

## 5. Frontend-Frontend Transport

Frontend nodes communicate with each other for metadata consistency, leader election, and cluster coordination. This communication is critical for maintaining a consistent view of cluster metadata across all FE nodes, ensuring high availability and data consistency. While FE-BE communication (Section 3) handles query execution and administrative operations, FE-FE communication focuses on metadata replication and cluster coordination to maintain a unified metadata view across the cluster.

### 5.1. BDB JE for Edit Log Replication

FE nodes replicate metadata changes through BDB JE (Berkeley DB Java Edition) edit logs, ensuring all FE nodes maintain consistent metadata. This protocol provides strong consistency guarantees through a leader-follower replication model, where the leader FE handles all write operations and replicates changes to follower and observer nodes via BDB JE's built-in replication mechanism.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                        Leader FE                                        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  LeaderImpl                                                       │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Handles all write operations                             │  │  │
│  │  │  - Writes edit log entries via EditLog                      │  │  │
│  │  │  - Replicates to followers via BDB JE                       │  │  │
│  │  │  - Maintains transaction consistency                        │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  BDB JE (Berkeley DB Java Edition)                                │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  BDBEnvironment                                             │  │  │
│  │  │  ├─> ReplicationConfig (replica ack timeout, etc.)          │  │  │
│  │  │  ├─> EnvironmentConfig (transactional, cache, etc.)         │  │  │
│  │  │  └─> BDBJEJournal (edit log storage)                        │  │  │
│  │  │                                                             │  │  │
│  │  │  Edit Log Entries                                           │  │  │
│  │  │  - DDL operations (CREATE, ALTER, DROP TABLE)               │  │  │
│  │  │  - Schema changes                                           │  │  │
│  │  │  - User/role management                                     │  │  │
│  │  │  - Database/catalog operations                              │  │  │
│  │  │  - Cluster configuration                                    │  │  │
│  │  │  - Transaction state changes                                │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ BDB JE Replication (Port 9010)
                          │ - Transactional replication protocol
                          │ - Automatic failover support
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Follower/Observer FE Nodes                           │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Edit Log Replay                                                  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  BDBEnvironment (Replica Mode)                              │  │  │
│  │  │  ├─> Receives edit log entries from leader                  │  │  │
│  │  │  ├─> Replays entries via EditLog.replayJournal()            │  │  │
│  │  │  ├─> Updates in-memory metadata (Catalog, Database, etc.)   │  │  │
│  │  │  ├─> Stores persistently in BDB JE                          │  │  │
│  │  │  └─> Maintains consistent metadata view                     │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  Leader Election (Follower only, not Observer)                    │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Participates in consensus protocol (Raft-like)           │  │  │
│  │  │  - Elects new leader on failure                             │  │  │
│  │  │  - Requires majority quorum                                 │  │  │
│  │  │  - Observer nodes do not participate                        │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: FE nodes listen on port `9010` (`edit_log_port`) for BDB JE replication protocol.
- **Protocol Details**:
    - BDB JE uses a custom replication protocol built on top of Berkeley DB Java Edition, providing transactional consistency and durability guarantees. The protocol supports automatic failover and maintains strong consistency across all FE nodes.
    - Replication configuration includes replica acknowledgment timeout, heartbeat timeout, and transaction rollback limits, ensuring reliable metadata replication even under network partitions or node failures.
    - The leader FE writes edit log entries to its local BDB JE environment, which automatically replicates entries to follower and observer nodes via the replication protocol.
- **Leader-Follower Model**:
    - The leader FE handles all write operations (DDL, metadata changes, configuration updates) and writes edit log entries via `EditLog`. Follower FEs forward write requests to the leader to ensure consistency.
    - Follower FEs receive edit log entries from the leader and replay them to update their in-memory metadata state. Observer nodes also receive edit logs but do not participate in leader election, improving read scalability without affecting consensus decisions.
    - Leader failure triggers automatic leader election among remaining follower nodes. For leader election to succeed, a majority of follower nodes must be alive (quorum requirement), ensuring cluster availability even with node failures.
- **Edit Log Contents**: Edit log entries contain metadata mutations including:
    - DDL operations (CREATE TABLE, ALTER TABLE, DROP TABLE, CREATE DATABASE, etc.)
    - Schema changes and column modifications
    - User and role management operations
    - Database and catalog operations
    - Cluster configuration changes
    - Transaction state changes and commit records
- **Metadata Synchronization**:
    - Follower FEs replay edit logs via `EditLog.replayJournal()` to maintain an in-memory copy of metadata. The replay process updates catalog state, database schemas, user permissions, and other metadata structures.
    - Metadata is stored persistently in BDB JE, enabling recovery after node restarts. Nodes can recover their metadata state by replaying edit logs from the beginning or from the last checkpoint.
    - Edit log replication ensures all FE nodes maintain consistent metadata views, enabling any FE node to serve read requests with the same metadata state as the leader.
- **Consistency Guarantees**: BDB JE replication provides transactional consistency, ensuring that metadata changes are either fully replicated to all nodes or not applied at all. This prevents partial updates and maintains cluster-wide metadata consistency.

### 5.2. Thrift RPC for Administrative Operations

FE nodes can communicate via Thrift RPC for certain administrative operations and inter-FE coordination tasks. While edit log replication (Section 5.1) is the primary mechanism for metadata consistency, Thrift RPC serves as a complementary protocol for administrative operations that require direct FE-to-FE communication without going through the edit log replication path.

**Implementation Diagram:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                        Frontend Node A                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Administrative Operations                                        │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Metadata queries (getTableNames, getTablesInfo)          │  │  │
│  │  │  - Profile retrieval (getQueryProfile)                      │  │  │
│  │  │  - Load job status (getLoads, getStreamLoads)               │  │  │
│  │  │  - Forward operations (forward)                             │  │  │
│  │  │  - Method capability checks (isMethodSupported)             │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  ThriftClient                                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Connection pooling                                       │  │  │
│  │  │  - Thrift binary protocol                                   │  │  │
│  │  │  - Serializes request/response                              │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  └──────────────────────┼────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ Thrift RPC (Port 9020)
                          │ - Thrift binary protocol
                          │ - FrontendService interface
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        Frontend Node B                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  FrontendService (Thrift Service Interface)                       │  │
│  │     ↑ implements                                                  │  │
│  │  FrontendServiceImpl                                              │  │
│  │     - Receives Thrift RPC calls                                   │  │
│  │     - Deserializes request payloads                               │  │
│  │     - Routes to appropriate handlers                              │  │
│  │     - Returns response data                                       │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

- **Port Configuration**: FE Thrift server listens on port `9020` (`rpc_port`) for `FrontendService` operations.
- **Service Definition**: Defined in [gensrc/thrift/FrontendService.thrift](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/thrift/FrontendService.thrift). Important RPC methods include:
    - **Metadata Queries**: `getTableNames`, `getTablesInfo`, `getTablesConfig` - Retrieve table metadata and configuration information from other FE nodes.
    - **Profile Retrieval**: `getQueryProfile` - Retrieve query execution profiles from other FE nodes for diagnostics and monitoring.
    - **Load Job Status**: `getLoads`, `getTrackingLoads`, `getRoutineLoadJobs`, `getStreamLoads` - Query load job status and progress from other FE nodes.
    - **Forward Operations**: `forward` - Forward administrative operations to the leader FE node, enabling follower FEs to delegate write operations to the leader.
    - **Method Capability Checks**: `isMethodSupported` - Check if a remote FE node supports a specific RPC method, enabling version compatibility checks.
    - **Execution Status Reporting**: `reportExecStatus`, `batchReportExecStatus` - Receive execution status reports from BE nodes, though this is primarily used for BE-FE communication (covered in Section 3).
- **Protocol Serialization**: Uses Thrift binary protocol for all message serialization. Request and response structures are defined in the `FrontendService.thrift` file, ensuring type safety and version compatibility.
- **Use Cases**: Thrift RPC is primarily used for:
    - Administrative queries that require direct FE-to-FE communication without metadata replication overhead
    - Load balancing and failover scenarios where FE nodes need to query metadata from other nodes
    - Version compatibility checks and method capability discovery
    - Forwarding operations from follower FEs to the leader FE
- **Migration Path**: New features prefer direct edit log replication (Section 5.1) for metadata consistency, as it provides stronger consistency guarantees and better scalability. Thrift RPC is maintained for backward compatibility and specific administrative use cases that require direct inter-FE communication.

## 6. Backend-Frontend Transport

Backend-Frontend communication in StarRocks includes both responses to Frontend-initiated requests and BE-initiated proactive reporting. Most BE-FE communication patterns are responses to FE-initiated requests covered in Section 3 (Frontend-Backend Transport), where FE initiates operations and BE responds accordingly. This includes:

- **Heartbeat responses**: BE responds to FE's heartbeat requests with node status and agent task updates (Section 3.3)
- **Query result delivery**: BE responds to FE's `fetch_data` RPC calls with query results (Section 3.1)
- **Agent task status**: BE piggybacks task status in heartbeat responses (Section 3.3)

### 6.1. Execution Status Reporting via Thrift RPC

BE nodes proactively report query execution status and profiles to FE nodes using Thrift RPC, enabling FE to track query progress, collect diagnostic information, and monitor fragment execution without requiring FE to poll BE nodes.

**Implementation Details:**

- **Port Configuration**: BE nodes call FE's `FrontendService` on port `9020` (`rpc_port`) using Thrift RPC. This is the same service interface used for FE-FE communication (Section 5.2), but BE nodes initiate calls to report execution status.
- **Service Definition**: The `reportExecStatus` and `batchReportExecStatus` methods are defined in [gensrc/thrift/FrontendService.thrift](https://github.com/StarRocks/starrocks/blob/f4a8df573a1140c8fbfbdf43551f62b6debdc4d1/gensrc/thrift/FrontendService.thrift). BE sends `TReportExecStatusParams` containing execution status, completion flags, and profile data, and receives `TReportExecStatusResult` with acknowledgment status.
- **Reporting Mechanisms**: BE uses multiple mechanisms to report execution status, depending on query type and execution engine:
    - **Pipeline Engine Periodic Reporting**: For pipeline-based queries (default execution engine), `FragmentContext::report_exec_state_if_necessary()` is called during driver execution. It reports profiles periodically based on the `runtime_profile_report_interval` session variable (default: 10 seconds). Reporting only occurs if profiling is enabled (`enable_profile=true`) and all pipeline drivers have completed local preparation. The reporting interval is normalized to prevent jitter, ensuring consistent timing regardless of execution noise.
    - **ProfileReportWorker for Load Queries**: A dedicated background thread (`ProfileReportWorker`) periodically reports profiles specifically for LOAD queries (both pipeline and non-pipeline). It checks registered load tasks every `profile_report_interval` seconds (BE config, default: 30 seconds) and triggers profile reporting for fragments that haven't reported within the interval. This mechanism ensures long-running load operations provide regular progress updates even if they don't trigger other reporting mechanisms.
    - **Fragment Completion Reporting**: When a fragment finishes execution (successfully or with errors), BE always sends a final status report with `done=true` and the complete profile attached. This occurs in both pipeline and non-pipeline execution engines, ensuring FE receives final execution state regardless of periodic reporting settings.
    - **On-Demand Reporting via trigger_profile_report (Optional, Currently Unused)**: FE can theoretically request profiles on-demand by calling the `trigger_profile_report` bRPC method. When BE receives this request, it immediately triggers `report_exec_state` for the specified fragments, bypassing normal periodic reporting intervals. However, this mechanism is not currently used in the FE codebase - all profile collection relies on BE-initiated `report_exec_status` calls. The API exists for potential future use cases requiring synchronous profile retrieval.
    - **Non-Pipeline Engine Reporting**: For non-pipeline execution (legacy engine), `FragmentExecState::coordinator_callback()` reports status when fragment execution completes or encounters errors. This mechanism is primarily used for backward compatibility with older query execution paths.
- **Profile Data Serialization**: Execution profiles are serialized as `TRuntimeProfileTree` using Thrift binary protocol and included in `TReportExecStatusParams`. Profiles track execution timings, resource usage (CPU, memory, I/O), row counts, data sizes, network transmission statistics, and custom counters from all operators and fragments involved in query execution. For pipeline queries, profiles are merged across all driver instances within a fragment before reporting, and query-level metrics (peak memory, cumulative CPU time, spill bytes, execution wall time) are added to the fragment profile.
- **Load Job Status**: For load operations, BE includes additional status information in `TReportExecStatusParams`, such as loaded rows, load bytes, filtered rows, unselected rows, rejected record paths, tablet commit/fail information, sink commit information, and load channel profiles. Load channel profiles track data ingestion metrics separately and are merged on FE. This enables FE to track load job progress, handle errors, and provide detailed load statistics.
- **Batch Reporting**: BE can batch multiple fragment status reports in a single `batchReportExecStatus` RPC call when multiple fragments on the same BE node need to report status to the same FE coordinator. This reduces network overhead and RPC call count, particularly beneficial for queries with many fragments or when `ProfileReportWorker` triggers reporting for multiple load tasks simultaneously.
- **Asynchronous Execution**: Profile reporting is executed asynchronously via dedicated thread pools (`ExecStateReporter`) to avoid blocking query execution. The reporter maintains separate thread pools for normal and priority reporting, with priority reporting used for final status reports of load operations to ensure timely delivery. Reports are retried up to `report_exec_rpc_request_retry_num` times (default: 3) on failure, with special handling for final status reports to ensure they are eventually delivered.
- **Connection Management**: BE uses Thrift RPC client connections to FE nodes, establishing connections to the coordinator FE node that deployed the query fragments. Connections are managed with retry logic and timeout handling (`thrift_rpc_timeout_ms`) to ensure reliable status delivery even under network issues or FE node failures. Failed reports for final status (done=true) are retried more aggressively to prevent query state from getting stuck.
- **Profile Enablement**: Profile reporting only occurs when profiling is enabled. For queries, this is controlled by the `enable_profile` session variable or query context setting. For load operations, profiles are reported based on load-specific profile enablement settings. When profiling is disabled, BE still reports execution status (completion, errors) but without profile data to reduce overhead.
