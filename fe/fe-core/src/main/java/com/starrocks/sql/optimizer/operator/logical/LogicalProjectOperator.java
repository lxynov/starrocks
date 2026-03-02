// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.sql.optimizer.operator.logical;

import com.google.common.base.Preconditions;
import com.google.common.collect.Maps;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.sql.optimizer.ExpressionContext;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptExpressionVisitor;
import com.starrocks.sql.optimizer.RowOutputInfo;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.Operator;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.OperatorVisitor;
import com.starrocks.sql.optimizer.operator.Projection;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.property.DomainProperty;
import com.starrocks.sql.optimizer.rewrite.ReplaceColumnRefRewriter;
import com.starrocks.sql.optimizer.rewrite.ScalarOperatorRewriter;
import org.apache.commons.collections4.CollectionUtils;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Deque;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.stream.Collectors;

public final class LogicalProjectOperator extends LogicalOperator {
    // Output columns
    private Map<ColumnRefOperator, ScalarOperator> columnRefMap;
    // Hidden columns which might be referenced by output or other hidden columns
    private Map<ColumnRefOperator, ScalarOperator> subColumnRefMap;

    public LogicalProjectOperator(Map<ColumnRefOperator, ScalarOperator> columnRefMap) {
        super(OperatorType.LOGICAL_PROJECT);
        this.columnRefMap = columnRefMap;
        this.subColumnRefMap = Maps.newHashMap();
    }

    public LogicalProjectOperator(Map<ColumnRefOperator, ScalarOperator> columnRefMap, long limit) {
        super(OperatorType.LOGICAL_PROJECT);
        this.columnRefMap = columnRefMap;
        this.subColumnRefMap = Maps.newHashMap();
        this.limit = limit;
    }

    public LogicalProjectOperator(Map<ColumnRefOperator, ScalarOperator> columnRefMap,
                                  Map<ColumnRefOperator, ScalarOperator> subColumnRefMap) {
        super(OperatorType.LOGICAL_PROJECT);
        this.columnRefMap = columnRefMap;
        this.subColumnRefMap = subColumnRefMap;
    }

    public LogicalProjectOperator(Map<ColumnRefOperator, ScalarOperator> columnRefMap,
                                  Map<ColumnRefOperator, ScalarOperator> subColumnRefMap, long limit) {
        super(OperatorType.LOGICAL_PROJECT);
        this.columnRefMap = columnRefMap;
        this.subColumnRefMap = subColumnRefMap;
        this.limit = limit;
    }

    private LogicalProjectOperator() {
        super(OperatorType.LOGICAL_PROJECT);
    }

    public Map<ColumnRefOperator, ScalarOperator> getColumnRefMap() {
        return columnRefMap;
    }

    public Map<ColumnRefOperator, ScalarOperator> getSubColumnRefMap() {
        return subColumnRefMap;
    }

    /**
     * Remove unused or self-referencing entries from subColumnRefMap, and inline entries that are constant or referenced exactly
     * once into the referencing expression. ASSERT_TRUE entries are promoted into columnRefMap to preserve their runtime side
     * effects.
     */
    public void compactSubColumnRefMap() {
        if (subColumnRefMap.isEmpty()) {
            return;
        }

        // 1. Remove self-referencing entries (key maps to itself) and promote ASSERT_TRUE entries into columnRefMap so they are
        // never dropped.
        for (Iterator<Map.Entry<ColumnRefOperator, ScalarOperator>> it = subColumnRefMap.entrySet().iterator(); it.hasNext(); ) {
            Map.Entry<ColumnRefOperator, ScalarOperator> entry = it.next();
            if (entry.getKey().equals(entry.getValue())) {
                it.remove();
            } else if (isAssertTrue(entry.getValue())) {
                columnRefMap.put(entry.getKey(), entry.getValue());
                it.remove();
            }
        }

        // 2. Collect all column refs used by output columns directly or transitively through hidden columns, then remove unused
        //
        // hidden columns.
        ColumnRefSet usedColumns = new ColumnRefSet();
        for (ScalarOperator expr : columnRefMap.values()) {
            usedColumns.union(expr.getUsedColumns());
        }
        Map<Integer, ScalarOperator> idToSubColumnExpr = new HashMap<>();
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : subColumnRefMap.entrySet()) {
            idToSubColumnExpr.put(entry.getKey().getId(), entry.getValue());
        }
        Deque<Integer> dq = usedColumns.getStream().collect(Collectors.toCollection(ArrayDeque::new));
        while (!dq.isEmpty()) {
            int id = dq.remove();
            ScalarOperator expr = idToSubColumnExpr.get(id);
            if (expr != null) {
                for (int depId : expr.getUsedColumns().getColumnIds()) {
                    if (!usedColumns.contains(depId)) {
                        usedColumns.union(depId);
                        dq.add(depId);
                    }
                }
            }
        }
        subColumnRefMap.keySet().removeIf(k -> !usedColumns.contains(k));

        if (subColumnRefMap.isEmpty()) {
            return;
        }

        // 3. Mark hidden columns which are constant or referenced exactly once as "to be inlined"
        List<ColumnRefOperator> toInline = new ArrayList<>();
        Map<ColumnRefOperator, Integer> refCounts = new HashMap<>();
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : subColumnRefMap.entrySet()) {
            if (entry.getValue().isConstant()) {
                toInline.add(entry.getKey());
            } else {
                refCounts.put(entry.getKey(), 0);
            }
        }
        for (ScalarOperator expr : columnRefMap.values()) {
            countReferences(expr, refCounts);
        }
        for (ScalarOperator expr : subColumnRefMap.values()) {
            countReferences(expr, refCounts);
        }
        for (Map.Entry<ColumnRefOperator, Integer> entry : refCounts.entrySet()) {
            if (entry.getValue() == 1) {
                toInline.add(entry.getKey());
            }
        }

        // 4. Inline the above-built "to be inlined" list, and remove them from subColumnRefMap
        if (!toInline.isEmpty()) {
            Map<ColumnRefOperator, ScalarOperator> inlineMap = new HashMap<>();
            for (ColumnRefOperator key : toInline) {
                inlineMap.put(key, subColumnRefMap.get(key));
            }
            ReplaceColumnRefRewriter rewriter = new ReplaceColumnRefRewriter(inlineMap);
            ScalarOperatorRewriter scalarRewriter = new ScalarOperatorRewriter();

            columnRefMap.replaceAll((k, v) -> {
                ScalarOperator result = rewriter.rewrite(v);
                if (result.isConstant()) {
                    result = scalarRewriter.rewrite(result, ScalarOperatorRewriter.DEFAULT_REWRITE_RULES);
                }
                return result;
            });

            for (Iterator<Map.Entry<ColumnRefOperator, ScalarOperator>> it = subColumnRefMap.entrySet().iterator();
                    it.hasNext(); ) {
                Map.Entry<ColumnRefOperator, ScalarOperator> entry = it.next();
                if (inlineMap.containsKey(entry.getKey())) {
                    it.remove();
                } else {
                    ScalarOperator result = rewriter.rewrite(entry.getValue());
                    if (result.isConstant()) {
                        result = scalarRewriter.rewrite(result, ScalarOperatorRewriter.DEFAULT_REWRITE_RULES);
                    }
                    entry.setValue(result);
                }
            }
        }
    }

    /**
     * Inline all hidden columns from subColumnRefMap into columnRefMap and clear subColumnRefMap. ASSERT_TRUE entries that are
     * not referenced by any output column are promoted into columnRefMap so their runtime side effects are preserved.
     */
    public void eliminateSubColumnRefMap() {
        if (subColumnRefMap.isEmpty()) {
            return;
        }

        ColumnRefSet referencedByOutput = new ColumnRefSet();
        for (ScalarOperator expr : columnRefMap.values()) {
            referencedByOutput.union(expr.getUsedColumns());
        }

        ReplaceColumnRefRewriter rewriter = new ReplaceColumnRefRewriter(subColumnRefMap, true);
        ScalarOperatorRewriter scalarRewriter = new ScalarOperatorRewriter();
        columnRefMap.replaceAll((k, v) -> {
            ScalarOperator result = rewriter.rewrite(v);
            if (result.isConstant()) {
                result = scalarRewriter.rewrite(result, ScalarOperatorRewriter.DEFAULT_REWRITE_RULES);
            }
            return result;
        });

        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : subColumnRefMap.entrySet()) {
            if (isAssertTrue(entry.getValue()) && !referencedByOutput.contains(entry.getKey())) {
                ScalarOperator resolved = rewriter.rewrite(entry.getValue());
                columnRefMap.put(entry.getKey(), resolved);
            }
        }

        subColumnRefMap.clear();
    }

    private static boolean isAssertTrue(ScalarOperator op) {
        return op instanceof CallOperator &&
                FunctionSet.ASSERT_TRUE.equals(((CallOperator) op).getFnName());
    }

    private static void countReferences(ScalarOperator expr, Map<ColumnRefOperator, Integer> refCounts) {
        if (expr.isColumnRef()) {
            refCounts.computeIfPresent((ColumnRefOperator) expr, (k, v) -> v + 1);
            return;
        }
        for (ScalarOperator child : expr.getChildren()) {
            countReferences(child, refCounts);
        }
    }

    @Override
    public ColumnRefSet getOutputColumns(ExpressionContext expressionContext) {
        ColumnRefSet columns = new ColumnRefSet();
        for (Map.Entry<ColumnRefOperator, ScalarOperator> kv : columnRefMap.entrySet()) {
            columns.union(kv.getKey());
        }
        return columns;
    }

    @Override
    public RowOutputInfo deriveRowOutputInfo(List<OptExpression> inputs) {
        return new RowOutputInfo(columnRefMap, Maps.newHashMap());
    }

    @Override
    public DomainProperty deriveDomainProperty(List<OptExpression> inputs) {
        if (CollectionUtils.isEmpty(inputs)) {
            return new DomainProperty(Map.of());
        }
        DomainProperty childDomainProperty = inputs.get(0).getDomainProperty();

        return childDomainProperty.projectDomainProperty(columnRefMap);
    }

    @Override
    public int hashCode() {
        return Objects.hash(super.hashCode(), opType, columnRefMap);
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) {
            return true;
        }

        if (!super.equals(o)) {
            return false;
        }

        LogicalProjectOperator that = (LogicalProjectOperator) o;

        return columnRefMap.keySet().equals(that.columnRefMap.keySet());
    }

    @Override
    public String toString() {
        return "LogicalProjectOperator " + columnRefMap.keySet();
    }

    @Override
    public <R, C> R accept(OperatorVisitor<R, C> visitor, C context) {
        return visitor.visitLogicalProject(this, context);
    }

    @Override
    public <R, C> R accept(OptExpressionVisitor<R, C> visitor, OptExpression optExpression, C context) {
        return visitor.visitLogicalProject(optExpression, context);
    }

    public static Builder builder() {
        return new Builder();
    }

    public static class Builder extends Operator.Builder<LogicalProjectOperator, LogicalProjectOperator.Builder> {

        @Override
        protected LogicalProjectOperator newInstance() {
            return new LogicalProjectOperator();
        }

        @Override
        public Builder withOperator(LogicalProjectOperator operator) {
            super.withOperator(operator);
            builder.columnRefMap = operator.getColumnRefMap();
            return this;
        }

        public Builder setColumnRefMap(Map<ColumnRefOperator, ScalarOperator> columnRefMap) {
            builder.columnRefMap = columnRefMap;
            return this;
        }

        public Builder setSubColumnRefMap(Map<ColumnRefOperator, ScalarOperator> subColumnRefMap) {
            builder.subColumnRefMap = subColumnRefMap;
            return this;
        }

        @Override
        public Builder setProjection(Projection projection) {
            Preconditions.checkState(false, "Shouldn't set projection to Project Operator");
            return this;
        }
    }
}