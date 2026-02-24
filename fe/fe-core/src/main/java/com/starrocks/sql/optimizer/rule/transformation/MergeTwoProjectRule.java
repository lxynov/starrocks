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

package com.starrocks.sql.optimizer.rule.transformation;

import com.google.common.collect.Lists;
import com.google.common.collect.Maps;
import com.starrocks.catalog.FunctionSet;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.base.ColumnRefSet;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalProjectOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rewrite.ReplaceColumnRefRewriter;
import com.starrocks.sql.optimizer.rewrite.ScalarOperatorRewriter;
import com.starrocks.sql.optimizer.rule.RuleType;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Stream;

public class MergeTwoProjectRule extends TransformationRule {
    public MergeTwoProjectRule() {
        super(RuleType.TF_MERGE_TWO_PROJECT, Pattern.create(OperatorType.LOGICAL_PROJECT)
                .addChildren(Pattern.create(OperatorType.LOGICAL_PROJECT, OperatorType.PATTERN_LEAF)));
    }

    @Override
    public List<OptExpression> transform(OptExpression input, OptimizerContext context) {
        LogicalProjectOperator firstProject = (LogicalProjectOperator) input.getOp();
        LogicalProjectOperator secondProject = (LogicalProjectOperator) input.getInputs().get(0).getOp();

        // Count how many outer expressions reference each inner column.
        // Columns referenced by more than one expression with non-trivial definitions
        // are kept as common sub-expressions to avoid duplicating computation.
        Map<ColumnRefOperator, Integer> refCounts = new HashMap<>();
        for (ColumnRefOperator innerCol : secondProject.getColumnRefMap().keySet()) {
            refCounts.put(innerCol, 0);
        }
        for (ScalarOperator outerExpr : firstProject.getColumnRefMap().values()) {
            ColumnRefSet usedCols = outerExpr.getUsedColumns();
            for (ColumnRefOperator innerCol : secondProject.getColumnRefMap().keySet()) {
                if (usedCols.contains(innerCol)) {
                    refCounts.merge(innerCol, 1, Integer::sum);
                }
            }
        }
        for (ScalarOperator outerExpr : firstProject.getCommonSubOperatorMap().values()) {
            ColumnRefSet usedCols = outerExpr.getUsedColumns();
            for (ColumnRefOperator innerCol : secondProject.getColumnRefMap().keySet()) {
                if (usedCols.contains(innerCol)) {
                    refCounts.merge(innerCol, 1, Integer::sum);
                }
            }
        }

        Map<ColumnRefOperator, ScalarOperator> inlineMap = Maps.newHashMap();
        Map<ColumnRefOperator, ScalarOperator> newCommonSubMap = Maps.newHashMap();
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : secondProject.getColumnRefMap().entrySet()) {
            int count = refCounts.getOrDefault(entry.getKey(), 0);
            ScalarOperator expr = entry.getValue();
            boolean isTrivial = expr.isColumnRef() || expr.isConstant();
            if (count <= 1 || isTrivial) {
                inlineMap.put(entry.getKey(), expr);
            } else {
                newCommonSubMap.put(entry.getKey(), expr);
            }
        }

        ScalarOperatorRewriter scalarRewriter = new ScalarOperatorRewriter();
        ReplaceColumnRefRewriter rewriter = new ReplaceColumnRefRewriter(inlineMap);
        Map<ColumnRefOperator, ScalarOperator> resultMap = Maps.newHashMap();
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : firstProject.getColumnRefMap().entrySet()) {
            ScalarOperator result = rewriter.rewrite(entry.getValue());
            if (result.isConstant()) {
                result = scalarRewriter.rewrite(result, ScalarOperatorRewriter.DEFAULT_REWRITE_RULES);
            }
            resultMap.put(entry.getKey(), result);
        }

        // ASSERT_TRUE must be executed in the runtime, so it should be kept anyway.
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : secondProject.getColumnRefMap().entrySet()) {
            if (entry.getValue() instanceof CallOperator) {
                CallOperator callOp = entry.getValue().cast();
                if (FunctionSet.ASSERT_TRUE.equals(callOp.getFnName())) {
                    resultMap.put(entry.getKey(), entry.getValue());
                    newCommonSubMap.remove(entry.getKey());
                }
            }
        }

        // Merge common sub-expression maps in dependency order:
        // 1. Inner project's existing common sub entries (deepest dependencies)
        // 2. New common sub entries from this merge (may depend on #1)
        // 3. Outer project's existing common sub entries, rewritten (may depend on #1 and #2)
        // LinkedHashMap preserves this insertion order so PlanFragmentBuilder
        // evaluates entries before their dependents.
        Map<ColumnRefOperator, ScalarOperator> mergedCommonSubMap = new LinkedHashMap<>();
        mergedCommonSubMap.putAll(secondProject.getCommonSubOperatorMap());
        mergedCommonSubMap.putAll(newCommonSubMap);
        for (Map.Entry<ColumnRefOperator, ScalarOperator> entry : firstProject.getCommonSubOperatorMap().entrySet()) {
            mergedCommonSubMap.put(entry.getKey(), rewriter.rewrite(entry.getValue()));
        }

        long limit = Stream.of(firstProject.getLimit(), secondProject.getLimit())
                .filter(l -> l >= 0)
                .min(Long::compare)
                .orElse(-1L);

        LogicalProjectOperator mergedProject =
                new LogicalProjectOperator(resultMap, mergedCommonSubMap, limit);
        OptExpression optExpression = new OptExpression(mergedProject);
        optExpression.getInputs().addAll(input.getInputs().get(0).getInputs());
        return Lists.newArrayList(optExpression);
    }
}
