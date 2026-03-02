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
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalProjectOperator;
import com.starrocks.sql.optimizer.operator.pattern.Pattern;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.rule.RuleType;

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

        // minimum value of limits on projections, but have to exclude unlimited(-1) case
        long limit = Stream.of(firstProject.getLimit(), secondProject.getLimit())
                .filter(l -> l >= 0)
                .min(Long::compare)
                .orElse(-1L);

        LogicalProjectOperator resultProject =
                new LogicalProjectOperator(firstProject.getColumnRefMap(), firstProject.getSubColumnRefMap(), limit);

        Map<ColumnRefOperator, ScalarOperator> subColumnRefMap = resultProject.getSubColumnRefMap();

        Stream.concat(secondProject.getColumnRefMap().entrySet().stream(), secondProject.getSubColumnRefMap().entrySet().stream())
                .forEach(entry -> {
                    ColumnRefOperator colRef = entry.getKey();
                    ScalarOperator expr = entry.getValue();
                    if (!colRef.equals(expr)) {
                        if (subColumnRefMap.containsKey(colRef)) {
                            if (!subColumnRefMap.get(colRef).equals(expr)) {
                                throw new IllegalStateException("Can't merge two projects: column " + colRef
                                        + " has conflicting expressions: existing=" + subColumnRefMap.get(colRef)
                                        + ", incoming=" + expr);
                            }
                        } else {
                            subColumnRefMap.put(colRef, expr);
                        }
                    }
                });

        resultProject.compactSubColumnRefMap();

        OptExpression optExpression = new OptExpression(resultProject);
        optExpression.getInputs().addAll(input.getInputs().get(0).getInputs());
        return Lists.newArrayList(optExpression);
    }
}
