# StarRocks Profile Coverage Expansion Plan

## Executive Summary

This plan outlines the strategy for expanding StarRocks profile coverage to capture additional execution details, with a focus on integrating useful plan attributes from EXPLAIN output. The goal is to make runtime profiles standalone and comprehensive, potentially enabling SQL reconstruction from profile output.

## Current State Analysis

### Existing Profile Capabilities

The current StarRocks profiling system captures:

1. **Runtime Metrics:**
    - Execution timings (operator total time, scan time, network time)
    - Resource usage (CPU, memory, I/O)
    - Data flow metrics (rows read/written, bytes read/written)
    - Operator-specific metrics (hash table size, filter effectiveness, etc.)

2. **Profile Structure:**
    - Hierarchical organization: Query → Fragment → FragmentInstance → Pipeline → Operator
    - CommonMetrics (shared across operators)
    - UniqueMetrics (operator-specific)
    - Info strings (query metadata, configuration)

3. **Profile Storage:**
    - In-memory storage via `ProfileManager`
    - Optional disk logging via `fe.profile.log`
    - Compression support (GZIP)

### EXPLAIN Output Capabilities

The `ProfilingExecPlan` structure (used by EXPLAIN) contains:

1. **Plan Structure:**
    - Fragment hierarchy
    - Plan node hierarchy with parent-child relationships
    - Plan node IDs

2. **Cost Estimates:**
    - CPU cost
    - Memory cost
    - Network cost
    - Total cost

3. **Statistics:**
    - Output row count estimates
    - Column statistics (min, max, null count, average size, cardinality)

4. **Operator-Specific Information:**
    - **AggregationNode:** Aggregate expressions, grouping expressions
    - **JoinNode:** Join type, distribution mode, equality join conjuncts
    - **ScanNode:** Table name, scan predicates
    - **ProjectNode:** Projection expressions, common expressions
    - **SelectNode:** Filter predicates
    - **SortNode:** Partition expressions, ordering expressions
    - **AnalyticEvalNode:** Window functions, partition expressions, order-by expressions
    - **ExchangeNode:** Distribution type, partition expressions

5. **Title Attributes:**
    - Join operations (INNER, LEFT, etc.)
    - Distribution modes (BROADCAST, SHUFFLE, etc.)
    - Aggregation modes (merge, update, finalize, serialize)
    - Sort types (TOP-N, PARTITION-TOP-N, SORT)

### Gap Analysis

**Status:** This section has been fact-checked against the codebase. Items marked with ✅ **EXISTS** are already available in runtime profiles as info strings. Items marked with **MISSING** or **PARTIALLY MISSING** are not currently in profiles.

**Missing from Profiles but Available in EXPLAIN:**

1. **Plan Structure Information:**
    - Fragment-to-fragment relationships (Note: Topology is available via `Topology` info string in query profile)
    - Complete plan node hierarchy (Note: Plan node IDs are available via operator names, can be extracted via regex parsing)

2. **Cost Estimates:**
    - Estimated CPU, memory, network costs
    - Total cost estimates
    - Comparison between estimated and actual costs

3. **Statistics:**
    - Estimated row counts
    - Column statistics
    - Cardinality estimates

4. **Expression Information:**
    - ~~Predicates (WHERE clauses, join conditions)~~ ✅ **EXISTS**: Available as `Predicates` and `JoinPredicates` info strings
    - Projection expressions (SELECT columns) - **MISSING**
    - ~~Aggregation expressions (GROUP BY, aggregate functions)~~ ✅ **EXISTS**: Available as `GroupingKeys` and `AggregateFunctions` info strings
    - ~~Ordering expressions (ORDER BY)~~ ✅ **EXISTS**: Available as `SortKeys` info string
    - Partition expressions - **PARTIALLY MISSING** (some partition info available in sort operators)

5. **Operator Configuration:**
    - ~~Join types and distribution modes~~ ✅ **EXISTS**: Available as `JoinType` and `DistributionMode` info strings
    - Aggregation modes - **MISSING** (streaming preaggregation mode not in profiles)
    - ~~Sort types~~ ✅ **EXISTS**: Available as `SortType` info string (TopN vs All)
    - ~~Table names for scan operators~~ ✅ **EXISTS**: Available as `Table` info string

6. **Native Function Information:**
    - Function names and signatures
    - Function call contexts
    - Function execution statistics (if applicable)

**Note:** Many items previously listed as missing actually exist in profiles as info strings. The above list has been fact-checked against the codebase.

## Expansion Goals

### Primary Goals

1. **Standalone Profile Completeness:**
    - Profiles should contain all information needed to understand query execution without requiring EXPLAIN output
    - Enable profile analysis without access to original SQL or plan

2. **Plan Attribute Integration:**
    - Integrate useful attributes from `ProfilingExecPlan` into runtime profiles
    - Preserve plan structure information in profiles
    - Include cost estimates and statistics for comparison with actuals

3. **SQL Reconstruction Feasibility:**
    - Capture sufficient information to enable simplified SQL reconstruction
    - Include table names, column references, predicates, and expressions
    - Support basic query structure reconstruction

### Secondary Goals

1. **Native Function Coverage:**
    - Track native function usage in profiles
    - Capture function-specific execution details
    - Enable function-level performance analysis

2. **Enhanced Debugging:**
    - Provide more context for performance issues
    - Enable better correlation between plan and execution
    - Support query optimization analysis

## Implementation Plan

### Phase 1: Research and Design (Weeks 1-2)

#### 1.1 Information Gap Analysis

**Tasks:**

- [ ] Catalog all information available in `ProfilingExecPlan` that is not in profiles
- [ ] Identify which EXPLAIN attributes are most valuable for profile analysis
- [ ] Determine memory and performance impact of adding plan information
- [ ] Research native function profiling requirements

**Deliverables:**

- Gap analysis document
- Priority list of attributes to integrate
- Memory impact assessment

#### 1.2 Native Function Research

**Tasks:**

- [ ] Identify native function execution points in BE code
- [ ] Determine what function-level information would be useful
- [ ] Research function call tracking mechanisms
- [ ] Assess performance overhead of function-level profiling

**Deliverables:**

- Native function profiling requirements document
- Function tracking design proposal

#### 1.3 Profile Structure Design

**Tasks:**

- [ ] Design extended profile structure to include plan information
- [ ] Determine storage format (info strings vs. structured data)
- [ ] Plan backward compatibility strategy
- [ ] Design SQL reconstruction data model

**Deliverables:**

- Extended profile structure specification
- Migration plan for existing profiles

### Phase 2: Core Plan Attribute Integration (Weeks 3-5)

#### 2.1 Plan Structure Integration

**Tasks:**

- [ ] Add plan node hierarchy to profile Summary section - **PARTIALLY EXISTS** (Topology available)
- [x] ~~Include fragment relationships in profile~~ ✅ **ALREADY EXISTS**: Available as `Topology` info string in query profile
- [ ] Store plan node IDs as structured info strings - **PARTIALLY EXISTS** (available via operator names, regex parsing)
- [x] ~~Add plan topology information~~ ✅ **ALREADY EXISTS**: Available as `Topology` info string

**Implementation Points:**

- Modify `StmtExecutor.buildTopLevelProfile()` to include plan structure
- Extend `RuntimeProfile` info strings with plan hierarchy
- Update `QueryRuntimeProfile` to merge plan information

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/StmtExecutor.java`
- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/common/util/RuntimeProfile.java`

#### 2.2 Cost Estimates Integration

**Tasks:**

- [ ] Add cost estimates to profile Summary section
- [ ] Include estimated vs. actual cost comparison
- [ ] Store cost breakdown (CPU, memory, network) per operator
- [ ] Add cost estimation accuracy metrics

**Implementation Points:**

- Extract cost estimates from `ProfilingExecPlan.ProfilingElement`
- Add cost info strings to operator profiles
- Calculate cost estimation accuracy (estimated vs. actual)

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/sql/ExplainAnalyzer.java`

#### 2.3 Statistics Integration

**Tasks:**

- [ ] Add estimated row counts to operator profiles
- [ ] Include column statistics where relevant
- [ ] Store estimated vs. actual row count comparisons
- [ ] Add statistics accuracy metrics

**Implementation Points:**

- Extract statistics from `ProfilingExecPlan.ProfilingElement`
- Add statistics info strings to operator profiles
- Calculate statistics accuracy metrics

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/common/util/ProfilingExecPlan.java`

### Phase 3: Expression and Configuration Integration (Weeks 6-8)

#### 3.1 Expression Information Integration

**Tasks:**

- [x] ~~Add predicates to scan and select operator profiles~~ ✅ **ALREADY EXISTS**: Available as `Predicates` info string
- [ ] Include projection expressions in project operator profiles - **MISSING**
- [x] ~~Store aggregation expressions in aggregation operator profiles~~ ✅ **ALREADY EXISTS**: Available as `GroupingKeys` and `AggregateFunctions` info strings
- [x] ~~Add join conditions to join operator profiles~~ ✅ **ALREADY EXISTS**: Available as `JoinPredicates` info string
- [x] ~~Include ordering expressions in sort operator profiles~~ ✅ **ALREADY EXISTS**: Available as `SortKeys` info string

**Implementation Points:**

- Extract expression information from `ProfilingExecPlan.ProfilingElement.uniqueInfos`
- Add expression info strings to operator profiles
- Truncate long expressions to prevent profile bloat (already handled in `ProfilingExecPlan`)

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/common/util/ProfilingExecPlan.java`

#### 3.2 Operator Configuration Integration

**Tasks:**

- [x] ~~Add join types and distribution modes to join operator profiles~~ ✅ **ALREADY EXISTS**: Available as `JoinType` and `DistributionMode` info strings
- [ ] Include aggregation modes in aggregation operator profiles - **MISSING** (streaming preaggregation mode)
- [x] ~~Store sort types in sort operator profiles~~ ✅ **ALREADY EXISTS**: Available as `SortType` info string
- [x] ~~Add table names to scan operator profiles~~ ✅ **ALREADY EXISTS**: Available as `Table` info string
- [ ] Include partition information in exchange operator profiles - **MISSING**

**Implementation Points:**

- Extract title attributes and unique infos from `ProfilingExecPlan.ProfilingElement`
- Add configuration info strings to operator profiles
- Ensure configuration information is accessible in profile output

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/sql/ExplainAnalyzer.java`

### Phase 4: Native Function Profiling (Weeks 9-11)

#### 4.1 Function Call Tracking

**Tasks:**

- [ ] Identify native function execution points in BE
- [ ] Design function call tracking mechanism
- [ ] Implement function name and signature capture
- [ ] Add function execution context information

**Implementation Points:**

- Modify function call expressions to track function usage
- Add function profiling to `FunctionContext`
- Store function information in operator profiles

**Files to Investigate:**

- `be/src/exprs/function_call_expr.cpp`
- `be/src/exprs/function_context.h`
- `be/src/exprs/vectorized_function_call_expr.cpp`

#### 4.2 Function-Level Metrics

**Tasks:**

- [ ] Track function execution time
- [ ] Capture function-specific metrics (if applicable)
- [ ] Add function call counts
- [ ] Include function error information

**Implementation Points:**

- Add function-level counters to operator profiles
- Track function execution in expression evaluation
- Aggregate function metrics across operators

**Files to Modify:**

- `be/src/exprs/function_call_expr.cpp`
- `be/src/exprs/vectorized_function_call_expr.cpp`
- `be/src/runtime/runtime_profile.h`

### Phase 5: SQL Reconstruction Support (Weeks 12-14)

#### 5.1 Data Model for SQL Reconstruction

**Tasks:**

- [ ] Design data structure for SQL reconstruction
- [ ] Identify minimum required information for basic SQL reconstruction
- [ ] Plan expression serialization format
- [ ] Design table and column reference tracking

**Deliverables:**

- SQL reconstruction data model specification
- Expression serialization format

#### 5.2 Profile Enhancement for SQL Reconstruction

**Tasks:**

- [ ] Add table and column metadata to profiles
- [ ] Include complete expression trees where possible
- [ ] Store query structure information
- [ ] Add query type and operation information

**Implementation Points:**

- Enhance profile info strings with SQL reconstruction data
- Add structured data section to profiles (JSON format)
- Ensure expression information is complete and accurate

**Files to Modify:**

- `fe/fe-core/src/main/java/com/starrocks/qe/StmtExecutor.java`
- `fe/fe-core/src/main/java/com/starrocks/qe/scheduler/QueryRuntimeProfile.java`
- `fe/fe-core/src/main/java/com/starrocks/common/util/RuntimeProfile.java`

### Phase 6: Testing and Validation (Weeks 15-16)

#### 6.1 Unit Testing

**Tasks:**

- [ ] Write unit tests for plan attribute integration
- [ ] Test expression information capture
- [ ] Validate cost estimate integration
- [ ] Test native function profiling

**Files to Create/Modify:**

- `fe/fe-core/src/test/java/com/starrocks/qe/scheduler/QueryRuntimeProfileTest.java`
- `fe/fe-core/src/test/java/com/starrocks/common/util/ProfilingExecPlanTest.java`

#### 6.2 Integration Testing

**Tasks:**

- [ ] Test profile generation with various query types
- [ ] Validate profile completeness for complex queries
- [ ] Test SQL reconstruction feasibility
- [ ] Verify backward compatibility

**Test Cases:**

- Simple SELECT queries
- Complex JOIN queries
- Aggregation queries
- Window function queries
- Subquery queries
- CTE queries

#### 6.3 Performance Testing

**Tasks:**

- [ ] Measure profile generation overhead
- [ ] Test memory impact of extended profiles
- [ ] Validate profile size limits
- [ ] Test profile compression effectiveness

**Metrics to Track:**

- Profile generation time
- Profile memory usage
- Profile size (compressed and uncompressed)
- Query execution time impact

### Phase 7: Documentation and Rollout (Weeks 17-18)

#### 7.1 Documentation

**Tasks:**

- [ ] Update profile documentation with new attributes
- [ ] Document SQL reconstruction capabilities
- [ ] Create examples of enhanced profiles
- [ ] Update EXPLAIN documentation to reference profile integration

**Files to Update:**

- `docs/en/best_practices/query_tuning/query_profile_overview.md`
- `docs/en/best_practices/query_tuning/query_profile_text_based_analysis.md`
- `xy-docs/starrocks-profiler.md`

#### 7.2 Rollout Plan

**Tasks:**

- [ ] Plan feature flag for gradual rollout
- [ ] Design migration strategy for existing profiles
- [ ] Create rollout checklist
- [ ] Plan monitoring and alerting

## Technical Considerations

### Memory Impact

**Concerns:**

- Adding plan information will increase profile memory usage
- Expression strings can be long
- Multiple fragments and operators multiply the impact

**Mitigation Strategies:**

- Use expression truncation (already implemented in `ProfilingExecPlan`)
- Compress profile content (already implemented)
- Make plan information optional via configuration
- Limit expression depth in profiles

### Performance Impact

**Concerns:**

- Extracting plan information adds overhead to profile generation
- Serializing additional data may slow profile collection
- Increased profile size may impact network transfer

**Mitigation Strategies:**

- Cache plan information extraction
- Use efficient serialization (JSON or Thrift)
- Profile collection is already asynchronous
- Optimize info string storage

### Backward Compatibility

**Concerns:**

- Existing profile consumers may not expect new attributes
- Profile format changes may break tools
- Profile size increases may exceed limits

**Mitigation Strategies:**

- Add new attributes as optional info strings
- Maintain existing profile structure
- Version profile format if needed
- Provide migration utilities

### Expression Serialization

**Concerns:**

- Expressions can be complex and large
- Full expression trees may be too verbose
- Expression serialization must be efficient

**Mitigation Strategies:**

- Use existing `ExprToSql.toSql()` for expression serialization
- Truncate long expressions (already implemented)
- Store expressions as info strings
- Consider structured format for complex expressions

## Success Criteria

### Functional Criteria

1. **Profile Completeness:**
    - Profiles contain all plan structure information
    - Cost estimates are included and comparable with actuals
    - Expression information is captured for all relevant operators
    - Operator configuration is available in profiles

2. **SQL Reconstruction:**
    - Basic SQL structure can be reconstructed from profiles
    - Table and column references are available
    - Predicates and expressions are captured
    - Query type and operations are identifiable

3. **Native Function Coverage:**
    - Function names and signatures are tracked
    - Function execution context is available
    - Function-level metrics are captured (if applicable)

### Performance Criteria

1. **Profile Generation:**
    - Profile generation overhead < 5% of query execution time
    - Profile memory usage increase < 20%
    - Profile size increase < 30% (compressed)

2. **Query Execution:**
    - No measurable impact on query execution time
    - Profile collection remains asynchronous
    - No blocking operations in profile generation

### Quality Criteria

1. **Backward Compatibility:**
    - Existing profile consumers continue to work
    - Profile format remains compatible
    - No breaking changes to profile API

2. **Documentation:**
    - All new attributes are documented
    - Examples are provided for enhanced profiles
    - SQL reconstruction capabilities are documented

## Risk Assessment

### High Risk

1. **Memory Usage:**
    - **Risk:** Profile memory usage may exceed limits
    - **Mitigation:** Make plan information optional, use compression, limit expression depth

2. **Performance Impact:**
    - **Risk:** Profile generation may slow query execution
    - **Mitigation:** Keep profile collection asynchronous, optimize extraction, cache results

### Medium Risk

1. **Expression Complexity:**
    - **Risk:** Complex expressions may be too large or difficult to serialize
    - **Mitigation:** Truncate expressions, use efficient serialization, limit depth

2. **Backward Compatibility:**
    - **Risk:** Profile format changes may break existing tools
    - **Mitigation:** Add attributes as optional, maintain structure, version format

### Low Risk

1. **Native Function Profiling:**
    - **Risk:** Function tracking may have performance overhead
    - **Mitigation:** Make optional, optimize tracking, measure impact

## Future Enhancements

### Potential Additions

1. **Query Optimization Hints:**
    - Capture optimizer decisions and alternatives
    - Include rejected plan information
    - Track optimization time

2. **Resource Group Information:**
    - Include resource group assignments
    - Track resource group limits and usage
    - Add resource group metrics

3. **Materialized View Usage:**
    - Track materialized view hits
    - Include MV selection information
    - Capture MV rewrite decisions

4. **Partition Pruning Information:**
    - Track partition pruning effectiveness
    - Include partition selection details
    - Capture partition statistics

5. **Index Usage:**
    - Track index selection and usage
    - Include index effectiveness metrics
    - Capture index-related statistics

## Conclusion

This plan provides a comprehensive roadmap for expanding StarRocks profile coverage. By integrating plan attributes from EXPLAIN output, we can make profiles standalone and comprehensive, enabling better query analysis and potentially supporting SQL reconstruction. The phased approach allows for incremental implementation with validation at each stage.

The key success factors are:

- Careful memory and performance management
- Maintaining backward compatibility
- Comprehensive testing and validation
- Clear documentation and rollout strategy

With proper execution, this expansion will significantly enhance StarRocks' observability and debugging capabilities.
