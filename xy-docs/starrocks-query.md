---
title: StarRocks Query Execution
---

> ⚠️ WIP

## 1. Query Lifecycle Overview

The query lifecycle spans both Frontend (FE) and Backend (BE) components, with FE handling query planning and coordination, and BE executing the physical query plan.

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         Client Connection                               │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  MySQL Protocol / HTTP / Arrow Flight                             │  │
│  │  - SQL string transmission                                        │  │
│  │  - Session management                                             │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼──────────────────────────────────────────────────┘
                       │ SQL String
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Frontend (FE) - Query Planning and Coordination      │
│                                                                         │
│  1. Connection Handling:                                                │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  ConnectProcessor (MySQL Protocol)                             │  │
│     │  ├─> Accepts client connections                                │  │
│     │  ├─> Extracts SQL string from packets                          │  │
│     │  ├─> Creates ConnectContext (session state)                    │  │
│     │  └─> Delegates to StmtExecutor                                 │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  2. SQL Parsing:                                                        │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  SqlParser.parse()                                             │  │
│     │  ├─> Tokenizes SQL text                                        │  │
│     │  ├─> Parses with ANTLR grammar                                 │  │
│     │  └─> Builds AST (Abstract Syntax Tree)                         │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  3. Semantic Analysis:                                                  │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  Analyzer.analyze()                                            │  │
│     │  ├─> Resolves table/column references                          │  │
│     │  ├─> Type checking and inference                               │  │
│     │  ├─> Function resolution                                       │  │
│     │  └─> Validates SQL semantics                                   │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  4. Query Planning:                                                     │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  StatementPlanner.plan()                                       │  │
│     │  ├─> Transforms AST to Logical Plan                            │  │
│     │  ├─> Optimizes with CBO (Cost-Based Optimizer)                 │  │
│     │  ├─> Transforms Logical Plan to Physical Plan                  │  │
│     │  ├─> Builds Plan Fragments                                     │  │
│     │  └─> Creates ExecPlan                                          │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  5. Query Coordination:                                                 │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  Coordinator (DefaultCoordinator)                              │  │
│     │  ├─> Schedules fragments to BE nodes                           │  │
│     │  ├─> Deploys fragments via bRPC                                │  │
│     │  ├─> Monitors execution progress                               │  │
│     │  ├─> Collects results from BE nodes                            │  │
│     │  └─> Delivers results to client                                │  │
│     └────────────────────────────────────────────────────────────────┘  │
└─────────────────────────┼───────────────────────────────────────────────┘
                          │ Plan Fragments (bRPC)
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Backend (BE) - Query Execution                       │
│                                                                         │
│  1. Fragment Reception:                                                 │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  PInternalService.exec_plan_fragment()                         │  │
│     │  ├─> Receives TExecPlanFragmentParams                          │  │
│     │  ├─> Deserializes plan fragment                                │  │
│     │  └─> Creates PlanFragmentExecutor                              │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  2. Pipeline Execution:                                                 │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  Pipeline Execution Engine                                     │  │
│     │  ├─> Instantiates operators                                    │  │
│     │  ├─> Schedules drivers (parallelism)                           │  │
│     │  ├─> Executes vectorized operators                             │  │
│     │  └─> Exchanges data between fragments                          │  │
│     └────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  3. Result Delivery:                                                    │
│     ┌────────────────────────────────────────────────────────────────┐  │
│     │  ResultBufferMgr                                               │  │
│     │  ├─> Buffers query results                                     │  │
│     │  ├─> Serializes results (Thrift/JSON)                          │  │
│     │  └─> Returns via fetch_data RPC                                │  │
│     └────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

## 2. SQL Parsing

StarRocks uses ANTLR (ANother Tool for Language Recognition) to parse SQL statements, transforming raw SQL text into structured Abstract Syntax Trees (AST) that can be analyzed and optimized. The parser supports both StarRocks SQL dialect and Trino SQL dialect, enabling compatibility with different query interfaces.

**Parsing Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                         SQL Text Input                                  │
│  "SELECT col1, col2 FROM table WHERE col1 > 100"                         │
└───────────────────┬───────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    SqlParser.parse()                                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Dialect Selection                                                │  │
│  │  ├─> StarRocks Dialect (default)                                 │  │
│  │  └─> Trino Dialect (if sql_dialect='trino')                      │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                      │
        ┌─────────────┴─────────────┐
        │                           │
        ▼                           ▼
┌───────────────────┐    ┌──────────────────────┐
│  StarRocks Parser │    │   Trino Parser       │
│  ┌───────────────┐│    │  ┌─────────────────┐ │
│  │ ANTLR Lexer   ││    │  │ Trino Parser    │ │
│  │ - Tokenizes    ││    │  │ (Trino library) │ │
│  │ - Keywords     ││    │  └─────────────────┘ │
│  │ - Identifiers  ││    │                      │
│  └───────┬───────┘│    └──────────────────────┘
│          │        │
│  ┌───────▼───────┐│
│  │ ANTLR Parser   ││
│  │ - Grammar rules││
│  │ - AST building ││
│  └───────┬───────┘│
└──────────┼────────┘
           │
           ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    AST (Abstract Syntax Tree)                           │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  StatementBase (root)                                              │  │
│  │  ├─> QueryStatement                                                │  │
│  │  │   ├─> SelectList                                                │  │
│  │  │   ├─> TableRelation                                             │  │
│  │  │   └─> Predicate                                                 │  │
│  │  ├─> InsertStmt                                                    │  │
│  │  ├─> CreateTableStmt                                               │  │
│  │  └─> ... (other statement types)                                   │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Key Components:**

- **ANTLR Grammar Files** (`fe/fe-grammar/`): Define the syntax rules for StarRocks SQL. Grammar files specify how SQL keywords, identifiers, expressions, and statements are structured. The grammar supports StarRocks-specific extensions like materialized views, table functions, and advanced aggregation functions.

- **SqlParser** (`fe/fe-core/src/main/java/com/starrocks/sql/parser/SqlParser.java`): Main entry point for SQL parsing. Handles dialect selection, statement splitting, and delegates to appropriate parser implementations. Supports parsing single statements or multi-statement batches.

- **AstBuilder** (`fe/fe-parser/`): Transforms ANTLR parse trees into StarRocks AST nodes. Each AST node type corresponds to a SQL construct (SELECT, FROM, WHERE, JOIN, etc.). The builder validates syntax and constructs type-safe AST representations.

- **Parser Error Handling**: The parser provides detailed error messages with line numbers and column positions when syntax errors are detected. Error recovery mechanisms attempt to continue parsing after errors when possible.

**Parsing Process:**

1. **Tokenization**: The ANTLR lexer scans the SQL text and identifies tokens (keywords, identifiers, literals, operators). Tokens are classified based on grammar rules.

2. **Parsing**: The ANTLR parser applies grammar rules to token sequences, building a parse tree that represents the syntactic structure of the SQL statement.

3. **AST Construction**: The `AstBuilder` traverses the parse tree and constructs StarRocks AST nodes. Each AST node contains semantic information extracted from the parse tree.

4. **Statement Splitting**: For multi-statement queries (semicolon-separated), the parser splits the input into individual statements and parses each separately.

5. **Error Reporting**: Syntax errors are reported with precise location information, enabling clients to display helpful error messages.

**StarRocks-Specific Extensions:**

The parser supports StarRocks-specific SQL extensions including:
- Materialized view queries and maintenance
- Table functions (unnest, json_each, etc.)
- Advanced window functions
- Query hints and optimizer directives
- Resource group assignments
- Query result formats (JSON, Arrow, etc.)

**Key Files to Reference:**

- `fe/fe-grammar/` - ANTLR grammar files defining SQL syntax
- `fe/fe-parser/` - Parser implementation and AST builders
- `fe/fe-core/src/main/java/com/starrocks/sql/parser/SqlParser.java` - Main parser entry point
- `fe/fe-core/src/main/java/com/starrocks/sql/ast/` - AST node definitions

## 3. Semantic Analysis and Type System

After parsing, StarRocks performs semantic analysis to validate SQL semantics, resolve references, and infer types. This stage ensures that the query is meaningful and can be executed, transforming the AST into a semantically validated representation ready for optimization.

**Semantic Analysis Pipeline:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    AST (from Parser)                                    │
└───────────────────┬───────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Analyzer.analyze()                                    │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  1. Scope Resolution                                               │  │
│  │     ├─> Resolves database/catalog references                       │  │
│  │     ├─> Resolves table references                                   │  │
│  │     └─> Resolves column references                                  │  │
│  │                                                                     │  │
│  │  2. Type Checking                                                  │  │
│  │     ├─> Validates expression types                                 │  │
│  │     ├─> Performs type inference                                    │  │
│  │     ├─> Applies implicit type coercion                             │  │
│  │     └─> Validates function argument types                          │  │
│  │                                                                     │  │
│  │  3. Function Resolution                                            │  │
│  │     ├─> Resolves function names to implementations                │  │
│  │     ├─> Selects overloaded function variants                       │  │
│  │     └─> Validates function signatures                               │  │
│  │                                                                     │  │
│  │  4. Semantic Validation                                            │  │
│  │     ├─> Validates GROUP BY clauses                                 │  │
│  │     ├─> Validates HAVING predicates                                │  │
│  │     ├─> Validates window function usage                            │  │
│  │     └─> Validates aggregate function usage                          │  │
│  │                                                                     │  │
│  │  5. View and Policy Rewriting                                       │  │
│  │     ├─> Expands view definitions                                   │  │
│  │     ├─> Applies row-level security policies                        │  │
│  │     └─> Rewrites queries with materialized views                   │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└───────────────────────┼───────────────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Analyzed AST (QueryRelation)                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  - All references resolved                                         │  │
│  │  - Types inferred and validated                                     │  │
│  │  - Functions resolved                                              │  │
│  │  - Ready for logical plan transformation                          │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Type System:**

StarRocks supports a rich type system including:

- **Primitive Types**: `BOOLEAN`, `TINYINT`, `SMALLINT`, `INT`, `BIGINT`, `LARGEINT`, `FLOAT`, `DOUBLE`, `DECIMAL`, `DATE`, `DATETIME`, `CHAR`, `VARCHAR`, `STRING`, `BINARY`, `JSON`

- **Complex Types**: `ARRAY<T>`, `MAP<K, V>`, `STRUCT<...>`

- **Type Inference**: The analyzer infers types for expressions based on operator rules and function signatures. For example, `INT + INT` infers `INT`, while `INT + DOUBLE` infers `DOUBLE` with implicit coercion.

- **Type Coercion**: Implicit type conversions are applied when necessary (e.g., `INT` to `BIGINT` for arithmetic operations). Coercion rules follow SQL standard semantics.

**Column and Table Resolution:**

- **Catalog Resolution**: Queries may specify catalogs (e.g., `catalog.database.table`). The analyzer resolves catalog names to catalog implementations (Hive, Iceberg, etc.).

- **Database Resolution**: Table references are resolved within the current database context or explicitly specified databases. The analyzer validates database existence and access permissions.

- **Table Resolution**: Table names are resolved to table metadata, including schema information, partition definitions, and table properties. The analyzer handles table aliases and validates table existence.

- **Column Resolution**: Column references are resolved to table columns, considering table aliases, qualified names (`table.column`), and scope rules. The analyzer validates column existence and access permissions.

**Function Resolution:**

- **Built-in Functions**: StarRocks provides hundreds of built-in functions (aggregate, scalar, window, table functions). The analyzer resolves function names to implementations and selects appropriate overloaded variants based on argument types.

- **UDF Resolution**: User-defined functions (UDFs) are resolved from the function registry. The analyzer validates UDF signatures and availability.

- **Overload Selection**: When multiple function variants exist, the analyzer selects the best match based on argument types, preferring exact matches over coercions.

**Key Files to Reference:**

- `fe/fe-type/` - Type system definitions and type checking logic
- `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/` - Semantic analysis implementation
- `fe/fe-core/src/main/java/com/starrocks/catalog/` - Catalog and metadata management
- `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/Analyzer.java` - Main analyzer entry point

## 4. Query Planning: Logical to Physical Plan Transformation

StarRocks uses a cost-based optimizer (CBO) to transform semantically analyzed queries into optimal execution plans. The planning process converts logical query representations into physical execution plans that specify how data will be accessed, joined, aggregated, and distributed across the cluster.

**Planning Pipeline:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Analyzed Query (QueryRelation)                       │
└───────────────────┬───────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Logical Plan Construction                             │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  RelationTransformer                                                │  │
│  │  ├─> Transforms QueryRelation to LogicalPlan                        │  │
│  │  ├─> Creates logical operators:                                     │  │
│  │  │   ├─> LogicalScanOperator (table access)                          │  │
│  │  │   ├─> LogicalJoinOperator (joins)                                │  │
│  │  │   ├─> LogicalAggregationOperator (grouping)                       │  │
│  │  │   ├─> LogicalProjectOperator (column selection)                  │  │
│  │  │   └─> LogicalFilterOperator (predicates)                          │  │
│  │  └─> Builds operator tree                                           │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Cost-Based Optimization                               │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Optimizer.optimize()                                               │  │
│  │  ├─> Rule-Based Transformations:                                   │  │
│  │  │   ├─> Predicate Pushdown                                         │  │
│  │  │   ├─> Projection Pushdown                                       │  │
│  │  │   ├─> Join Reordering                                           │  │
│  │  │   ├─> Subquery Unnesting                                        │  │
│  │  │   └─> Materialized View Rewriting                               │  │
│  │  │                                                                  │  │
│  │  ├─> Cost Estimation:                                               │  │
│  │  │   ├─> Row count estimation                                      │  │
│  │  │   ├─> Cardinality estimation                                    │  │
│  │  │   ├─> Selectivity estimation                                   │  │
│  │  │   └─> Cost calculation (CPU, memory, I/O)                      │  │
│  │  │                                                                  │  │
│  │  ├─> Physical Plan Generation:                                     │  │
│  │  │   ├─> PhysicalScanOperator (OlapScan, HiveScan, etc.)           │  │
│  │  │   ├─> PhysicalHashJoinOperator / PhysicalNestLoopJoinOperator    │  │
│  │  │   ├─> PhysicalHashAggregateOperator                             │  │
│  │  │   └─> PhysicalExchangeOperator (data shuffling)                 │  │
│  │  │                                                                  │  │
│  │  └─> Plan Selection:                                               │  │
│  │      └─> Selects lowest-cost plan from alternatives               │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Physical Plan (OptExpression)                        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  - Physical operators with execution strategies                     │  │
│  │  - Data distribution requirements                                   │  │
│  │  - Resource requirements                                             │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Plan Fragment Construction                           │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  PlanFragmentBuilder.createPhysicalPlan()                           │  │
│  │  ├─> Splits plan into fragments                                     │  │
│  │  ├─> Assigns fragments to BE nodes                                  │  │
│  │  ├─> Adds ExchangeNodes for data shuffling                          │  │
│  │  ├─> Specifies data distribution (hash, broadcast, range)            │  │
│  │  └─> Creates ExecPlan with fragment list                           │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    ExecPlan (Ready for Deployment)                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  - List of PlanFragments                                           │  │
│  │  - Fragment instance assignments                                   │  │
│  │  - Data exchange specifications                                    │  │
│  │  - Result sink configuration                                       │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Logical Plan:**

The logical plan represents the query's logical structure without specifying execution details. Logical operators include:

- **LogicalScanOperator**: Represents table access without specifying scan method
- **LogicalJoinOperator**: Represents joins without specifying join algorithm
- **LogicalAggregationOperator**: Represents grouping and aggregation
- **LogicalProjectOperator**: Represents column selection and expressions
- **LogicalFilterOperator**: Represents predicate filtering

**Cost-Based Optimization:**

The optimizer explores alternative execution plans and selects the optimal one based on cost estimates:

- **Rule-Based Transformations**: Applies optimization rules (predicate pushdown, join reordering, etc.) to generate alternative logical plans.

- **Cost Estimation**: Estimates execution costs using statistics (row counts, cardinality, selectivity). Costs include CPU, memory, and I/O components.

- **Physical Plan Generation**: Transforms logical operators into physical operators with specific execution strategies (hash join vs. nested loop join, hash aggregation vs. streaming aggregation).

- **Plan Selection**: Selects the lowest-cost plan from alternatives using dynamic programming and memoization techniques.

**Physical Plan:**

The physical plan specifies execution strategies:

- **PhysicalScanOperator**: Specifies scan methods (OlapScan for StarRocks tables, HiveScan for Hive tables, etc.)

- **PhysicalHashJoinOperator**: Hash join implementation with build and probe phases

- **PhysicalNestLoopJoinOperator**: Nested loop join for small tables

- **PhysicalHashAggregateOperator**: Hash-based aggregation with grouping

- **PhysicalExchangeOperator**: Data shuffling between nodes (hash distribution, broadcast, range distribution)

**Plan Fragments:**

The physical plan is split into plan fragments that can be executed independently on different BE nodes:

- **Fragment Boundaries**: Fragments are separated by `ExchangeNode` operators that require data shuffling.

- **Fragment Assignment**: Fragments are assigned to BE nodes based on data locality (for scan fragments) and load balancing.

- **Data Distribution**: Exchange nodes specify how data is distributed (hash partitioning for joins, broadcast for small tables, range partitioning for sorting).

**Key Files to Reference:**

- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/` - Optimizer implementation
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/operator/` - Logical and physical operator definitions
- `fe/fe-core/src/main/java/com/starrocks/planner/` - Plan fragment construction
- `fe/fe-core/src/main/java/com/starrocks/sql/StatementPlanner.java` - Main planning entry point
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/QueryOptimizer.java` - Cost-based optimizer

## 5. Vectorized Execution: The Heart of StarRocks Performance

StarRocks implements a fully vectorized execution engine that processes data in columnar batches (chunks) rather than row-by-row, achieving 3-10x performance improvements over traditional row-based execution. Vectorization leverages CPU cache locality, SIMD instructions, and reduced function call overhead to maximize query throughput.

**Vectorization Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Row-Based Execution (Traditional)                    │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  for each row:                                                     │  │
│  │    evaluate predicate(row)                                        │  │
│  │    compute expression(row)                                         │  │
│  │    aggregate(row)                                                  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Problems:                                                      │  │  │
│  │  │  - High function call overhead                                 │  │  │
│  │  │  - Poor CPU cache utilization                                   │  │  │
│  │  │  - Branch mispredictions                                        │  │  │
│  │  │  - No SIMD utilization                                         │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    Vectorized Execution (StarRocks)                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Chunk (Columnar Batch)                                            │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Column 1: [val1, val2, val3, ..., valN] (contiguous)        │  │  │
│  │  │  Column 2: [val1, val2, val3, ..., valN] (contiguous)        │  │  │
│  │  │  Column 3: [val1, val2, val3, ..., valN] (contiguous)        │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                     │  │
│  │  Vectorized Operations:                                           │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  evaluate_predicate(chunk) → filter vector                    │  │  │
│  │  │  compute_expression(chunk) → result column                   │  │  │
│  │  │  aggregate(chunk) → aggregated values                         │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                     │  │
│  │  Benefits:                                                         │  │
│  │  ├─> Process 1024+ rows per function call                         │  │
│  │  ├─> Better CPU cache utilization                                 │  │
│  │  ├─> SIMD instruction utilization                                  │  │
│  │  └─> Reduced branch mispredictions                                 │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Columnar Data Structures:**

- **Chunk**: A batch of rows stored in columnar format. Each chunk contains multiple columns, with each column storing values for all rows in the chunk. Chunks typically contain 1024-4096 rows.

- **Column**: A contiguous array of values of the same type. Columns can be:
    - **Fixed-length columns**: Integers, floats, fixed-length strings
    - **Variable-length columns**: Variable-length strings, binary data
    - **Nullable columns**: Include a null bitmap for null value tracking
    - **Constant columns**: Single value repeated for all rows (optimization)

- **Column Layout**: Column data is stored contiguously in memory, enabling efficient SIMD operations and cache-friendly access patterns.

**Vectorized Operations:**

- **Predicate Evaluation**: Predicates are evaluated on entire chunks, producing filter vectors (bitmaps) indicating which rows pass the predicate. Filter vectors enable efficient row filtering without materializing intermediate results.

- **Expression Evaluation**: Expressions are evaluated column-by-column, processing entire columns at once. This enables SIMD optimizations for arithmetic operations.

- **Aggregation**: Aggregations process chunks of data, updating aggregate state incrementally. Hash-based aggregations use vectorized hash table operations.

- **Joins**: Hash joins use vectorized hash table lookups and probe operations, processing multiple rows per iteration.

**SIMD Optimizations:**

StarRocks leverages SIMD (Single Instruction, Multiple Data) instructions to process multiple values simultaneously:

- **AVX2/AVX-512**: Uses CPU vector instructions to perform arithmetic operations on 4-8 values at once (depending on data type and CPU capabilities).

- **SIMD Utilities** (`be/src/simd/`): Provides SIMD-optimized implementations for common operations (comparisons, arithmetic, filtering).

- **Automatic SIMD**: The compiler and runtime automatically utilize SIMD when processing contiguous column data.

**Performance Benefits:**

- **3-10x Speedup**: Vectorized execution typically achieves 3-10x performance improvements over row-based execution for analytical workloads.

- **Reduced Function Call Overhead**: Processing batches of rows reduces function call overhead from per-row to per-chunk.

- **Better Cache Utilization**: Columnar layout improves CPU cache hit rates by accessing contiguous memory.

- **Reduced Branch Mispredictions**: Batch processing reduces branch misprediction penalties.

**Key Files to Reference:**

- `be/src/column/` - Column data structure implementations
- `be/src/column/chunk.h` - Chunk definition and operations
- `be/src/exprs/binary_function.h` - Vectorized binary operations
- `be/src/exec/` - Vectorized operator implementations
- `be/src/simd/` - SIMD utility functions

## 6. The Pipeline Execution Engine

StarRocks uses a pipeline-based execution engine that processes queries as a series of connected operators, enabling fine-grained parallelism and efficient resource utilization. The pipeline engine replaces the traditional volcano-style iterator model with a more modern execution model that supports backpressure, flow control, and dynamic parallelism.

**Pipeline vs. Non-Pipeline Execution:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Non-Pipeline (Volcano Model)                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Operator Tree:                                                   │  │
│  │                                                                   │  │
│  │        Root                                                       │  │
│  │         │                                                         │  │
│  │      Join                                                         │  │
│  │      /   \                                                        │  │
│  │   Scan  Scan                                                      │  │
│  │                                                                   │  │
│  │  Execution:                                                       │  │
│  │  - Pull-based: Root pulls from Join, Join pulls from Scans        │  │
│  │  - Blocking: Operators block until all input is available         │  │
│  │  - Limited parallelism: Only one operator active at a time        │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    Pipeline Execution Model                             │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Pipeline:                                                        │  │
│  │                                                                   │  │
│  │  Scan → Filter → Join → Aggregate → Sink                          │  │
│  │   │       │       │         │          │                          │  │
│  │   └───────┴───────┴─────────┴──────────┘                          │  │
│  │                                                                   │  │
│  │  Execution:                                                       │  │
│  │  - Driver-driven: Driver pulls from each operator, pushes to next │  │
│  │  - Non-blocking: Operators process chunks as they arrive          │  │
│  │  - Parallelism: Multiple operators active simultaneously          │  │
│  │  - Backpressure: need_input()/has_output() gate chunk movement    │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Pipeline Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Pipeline Execution                                   │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Pipeline                                                         │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Operator 1 → Operator 2 → Operator 3 → ... → Operator N    │  │  │
│  │  │  (Source)     (Filter)     (Join)            (Sink)         │  │  │
│  │  └───────────────────┬─────────────────────────────────────────┘  │  │
│  │                      │                                            │  │
│  │  Driver (Execution Unit)                                          │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  - Executes pipeline operators sequentially                 │  │  │
│  │  │  - Processes chunks through pipeline                        │  │  │
│  │  │  - Manages operator state                                   │  │  │
│  │  │  - Handles backpressure and flow control                    │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                   │  │
│  │  Parallelism:                                                     │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Driver 1: Pipeline instance 1                              │  │  │
│  │  │  Driver 2: Pipeline instance 2                              │  │  │
│  │  │  Driver 3: Pipeline instance 3                              │  │  │
│  │  │  ... (pipeline_dop instances)                               │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Pipeline Components:**

- **Pipeline**: A sequence of connected operators that process data in a streaming fashion. Pipelines are created from plan fragments, with each pipeline representing a portion of the query execution plan.

- **Operator**: A processing unit that transforms chunks of data. Operators do not call each other; they implement callbacks used by the driver:
    - `Operator::pull_chunk()`: Produces a chunk when the driver asks (used for upstream)
    - `Operator::push_chunk()`: Consumes a chunk when the driver delivers one (used for downstream)
    - `Operator::has_output()`: Indicates if the operator has data ready for pull_chunk
    - `Operator::need_input()`: Indicates if the operator can accept input via push_chunk

- **Driver**: An execution unit that runs a pipeline instance. The driver moves data by pulling from each operator and pushing to the next: it calls `curr_op->pull_chunk()` then `next_op->push_chunk()` for each consecutive pair, only when `curr_op->has_output()` and `next_op->need_input()`. Multiple drivers can execute the same pipeline in parallel (pipeline parallelism).

- **Driver Queue**: A queue of ready drivers waiting for execution. The scheduler selects drivers from the queue and executes them on worker threads.

**Pipeline Execution Flow:**

1. **Pipeline Creation**: Plan fragments are converted into pipelines, with each pipeline containing a sequence of operators.

2. **Driver Instantiation**: For each pipeline, multiple drivers are created based on the degree of parallelism (`pipeline_dop`). Each driver executes an independent instance of the pipeline.

3. **Operator Execution**: Drivers execute operators in sequence:
    - Source operators produce chunks
    - Intermediate operators transform chunks
    - Sink operators consume chunks (send to network, write to storage, etc.)

4. **Chunk Flow**: The driver moves chunks along the pipeline. For each consecutive operator pair, it pulls a chunk from the current operator (`pull_chunk`) and pushes it to the next (`push_chunk`). Operators never call each other; they only respond to these driver calls.

5. **Backpressure**: The driver only moves a chunk when both `curr_op->has_output()` and `next_op->need_input()` are true. When downstream cannot accept more data, `need_input()` is false and the driver skips that pair, preventing memory exhaustion.

**Pipeline Scheduling:**

- **Event-Driven Scheduling**: Drivers are scheduled based on readiness (data availability, resource availability). The scheduler maintains a queue of ready drivers and executes them on worker threads.

- **Work Stealing**: When a worker thread becomes idle, it can steal work from other threads' queues, improving load balancing.

- **Resource Management**: The scheduler tracks resource usage (CPU, memory) and throttles execution when resources are constrained.

**Pipeline Parallelism:**

- **Pipeline-Level Parallelism**: Multiple drivers execute the same pipeline in parallel, processing different data partitions.

- **Operator-Level Parallelism**: Some operators (e.g., hash joins) can process multiple chunks concurrently within a single driver.

- **Fragment-Level Parallelism**: Different plan fragments execute in parallel on different BE nodes.

---

### Major Classes and Files

This subsection describes the main pipeline types, their roles, lifecycles, and where they live in the codebase.

#### Pipeline (`be/src/exec/pipeline/pipeline.h`)

- **Role**: A pipeline is a **chain of operator factories** (source → … → sink) that defines one logical execution path. It does not hold data; it defines how to create operators and drivers.
- **Key members**:
  - `_id`, `_op_factories`, `_drivers`, `_runtime_profile`
  - `_num_finished_drivers` / `_num_epoch_finished_drivers` for completion and stream MV epochs
- **Lifecycle**:
  - Created during fragment execution (e.g. from `PipelineBuilderContext` / plan).
  - `prepare()` / `close()` run on all operator factories.
  - `create_operators(degree_of_parallelism, i)` builds one set of operators per driver index.
  - `instantiate_drivers(state)` creates drivers from the pipeline and submits them to the executor.
  - When all drivers finish, the pipeline is done; `count_down_driver()` / `clear_drivers()` manage driver count and cleanup.
- **Other**: `source_operator_factory()` / `sink_operator_factory()` for the ends of the chain; `to_readable_string()` for debugging; STREAM MV uses `reset_epoch()` and `count_down_epoch_finished_driver()`.

#### PipelineDriver (`be/src/exec/pipeline/pipeline_driver.h`)

- **Role**: The **unit of execution**. One driver owns one list of operators (one instance of the pipeline) and is the only thing that runs on a worker thread; it moves chunks by calling `pull_chunk` / `push_chunk` on consecutive operators.
- **Key members**:
  - `_operators` (the chain), `_query_ctx`, `_fragment_ctx`, `_pipeline`, `_driver_id`, `_source_node_id`
  - `_state` (`DriverState`), `_driver_acct` (schedule/time stats for the queue), `_workgroup` (when workgroup scheduling is on)
  - Runtime filters / dependencies: `_local_rf_holders`, `_global_rf_descriptors`, `_dependencies`
- **DriverState** (simplified): `NOT_READY` → `READY` / `RUNNING` → blocking states (`INPUT_EMPTY`, `OUTPUT_FULL`, `PRECONDITION_BLOCK`, `PENDING_FINISH`, `LOCAL_WAITING`, etc.) → terminal (`FINISH`, `CANCELED`, `INTERNAL_ERROR`). Epoch states `EPOCH_PENDING_FINISH` / `EPOCH_FINISH` are used for stream MV.
- **Lifecycle**:
  - **Create**: Built from `Pipeline::create_operators()` plus context (query, fragment, pipeline); optionally bound to a `MorselQueue` for scan parallelism.
  - **Prepare**: `prepare(runtime_state)` and `prepare_local_state(runtime_state)` initialize operators and profile; state becomes `READY` (or `PRECONDITION_BLOCK` if waiting on runtime filters/dependencies).
  - **Execute**: Executor calls `process(runtime_state, worker_id)`. Driver runs a loop: for each consecutive operator pair, if `curr->has_output()` and `next->need_input()`, it pulls a chunk from `curr` and pushes to `next`; updates `DriverAcct`; yields (e.g. `READY`, `LOCAL_WAITING`) or blocks (e.g. `INPUT_EMPTY`, `OUTPUT_FULL`, `PRECONDITION_BLOCK`) and returns. When sink is finished, it marks operators finishing and returns `FINISH` or `PENDING_FINISH`.
  - **Finalize**: Executor calls `finalize(runtime_state, state)` (e.g. on `FINISH`/`CANCELED`/`INTERNAL_ERROR`), which closes operators and releases resources. Driver object may then be destroyed when no longer referenced.
- **Other**: Precondition and runtime-filter logic (`is_precondition_block()`, `mark_precondition_ready()`, `dependencies_block()`, `local_rf_block()`, `global_rf_block()`), reporting (`report_exec_state_if_necessary()`), workgroup and queue level (`workgroup()`, `set_driver_queue_level()`). See also `DriverAcct` in the same header (used by the driver queue for scheduling decisions).

#### DriverQueue (`be/src/exec/pipeline/pipeline_driver_queue.h`)

- **Role**: Holds **ready** drivers that worker threads take from. Decides order and priority (e.g. cancelled first, then level-based or workgroup fairness).
- **Interface** (abstract `DriverQueue`):
  - `put_back(driver)` / `put_back_from_executor(driver)` / `put_back(drivers)`: enqueue after yield or from executor.
  - `take(block)`: get next driver to run; may block if queue empty.
  - `cancel(driver)`: mark driver cancelled and give it priority to run so it can finalize.
  - `update_statistics(driver)`: update queue’s view of driver cost (used for scheduling).
  - `should_yield(driver, unaccounted_runtime_ns)`: used by workgroup scheduler to decide preemption.
- **Implementations**:
  - **QuerySharedDriverQueue**: One queue per query (conceptually); uses `SubQuerySharedDriverQueue` and multiple levels (`_queues[QUEUE_SIZE]`). Level is derived from accumulated time so long-running drivers move to lower priority. Cancelled drivers are served first from a `pending_cancel_queue`.
  - **WorkGroupDriverQueue**: When resource groups are enabled. Two-level: pick workgroup by minimum vruntime, then pick a driver from that workgroup’s queue. Supports `should_yield` for CPU fairness.
- **Lifecycle**: Created with the executor (e.g. `GlobalDriverExecutor`), used for the lifetime of the executor; `close()` drains and stops `take()`.

#### DriverExecutor and GlobalDriverExecutor (`be/src/exec/pipeline/pipeline_driver_executor.h`, `.cpp`)

- **Role**: **Schedules and runs** drivers on a thread pool. Submits drivers into the queue, runs worker threads that take drivers and call `process()`, and finalizes or re-queues them based on state. Reports execution state and audit statistics to the FE.
- **DriverExecutor** (interface):
  - `submit(driver)`, `cancel(driver)`, `close()`
  - `report_exec_state()`, `report_audit_statistics()`
  - `iterate_immutable_blocking_driver()`, `activate_parked_driver()`, `calculate_parked_driver()` (for poller integration)
  - `report_epoch()` (stream MV), `bind_cpus()` (NUMA/CPU binding)
- **GlobalDriverExecutor** (concrete):
  - Owns `_driver_queue` (e.g. `QuerySharedDriverQueue` or `WorkGroupDriverQueue`), `_thread_pool`, `_blocked_driver_poller`, `_exec_state_reporter`, `_audit_statistics_reporter`.
  - **submit(driver)**: Starts timers; if precondition block, sends driver to poller; else prepares operators and either puts driver in the ready queue or (if source has no output yet) into the poller as `INPUT_EMPTY` or parked (stream).
  - **Worker loop** (`_worker_thread()`): Gets next driver from `_get_next_driver(local_driver_queue)` (local queue first for `LOCAL_WAITING` drivers, then global `_driver_queue->take()`). If fragment cancelled or driver already finished, finalizes or re-blocks; else calls `driver->process()`. According to returned `DriverState`: `READY`/`RUNNING` → `put_back_from_executor`; `LOCAL_WAITING` → push to local queue; `FINISH`/`CANCELED`/`INTERNAL_ERROR` → `_finalize_driver`; `EPOCH_FINISH` → `_finalize_epoch` and park; blocking states → `_blocked_driver_poller->add_blocked_driver`.
  - **Lifecycle**: `initialize(num_threads)` starts the poller and worker threads; `close()` closes the queue and waits for the thread pool. Executor lives for the BE process or the scope that owns it (e.g. global pipeline executor).

#### PipelineDriverPoller (`be/src/exec/pipeline/pipeline_driver_poller.h`)

- **Role**: Manages **blocked** drivers (e.g. `INPUT_EMPTY`, `OUTPUT_FULL`, `PRECONDITION_BLOCK`, `PENDING_FINISH`). A dedicated thread periodically checks whether they become ready and moves them back to the driver queue.
- **Key operations**: `add_blocked_driver(driver)`, `remove_blocked_driver()`, `park_driver()` / `activate_parked_driver(predicate)`, `for_each_driver()`.
- **Lifecycle**: Started with the executor (`start()`), stopped on `shutdown()`. Blocked lists are internal (`_blocked_drivers`, `_parked_drivers`).

#### FragmentContext and QueryContext (`be/src/exec/pipeline/fragment_context.h`, `query_context.h`)

- **FragmentContext**: Per-fragment-instance state: `runtime_state`, plan, fragment instance id, FE address, finish promise, cancellation, execution groups, etc. Drivers hold a raw pointer and use it for runtime state and reporting.
- **QueryContext**: Per-query state on one BE: query id, fragment count, expiration, memory/cpu tracking, spill manager, etc. Shared by all fragments of that query on this BE. Drivers hold a raw pointer for cancellation and resource accounting.

#### Operator and OperatorFactory (`be/src/exec/pipeline/operator.h`, etc.)

- **Operator lifecycle** (each method invoked in order, once per stage): `prepare` → `prepare_local_state` → (during execution) `set_finishing` → `set_finished` → optionally `set_cancelled` → `close`.
- **Execution contract**: Driver only moves data when `has_output()` and `need_input()` are true; it calls `pull_chunk()` on the current operator and `push_chunk()` on the next. Operators do not call each other.
- **OperatorFactory**: Creates operator instances; pipelines hold `OpFactories` and call `create(degree_of_parallelism, driver_index)` to build the operator chain for each driver.

#### Other Important Files

- **`be/src/exec/pipeline/pipeline_fwd.h`**: Forward declarations and type aliases (`QueryContext`, `FragmentContext`, `Pipeline`, `PipelineDriver`, `DriverPtr`, `Drivers`, `Operator`, `OpFactories`, etc.).
- **`be/src/exec/pipeline/fragment_executor.h`**: Coordinates fragment execution: builds pipelines from the plan, creates fragment/query context, instantiates drivers, and submits them to the executor.
- **`be/src/exec/pipeline/pipeline_builder.cpp`**: Builds pipeline trees from the physical plan (creates pipelines and operator factories).
- **`be/src/exec/workgroup/pipeline_executor_set.cpp`** (or similar): Integrates pipeline executor with workgroups when resource groups are enabled.
- **`be/src/service/internal_service.cpp`**: RPC entry that receives plan fragments and triggers pipeline execution (fragment execution).
- **`be/src/runtime/plan_fragment_executor.cpp`**: Plan fragment execution coordination (FE-facing fragment execution entry; may delegate to pipeline fragment executor).

**Key Files Quick Reference:**

| File | Purpose |
|------|---------|
| `be/src/exec/pipeline/pipeline.h` | Pipeline: operator factory chain, driver creation, lifecycle |
| `be/src/exec/pipeline/pipeline_driver.h` | PipelineDriver: execution unit, states, process loop, precondition/RF |
| `be/src/exec/pipeline/pipeline_driver_queue.h` | DriverQueue: ready queue, QuerySharedDriverQueue, WorkGroupDriverQueue |
| `be/src/exec/pipeline/pipeline_driver_executor.h/.cpp` | DriverExecutor / GlobalDriverExecutor: submit, worker loop, finalize, reporting |
| `be/src/exec/pipeline/pipeline_driver_poller.h` | PipelineDriverPoller: blocked and parked drivers, reactivation |
| `be/src/exec/pipeline/fragment_context.h` | FragmentContext: per-fragment-instance state |
| `be/src/exec/pipeline/query_context.h` | QueryContext: per-query state on one BE |
| `be/src/exec/pipeline/operator.h` | Operator: lifecycle, has_output/need_input, pull_chunk/push_chunk |
| `be/src/exec/pipeline/pipeline_fwd.h` | Forward declarations and pipeline type aliases |
| `be/src/exec/pipeline/fragment_executor.h` | Fragment execution coordination and driver submission |
| `be/src/exec/pipeline/pipeline_builder.cpp` | Builds pipelines from the physical plan |

## 7. Core Operators: Scan, Filter, Join, Aggregate

StarRocks implements a comprehensive set of vectorized operators that form the building blocks of query execution. Each operator is optimized for vectorized execution, processing chunks of data efficiently.

**Operator Hierarchy:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Operator Base Classes                                │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Operator (Base)                                                  │  │
│  │  ├─> SourceOperator (produces data)                               │  │
│  │  │   ├─> OlapChunkSource (StarRocks table scan)                   │  │
│  │  │   ├─> HiveChunkSource (Hive table scan)                        │  │
│  │  │   └─> IcebergChunkSource (Iceberg table scan)                  │  │
│  │  │                                                                │  │
│  │  ├─> TransformOperator (transforms data)                          │  │
│  │  │   ├─> ProjectOperator (column selection)                       │  │
│  │  │   ├─> FilterOperator (predicate filtering)                     │  │
│  │  │   ├─> HashJoinOperator (hash joins)                            │  │
│  │  │   ├─> AggregateOperator (grouping and aggregation)             │  │
│  │  │   └─> SortOperator (sorting)                                   │  │
│  │  │                                                                │  │
│  │  └─> SinkOperator (consumes data)                                 │  │
│  │      ├─> ExchangeSinkOperator (sends to other nodes)              │  │
│  │      ├─> ResultSinkOperator (sends to FE)                         │  │
│  │      └─> TableSinkOperator (writes to tables)                     │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

### 7.1. Scan Operators

Scan operators read data from storage systems and produce chunks for downstream operators.

**OlapScanNode / OlapChunkSource:**

- **Purpose**: Scans StarRocks native tables (OLAP tables) stored in columnar format.

- **Implementation**:
    - Reads data from tablets (logical storage units)
    - Applies predicate pushdown to filter data at storage level
    - Supports index-based pruning (zonemap, bloom filter)
    - Reads column data in batches
    - Supports late materialization (decode only needed columns)

- **Optimizations**:
    - **Predicate Pushdown**: Filters are pushed to storage layer, reducing I/O
    - **Index Pruning**: Uses zonemap and bloom filters to skip irrelevant data
    - **Column Pruning**: Reads only columns needed by the query
    - **Late Materialization**: Decodes columns only when needed

**Key Files:**

- `be/src/exec/olap_scan_node.cpp` - Legacy scan node
- `be/src/exec/pipeline/scan/olap_chunk_source.cpp` - Pipeline scan source

### 7.2. Filter Operators

Filter operators apply predicates to chunks, producing filtered chunks with only matching rows.

**FilterOperator:**

- **Purpose**: Filters rows based on predicate expressions.

- **Implementation**:
    - Evaluates predicates on entire chunks
    - Produces filter vectors (bitmaps) indicating matching rows
    - Applies filter vectors to chunks, removing non-matching rows
    - Supports complex predicates (AND, OR, NOT)

- **Optimizations**:
    - **Vectorized Evaluation**: Evaluates predicates on entire columns
    - **Short-Circuit Evaluation**: Stops evaluation when possible
    - **SIMD Optimizations**: Uses SIMD for comparison operations

**Key Files:**

- `be/src/exec/pipeline/project_operator.cpp` - Project and filter operations

### 7.3. Join Operators

Join operators combine data from multiple sources based on join conditions.

**HashJoinOperator:**

- **Purpose**: Implements hash joins for equi-joins.

- **Implementation**:
    - **Build Phase**: Builds a hash table from the smaller (build) table
    - **Probe Phase**: Probes the hash table with rows from the larger (probe) table
    - Supports inner, left, right, and full outer joins
    - Handles multiple join keys

- **Optimizations**:
    - **Vectorized Hash Table**: Uses vectorized hash table operations
    - **Runtime Filters**: Applies runtime filters (bloom filters) to reduce probe cost
    - **Partitioned Joins**: Partitions large joins for better memory utilization

**NestLoopJoinOperator:**

- **Purpose**: Implements nested loop joins for small tables or non-equi joins.

- **Implementation**:
    - Iterates through outer table rows
    - For each outer row, scans inner table for matches
    - Supports all join types

**Key Files:**

- `be/src/exec/hash_join_node.cpp` - Legacy hash join
- `be/src/exec/pipeline/join/hash_join_operator.cpp` - Pipeline hash join

### 7.4. Aggregate Operators

Aggregate operators perform grouping and aggregation operations.

**AggregateOperator:**

- **Purpose**: Groups rows and computes aggregate functions (SUM, COUNT, AVG, etc.).

- **Implementation**:
    - **Streaming Aggregation**: Processes chunks incrementally, updating aggregate state
    - **Hash Aggregation**: Uses hash tables to group rows
    - Supports multiple aggregate functions per operator
    - Handles GROUP BY clauses

- **Aggregation Modes**:
    - **Streaming**: Processes data as it arrives (for pre-sorted data)
    - **Hash-Based**: Uses hash tables for grouping (general case)
    - **Two-Phase**: Local aggregation followed by global aggregation (distributed)

- **Optimizations**:
    - **Vectorized Hash Tables**: Uses vectorized hash table operations
    - **Pre-Aggregation**: Aggregates at scan level when possible
    - **Partial Aggregation**: Performs partial aggregation before shuffling

**Key Files:**

- `be/src/exec/aggregation_node.cpp` - Legacy aggregation
- `be/src/exec/pipeline/aggregate/aggregate_operator.cpp` - Pipeline aggregation

### 7.5. Exchange Operators

Exchange operators handle data shuffling between nodes and fragments.

**ExchangeSourceOperator / ExchangeSinkOperator:**

- **Purpose**: Transfers data between plan fragments executing on different nodes.

- **Implementation**:
    - **ExchangeSink**: Sends chunks to remote nodes via bRPC
    - **ExchangeSource**: Receives chunks from remote nodes
    - Supports hash distribution, broadcast, and range distribution
    - Handles backpressure and flow control

- **Data Distribution**:
    - **Hash Distribution**: Distributes data based on hash of partition keys
    - **Broadcast**: Broadcasts data to all nodes (for small tables)
    - **Range Distribution**: Distributes data based on value ranges (for sorting)

**Key Files:**

- `be/src/exec/exchange_node.cpp` - Legacy exchange
- `be/src/exec/pipeline/exchange/exchange_sink_operator.cpp` - Pipeline exchange sink
- `be/src/exec/pipeline/exchange/exchange_source_operator.cpp` - Pipeline exchange source

**Key Files to Reference:**

- `be/src/exec/olap_scan_node.cpp` - OLAP table scanning
- `be/src/exec/hash_join_node.cpp` - Hash join implementation
- `be/src/exec/aggregation_node.cpp` - Aggregation implementation
- `be/src/exec/exchange_node.cpp` - Data exchange
- `be/src/exec/pipeline/` - Pipeline operator implementations

## 8. Expression Evaluation and JIT Compilation

StarRocks evaluates SQL expressions (predicates, projections, aggregations) in vectorized mode, processing entire columns at once. The expression evaluation engine supports complex expressions, function calls, and can leverage JIT (Just-In-Time) compilation for hot expressions to further improve performance.

**Expression Evaluation Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Expression Tree                                      │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Expr (Base)                                                        │  │
│  │  ├─> SlotRef (column reference)                                     │  │
│  │  ├─> LiteralExpr (constant value)                                   │  │
│  │  ├─> BinaryPredicate (comparisons: =, <, >, etc.)                   │  │
│  │  ├─> CompoundPredicate (AND, OR, NOT)                              │  │
│  │  ├─> ArithmeticExpr (+, -, *, /, %)                                   │  │
│  │  ├─> FunctionCallExpr (function calls)                               │  │
│  │  └─> CaseExpr (CASE WHEN ... THEN ... END)                          │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    Vectorized Evaluation                                 │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Expr::evaluate(Chunk* chunk) → ColumnPtr                          │  │
│  │                                                                     │  │
│  │  Evaluation Process:                                               │  │
│  │  1. Recursively evaluate child expressions                          │  │
│  │  2. Apply operator/function to child results                       │  │
│  │  3. Return result column                                           │  │
│  │                                                                     │  │
│  │  Example: col1 + col2 * 10                                        │  │
│  │  ├─> Evaluate col2 → Column1                                      │  │
│  │  ├─> Evaluate 10 → ConstantColumn                                  │  │
│  │  ├─> Evaluate col2 * 10 → Column2 (vectorized multiply)         │  │
│  │  ├─> Evaluate col1 → Column3                                      │  │
│  │  └─> Evaluate col1 + (col2 * 10) → ResultColumn                  │  │
│  └───────────────────┬───────────────────────────────────────────────┘  │
└──────────────────────┼───────────────────────────────────────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                    JIT Compilation (Optional)                            │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Hot Expression Detection                                            │  │
│  │  ├─> Tracks expression execution frequency                         │  │
│  │  └─> Identifies hot expressions (> threshold)                       │  │
│  │                                                                     │  │
│  │  JIT Compilation                                                    │  │
│  │  ├─> Compiles expression tree to native code                       │  │
│  │  ├─> Eliminates function call overhead                             │  │
│  │  ├─> Enables further optimizations                                  │  │
│  │  └─> Caches compiled code for reuse                                │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**Expression Tree Structure:**

Expressions are represented as trees, with each node being an `Expr` subclass:

- **Leaf Nodes**: `SlotRef` (column references), `LiteralExpr` (constants)
- **Unary Operators**: `CastExpr` (type casting), `IsNullPredicate` (null checks)
- **Binary Operators**: `BinaryPredicate` (comparisons), `ArithmeticExpr` (arithmetic)
- **N-ary Operators**: `FunctionCallExpr` (functions), `CompoundPredicate` (logical operators)

**Vectorized Evaluation:**

- **Column-Based Processing**: Expressions are evaluated on entire columns, producing result columns. This enables SIMD optimizations and reduces function call overhead.

- **Lazy Evaluation**: Child expressions are evaluated only when needed, and results are cached when possible.

- **Constant Folding**: Constant expressions are evaluated at compile time, reducing runtime overhead.

- **Null Handling**: Null values are tracked via null bitmaps, enabling efficient null propagation.

**Function Call Evaluation:**

- **Built-in Functions**: StarRocks provides hundreds of built-in functions with vectorized implementations. Functions are registered in a function registry and selected based on argument types.

- **Function Overloading**: The expression evaluator selects appropriate function overloads based on argument types, preferring exact matches over type coercions.

- **Vectorized Function Implementations**: Functions are implemented to process entire columns at once, leveraging SIMD when possible.

**JIT Compilation:**

- **Hot Expression Detection**: The system tracks expression execution frequency and identifies "hot" expressions that are executed frequently.

- **JIT Compilation**: Hot expressions are compiled to native code using JIT compilation (e.g., LLVM), eliminating function call overhead and enabling further optimizations.

- **Code Caching**: Compiled code is cached and reused for subsequent evaluations of the same expression.

**Operation on Encoded Data:**

StarRocks supports operating directly on encoded data without decoding, reducing CPU overhead:

- **Dictionary Encoding**: String columns can be dictionary-encoded, with operations performed on dictionary IDs rather than actual strings.

- **Run-Length Encoding**: Repeated values are run-length encoded, with operations performed on run representations.

- **Bit-Packed Encoding**: Small integer values are bit-packed, reducing memory usage and enabling efficient operations.

**Key Files to Reference:**

- `be/src/exprs/` - Expression evaluation implementation
- `be/src/exprs/expr.cpp` - Base expression class
- `be/src/exprs/expr_context.cpp` - Expression evaluation context
- `be/src/exprs/function_call_expr.cpp` - Function call evaluation
- `be/src/exprs/vectorized/` - Vectorized expression implementations

## 9. Memory Management in the Execution Engine

StarRocks implements sophisticated memory management to track memory usage, enforce limits, and prevent memory exhaustion. The memory management system uses `MemTracker` for hierarchical memory tracking, chunk-based allocation for efficient memory usage, and spilling mechanisms for operators that exceed memory limits.

**Memory Management Architecture:**

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    Memory Hierarchy                                     │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Query MemTracker (per query)                                       │  │
│  │  ├─> Tracks total query memory usage                                │  │
│  │  ├─> Enforces query memory limits                                   │  │
│  │  └─> Coordinates memory across fragments                            │  │
│  │       │                                                             │  │
│  │       ├─> Fragment MemTracker (per fragment)                         │  │
│  │       │   ├─> Tracks fragment memory                                │  │
│  │       │   └─> Coordinates memory across operators                    │  │
│  │       │       │                                                       │  │
│  │       │       ├─> Operator MemTracker (per operator)                │  │
│  │       │       │   ├─> HashJoinOperator: hash table memory          │  │
│  │       │       │   ├─> AggregateOperator: hash table memory          │  │
│  │       │       │   └─> SortOperator: sort buffer memory              │  │
│  │       │       │                                                       │  │
│  │       │       └─> Buffer MemTracker (per buffer)                   │  │
│  │       │           ├─> Exchange buffer memory                        │  │
│  │       │           └─> Result buffer memory                           │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    Chunk-Based Memory Allocation                        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Chunk Memory Management                                            │  │
│  │  ├─> Chunks allocated from MemPool                                  │  │
│  │  ├─> Column data stored contiguously                                │  │
│  │  ├─> Memory tracked via MemTracker                                  │  │
│  │  └─> Automatic cleanup on chunk destruction                         │  │
│  │                                                                     │  │
│  │  MemPool (Memory Pool)                                              │  │
│  │  ├─> Pre-allocates memory blocks                                    │  │
│  │  ├─> Reduces allocation overhead                                     │  │
│  │  └─> Tracks memory via MemTracker                                   │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                    Memory Spilling                                       │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  When Memory Limit Exceeded:                                        │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  1. Operator detects memory limit                             │  │  │
│  │  │  2. Spills intermediate data to disk                         │  │  │
│  │  │  3. Continues processing with reduced memory                   │  │  │
│  │  │  4. Reads spilled data when needed                             │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │                                                                     │  │
│  │  Spillable Operators:                                                │  │
│  │  ├─> HashJoinOperator: spills hash table partitions                │  │
│  │  ├─> AggregateOperator: spills hash table partitions                │  │
│  │  └─> SortOperator: spills sort runs                                 │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

**MemTracker System:**

- **Hierarchical Tracking**: Memory is tracked in a hierarchy from query-level to operator-level, enabling precise memory accounting and limit enforcement.

- **Automatic Tracking**: Memory allocations are automatically tracked via `MemTracker`, with operators registering their memory usage.

- **Limit Enforcement**: `MemTracker` enforces memory limits at each level, triggering spilling or query cancellation when limits are exceeded.

- **Memory Reporting**: Memory usage is reported in query profiles, enabling memory analysis and optimization.

**Chunk-Based Allocation:**

- **Chunk Memory**: Chunks are allocated from memory pools (`MemPool`), reducing allocation overhead and improving cache locality.

- **Column Memory**: Column data is stored contiguously within chunks, enabling efficient SIMD operations and cache-friendly access.

- **Memory Reuse**: Chunks and columns are reused when possible, reducing allocation and deallocation overhead.

**Memory Limits:**

- **Query Memory Limit**: Each query has a memory limit (`query_mem_limit`), enforced at the query `MemTracker` level.

- **Operator Memory Limits**: Operators may have individual memory limits, enabling fine-grained memory control.

- **Global Memory Limits**: Global memory limits prevent cluster-wide memory exhaustion.

**Memory Spilling:**

When operators exceed memory limits, they can spill intermediate data to disk:

- **Hash Join Spilling**: Hash join operators spill hash table partitions to disk when memory is exhausted, then probe spilled partitions.

- **Aggregation Spilling**: Aggregate operators spill hash table partitions to disk, performing multi-pass aggregation.

- **Sort Spilling**: Sort operators spill sort runs to disk, then merge sorted runs.

**Buffer Management:**

- **Exchange Buffers**: Data exchange between nodes uses bounded buffers to prevent memory exhaustion. When buffers are full, senders block until space is available.

- **Result Buffers**: Query results are buffered in `ResultBufferMgr`, with bounded buffer sizes to prevent FE memory exhaustion.

**Key Files to Reference:**

- `be/src/runtime/mem_tracker.h` - Memory tracking implementation
- `be/src/runtime/mem_pool.h` - Memory pool implementation
- `be/src/column/chunk.h` - Chunk memory management
- `be/src/runtime/buffer_control_block.h` - Buffer management
- `be/src/exec/pipeline/operator.h` - Operator memory tracking
