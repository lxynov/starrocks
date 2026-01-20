---
title: StarRocks Observability
---

## Introduction

Observability is crucial for operating and optimizing any database system at scale. StarRocks provides a comprehensive observability ecosystem that enables administrators and developers to monitor, debug, and optimize their clusters effectively. This blog post explores the key observability components in StarRocks, including AuditLoader, Query Profiles, metrics, logging, and tracing capabilities.

## 1. AuditLoader: Centralized Audit Log Management

### Overview

StarRocks generates detailed audit logs for every SQL statement executed in the cluster. By default, these logs are stored in local files (`fe/log/fe.audit.log`), making them difficult to query and analyze at scale. **AuditLoader** is a plugin that solves this problem by automatically loading audit logs into StarRocks tables, enabling SQL-based analysis of query patterns, performance trends, and security events.

### Key Features

- **Automatic Log Ingestion**: AuditLoader runs as a background plugin on each FE node, continuously reading audit logs and loading them into StarRocks via Stream Load
- **SQL-Based Analysis**: Once loaded, audit logs become queryable data, allowing you to use SQL to analyze query patterns, identify slow queries, track resource usage, and audit user activity
- **Filtering Capabilities**: Configure filters to selectively load audit logs based on criteria such as query type, client IP, user, or other attributes
- **Version Compatibility**: The plugin is designed to handle schema evolution across StarRocks versions, ensuring compatibility during upgrades

### Audit Log Schema

The audit log table captures comprehensive information about each query execution:

- **Query Identification**: `queryId`, `timestamp`, `queryType`, `stmtId`
- **User Context**: `user`, `authorizedUser`, `clientIp`, `feIp`
- **Resource Management**: `resourceGroup`, `warehouse`, `catalog`, `db`
- **Performance Metrics**: `queryTime`, `scanBytes`, `scanRows`, `returnRows`, `cpuCostNs`, `memCostBytes`
- **Planning Metrics**: `planCpuCosts`, `planMemCosts`, `pendingTimeMs`
- **Materialized View Usage**: `candidateMVs`, `hitMVs`
- **Query Details**: `stmt` (full SQL statement), `digest` (SQL fingerprint), `state`, `errorCode`

### Installation and Configuration

1. **Create the Audit Log Table**: Design a table with dynamic partitioning to store audit logs efficiently:

```sql
CREATE DATABASE starrocks_audit_db__;

CREATE TABLE starrocks_audit_db__.starrocks_audit_tbl__ (
  `queryId` VARCHAR(64) COMMENT "Unique ID of the query",
  `timestamp` DATETIME NOT NULL COMMENT "Query start time",
  `queryType` VARCHAR(12) COMMENT "Query type (query, slow_query, connection)",
  `clientIp` VARCHAR(32) COMMENT "Client IP",
  `user` VARCHAR(64) COMMENT "Query username",
  `authorizedUser` VARCHAR(64) COMMENT "Unique identifier of the user",
  `resourceGroup` VARCHAR(64) COMMENT "Resource group name",
  `catalog` VARCHAR(32) COMMENT "Catalog name",
  `db` VARCHAR(96) COMMENT "Database where the query runs",
  `state` VARCHAR(8) COMMENT "Query state (EOF, ERR, OK)",
  `errorCode` VARCHAR(512) COMMENT "Error code",
  `queryTime` BIGINT COMMENT "Query execution time (milliseconds)",
  `scanBytes` BIGINT COMMENT "Number of bytes scanned",
  `scanRows` BIGINT COMMENT "Number of rows scanned",
  `returnRows` BIGINT COMMENT "Number of rows returned",
  `cpuCostNs` BIGINT COMMENT "CPU time consumed (nanoseconds)",
  `memCostBytes` BIGINT COMMENT "Memory consumed (bytes)",
  `stmt` VARCHAR(1048576) COMMENT "Original SQL statement",
  `digest` VARCHAR(32) COMMENT "Fingerprint of slow SQL",
  `planCpuCosts` DOUBLE COMMENT "CPU usage during query planning",
  `planMemCosts` DOUBLE COMMENT "Memory usage during query planning",
  `pendingTimeMs` BIGINT COMMENT "Time the query waited in the queue",
  `candidateMVs` VARCHAR(65533) NULL COMMENT "List of candidate materialized views",
  `hitMvs` VARCHAR(65533) NULL COMMENT "List of matched materialized views",
  `warehouse` VARCHAR(32) NULL COMMENT "Warehouse name"
) ENGINE = OLAP
DUPLICATE KEY (`queryId`, `timestamp`, `queryType`)
COMMENT "Audit log table"
PARTITION BY date_trunc('day', `timestamp`)
PROPERTIES (
  "replication_num" = "1",
  "partition_live_number" = "30"
);
```

2. **Download and Configure AuditLoader**: Download the plugin package, configure connection details, and optionally set up filtering rules

3. **Install the Plugin**: Use the `INSTALL PLUGIN` statement to activate AuditLoader on all FE nodes

### Use Cases

- **Performance Analysis**: Identify slow queries, analyze query patterns, and track resource consumption trends
- **Security Auditing**: Monitor user activity, track access patterns, and detect suspicious behavior
- **Capacity Planning**: Analyze query volumes, resource usage, and peak load patterns
- **Troubleshooting**: Correlate query failures with system events and identify problematic query patterns

## 2. Query Profile: Deep Query Execution Analysis

### Overview

**Query Profile** is StarRocks' powerful tool for analyzing query execution performance. It provides detailed, node-level execution information for every operator in a query plan, enabling you to identify bottlenecks, optimize query plans, and understand resource consumption patterns.

### Key Capabilities

- **Operator-Level Metrics**: Each operator in the execution plan reports detailed metrics including:
  - Execution time (CPU time, wall-clock time)
  - Data volume (rows processed, bytes scanned)
  - Memory usage
  - Network I/O
  - Operator-specific metrics (e.g., hash table build time, join selectivity)

- **Multi-Node Visibility**: Query Profile aggregates execution information from all BE nodes involved in query execution, providing a complete picture of distributed query performance

- **Runtime Profile**: For long-running queries, Runtime Query Profile (v3.1+) provides real-time insights by reporting profile data at configurable intervals (default: 10 seconds)

- **Visualization Support**: StarRocks Enterprise Edition provides interactive visualization tools for query profiles, making it easier to identify bottlenecks and understand execution flow

### Enabling Query Profile

```sql
-- Enable Query Profile globally
SET GLOBAL enable_profile = true;

-- Enable only for slow queries (recommended for production)
SET GLOBAL big_query_profile_threshold = '30s';

-- Configure runtime profile reporting interval
SET runtime_profile_report_interval = 30;
```

### Accessing Query Profiles

**Via Web UI:**

1. Navigate to `http://<fe_ip>:<fe_http_port>`
2. Click **queries** in the top navigation
3. Select a query from the **Finished Queries** list
4. Click the **Profile** link to view detailed execution information

**Via SQL Functions:**
```sql
-- Get the last query ID
SELECT last_query_id();

-- List recent queries
SHOW PROFILELIST;

-- Get detailed profile for a specific query
SELECT get_query_profile('019b364f-10c4-704c-b79a-af2cc3a77b89')\G
```

**Via HTTP API:**
```bash
curl http://<fe_ip>:<fe_http_port>/api/profile?query_id=<query_id>
```

### Profile Structure

Query Profile is organized hierarchically, mirroring the execution plan structure:

- **Summary Metrics**: Total execution time, query state, query ID, SQL statement, user, variables
- **Planner Metrics**: Time spent in query planning, optimization phases
- **Fragment Metrics**: Execution statistics for each plan fragment
- **Operator Metrics**: Detailed metrics for each operator (Scan, Join, Aggregate, Exchange, etc.)

### Use Cases

- **Performance Tuning**: Identify slow operators, understand data skew, and optimize query plans
- **Resource Optimization**: Analyze memory usage, CPU consumption, and I/O patterns
- **Bottleneck Detection**: Quickly identify which operators are consuming the most time or resources
- **Query Plan Validation**: Verify that the optimizer is generating efficient execution plans

## 3. Prometheus and Grafana Integration

### Overview

StarRocks exposes comprehensive metrics through Prometheus-compatible endpoints, enabling integration with standard monitoring stacks. Combined with Grafana dashboards, this provides real-time visibility into cluster health, performance, and resource utilization.

### Metrics Endpoints

**FE Metrics:**

- Endpoint: `http://<fe_ip>:<fe_http_port>/metrics`
- Formats: Prometheus (default), JSON (`?type=json`), Core (`?type=core`)

**BE Metrics:**

- Endpoint: `http://<be_ip>:<be_http_port>/metrics`
- Formats: Prometheus (default), JSON (`?type=json`), Core (`?type=core`)

### Key Metric Categories

**Cluster Health:**

- Node status and availability
- Replication status
- Tablet health

**Query Performance:**

- Query QPS and latency
- Query queue length
- Query success/failure rates

**Resource Utilization:**

- CPU usage
- Memory consumption
- Disk I/O (read/write throughput)
- Network I/O

**Storage Metrics:**

- Compaction rates (base and cumulative)
- Data ingestion rates
- Storage capacity and utilization

**Load Operations:**

- Stream Load throughput
- Broker Load progress
- Routine Load status

### Grafana Dashboards

StarRocks provides pre-built Grafana dashboard templates that visualize:

- Cluster overview and health
- Query performance trends
- Resource utilization
- Storage and compaction metrics
- Load operation status

### Alert Configuration

Configure Prometheus alert rules to notify on:

- Node failures or unavailability
- High query latency
- Resource exhaustion (CPU, memory, disk)
- Replication failures
- Compaction failures

## 4. Comprehensive Logging System

### Log File Types

StarRocks maintains several specialized log files for different purposes:

**FE Logs:**

- `fe.log`: Main FE log with startup, cluster state, DML/DQL requests, and scheduling information
- `fe.warn.log`: Warnings and errors only, useful for quick issue identification
- `fe.audit.log`: Detailed audit logs for all SQL statements (used by AuditLoader)
- `fe.big_query.log`: Dedicated log for high-resource-consumption queries
- `fe.dump.log`: Query dump logs for detailed debugging (enabled via `enable_query_dump`)

**BE Logs:**

- `be.INFO`: Main BE log with execution details, compaction, and replication information
- `be.WARNING`: Warnings and errors from BE nodes
- `be.out`: Standard output and error messages

### Log Configuration

**FE Log Settings:**

- `sys_log_dir`: Log storage directory (default: `${STARROCKS_HOME}/log`)
- `sys_log_level`: Log level (default: `INFO`)
- `sys_log_roll_num`: Number of retained log files (default: 10)
- `sys_log_roll_interval`: Rotation frequency (default: `DAY`)
- `sys_log_roll_mode`: Rotation mode (default: `SIZE-MB-1024`)
- `sys_log_enable_compress`: Enable log compression (default: false)
- `sys_log_format`: Log format layout (default: `"plaintext"`). Valid values: `"plaintext"` or `"json"` (case-insensitive). When set to `"plaintext"`, logs use human-readable PatternLayout with timestamps, level, thread, class.method:line, and stack traces. When set to `"json"`, logs use JsonTemplateLayout emitting structured JSON events (UTC timestamps, level, thread id/name, source file/method/line, message, exception stackTrace) suitable for log aggregators (ELK, Splunk). This setting applies to all FE system logs including `fe.log`, `fe.warn.log`, `fe.audit.log`, `fe.big_query.log`, `fe.dump.log`, and `fe.profile.log`. Note: This setting is not mutable and requires FE restart to take effect.
- `sys_log_json_max_string_length`: Maximum string length for JSON-formatted system logs in bytes (default: 1048576 = 1MB). When `sys_log_format` is set to `"json"`, string-valued fields (e.g., "message" and exception stack traces) are truncated if their length exceeds this limit. Applies to default, warning, audit, dump, and bigquery layouts. Profile logs use a separate configuration (`sys_log_json_profile_max_string_length`).
- `sys_log_json_profile_max_string_length`: Maximum string length for JSON-formatted profile logs in bytes (default: 104857600 = 100MB). When `sys_log_format` is set to `"json"`, this controls the maxStringLength for profile and features log appenders. String field values in JSON-formatted profile logs will be truncated to this byte length; non-string fields are unaffected. This setting is ignored when `plaintext` logging is used.

**Audit Log Settings:**

- `audit_log_dir`: Audit log directory
- `audit_log_modules`: Types of operations to log
- `audit_log_json_format`: Log in JSON format (default: false)

### Log Analysis

Logs can be analyzed using:

- Standard text processing tools (grep, awk, sed) - best suited for `plaintext` format
- Log aggregation systems (ELK stack, Splunk, Loki, etc.) - recommended when `sys_log_format` is set to `"json"` for structured log ingestion and querying
- StarRocks HTTP interface: `GET /api/get_log_file` for remote log access
- BE grep interface: `GET /greplog` for searching BE logs

**Note:** When `sys_log_format` is set to `"json"`, logs are emitted as structured JSON events, making them ideal for integration with log aggregation and analysis platforms. The JSON format includes standardized fields (timestamp, level, thread, source location, message, exception) that enable efficient filtering, searching, and correlation across log entries.

## 5. Query Trace Profile

### Overview

**Query Trace Profile** (v3.2.0+) provides detailed debugging information for query execution, including time costs, variable values, and log records. This feature is particularly useful for deep-dive debugging and understanding query optimization behavior.

### Trace Modules

- `BASE`: Base module with fundamental query processing information
- `MV`: Materialized view module, showing MV selection and rewriting
- `OPTIMIZER`: Optimizer module with detailed optimization steps
- `SCHEDULE`: Scheduling module with fragment deployment and execution coordination
- `EXTERNAL`: External table-related module for connector operations

### Usage

```sql
-- Trace time costs for optimizer module
TRACE TIMES OPTIMIZER SELECT * FROM t1 JOIN t2 ON t1.v1 = t2.v1;

-- Trace variable values
TRACE VALUES SELECT * FROM t1;

-- Trace log records
TRACE LOGS SELECT * FROM t1;

-- Trace all information
TRACE ALL OPTIMIZER SELECT * FROM t1 JOIN t2 ON t1.v1 = t2.v1;
```

### Use Cases

- **Optimizer Debugging**: Understand why the optimizer makes specific decisions
- **Materialized View Analysis**: Verify MV selection and rewriting behavior
- **Performance Investigation**: Correlate time costs with optimization phases
- **Issue Reproduction**: Capture complete query context for troubleshooting

## 6. HTTP API Interfaces

StarRocks provides extensive HTTP APIs for observability:

**Query Information:**

- `GET /api/profile?query_id={}`: Get query profile
- `GET /api/query_detail`: Get query details
- `GET /api/connection`: List active connections

**Metrics:**

- `GET /metrics`: Get Prometheus metrics (FE and BE)
- `GET /metrics?type=json`: Get metrics in JSON format
- `GET /metrics?type=core`: Get core metrics only

**System Information:**

- `GET /api/show_proc`: Show process information
- `GET /api/show_runtime_info`: Show runtime information
- `GET /api/show_meta_info`: Show metadata information
- `GET /varz`: View current configuration (BE)

**Logs:**

- `HEAD/GET /api/get_log_file`: Retrieve log files
- `GET /greplog`: Search BE logs

**Health Checks:**

- `GET /api/health`: Health check endpoint

## 7. Best Practices for Observability

### Production Recommendations

1. **Enable AuditLoader**: Set up AuditLoader to centralize audit log analysis and enable SQL-based query pattern analysis

2. **Selective Query Profiling**: Use `big_query_profile_threshold` to profile only slow queries in production, reducing overhead while maintaining visibility into performance issues

3. **Prometheus Monitoring**: Deploy Prometheus and Grafana for real-time cluster monitoring and alerting

4. **Log Retention**: Configure appropriate log retention policies to balance observability needs with storage costs

5. **Structured Logging**: Enable JSON format for audit logs when integrating with log aggregation systems

6. **Dashboard Customization**: Customize Grafana dashboards to focus on metrics relevant to your workload

7. **Alert Tuning**: Configure alerts for critical issues (node failures, high latency, resource exhaustion) while avoiding alert fatigue

### Observability Workflow

1. **Proactive Monitoring**: Use Prometheus/Grafana to monitor cluster health and performance trends
2. **Anomaly Detection**: Set up alerts for unusual patterns or threshold violations
3. **Query Analysis**: Use Query Profile to analyze slow or problematic queries
4. **Audit Investigation**: Query audit logs to understand query patterns and user behavior
5. **Deep Debugging**: Use Query Trace Profile for detailed investigation of optimization and execution behavior
6. **Log Analysis**: Correlate logs with metrics and profiles to understand root causes

## 8. Integration with External Systems

### Log Aggregation

StarRocks logs can be integrated with:

- **ELK Stack** (Elasticsearch, Logstash, Kibana)
- **Splunk**
- **Fluentd/Fluent Bit**
- **Loki** (Grafana's log aggregation system)

### Metrics Integration

Prometheus metrics can be forwarded to:

- **Grafana Cloud**
- **Datadog** (via Prometheus endpoint)
- **New Relic** (via Prometheus remote write)
- **Custom monitoring systems**

### Audit Log Analysis

Audit logs loaded via AuditLoader can be:

- Analyzed using standard SQL queries
- Exported to external analytics systems
- Integrated with business intelligence tools
- Used for compliance reporting

## Conclusion

StarRocks provides a comprehensive observability ecosystem that enables effective monitoring, debugging, and optimization of database clusters. By leveraging AuditLoader for centralized audit log management, Query Profile for deep performance analysis, Prometheus/Grafana for real-time monitoring, and the extensive logging and tracing capabilities, administrators can maintain healthy, performant clusters and quickly identify and resolve issues.

The key to effective observability is understanding which tools to use for different scenarios:

- **AuditLoader**: For query pattern analysis, security auditing, and capacity planning
- **Query Profile**: For performance tuning and bottleneck identification
- **Prometheus/Grafana**: For real-time monitoring and alerting
- **Logs**: For troubleshooting and detailed investigation
- **Query Trace Profile**: For deep debugging of optimization and execution behavior

By combining these observability components, you can build a complete picture of your StarRocks cluster's behavior and performance, enabling data-driven optimization and reliable operations at scale.

