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

import com.google.common.collect.Maps;
import com.starrocks.sql.optimizer.OptExpression;
import com.starrocks.sql.optimizer.OptimizerFactory;
import com.starrocks.sql.optimizer.base.ColumnRefFactory;
import com.starrocks.sql.optimizer.operator.OperatorType;
import com.starrocks.sql.optimizer.operator.logical.LogicalProjectOperator;
import com.starrocks.sql.optimizer.operator.scalar.CallOperator;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ConstantOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.type.BooleanType;
import com.starrocks.type.IntegerType;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.Map;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class MergeTwoProjectRuleTest {

    @Test
    public void testMergeAndLimit() {
        // Bottom project: {b -> a, c -> 5}
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);
        ColumnRefOperator c = new ColumnRefOperator(3, IntegerType.INT, "c", true);

        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, a);
        bottomMap.put(c, ConstantOperator.createInt(5));
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, 20);

        // Top project: {x -> b, y -> c}
        ColumnRefOperator x = new ColumnRefOperator(4, IntegerType.INT, "x", true);
        ColumnRefOperator y = new ColumnRefOperator(5, IntegerType.INT, "y", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        topMap.put(y, c);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, 10);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        // Validate: the result is a single Project with remapped expressions and min limit (10)
        assertEquals(1, out.size());
        assertEquals(OperatorType.LOGICAL_PROJECT, out.get(0).getOp().getOpType());
        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();
        assertEquals(10, result.getLimit());

        Map<ColumnRefOperator, ScalarOperator> resMap = result.getColumnRefMap();
        // x -> a (because b -> a in bottom)
        assertInstanceOf(ColumnRefOperator.class, resMap.get(x));
        assertEquals(a, resMap.get(x));
        // y -> 5 (because c -> 5 in bottom)
        assertInstanceOf(ConstantOperator.class, resMap.get(y));
        assertEquals(5, ((ConstantOperator) resMap.get(y)).getInt());

        // The child of the merged project should be the child of bottom project (no projects below)
        assertEquals(0, out.get(0).getInputs().size());
    }

    @Test
    public void testUnlimitedAndLimit() {
        // Bottom project has unlimited (-1), top has 7 -> result should be 7
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);

        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, a);
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        ColumnRefOperator x = new ColumnRefOperator(3, IntegerType.INT, "x", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, 7);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();
        assertEquals(7, result.getLimit());
    }

    @Test
    public void testPreserveAssertTrue() {
        // Bottom project contains an ASSERT_TRUE() call; it should be preserved in the result
        ColumnRefOperator a = new ColumnRefOperator(1, BooleanType.BOOLEAN, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, BooleanType.BOOLEAN, "b", true);

        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        // Construct a CallOperator with fnName = FunctionSet.ASSERT_TRUE, return type BOOLEAN, and one arg
        CallOperator assertTrue = new CallOperator(
                com.starrocks.catalog.FunctionSet.ASSERT_TRUE,
                BooleanType.BOOLEAN,
                java.util.List.of(a)
        );
        bottomMap.put(b, assertTrue);
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        // Top project just references b -> should not remove the ASSERT_TRUE entry for b
        ColumnRefOperator x = new ColumnRefOperator(3, BooleanType.BOOLEAN, "x", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, -1);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();
        Map<ColumnRefOperator, ScalarOperator> resMap = result.getColumnRefMap();

        // x should be rewritten to ASSERT_TRUE(a) as it references b which maps to ASSERT_TRUE(a)
        assertInstanceOf(CallOperator.class, resMap.get(x));
        assertEquals(com.starrocks.catalog.FunctionSet.ASSERT_TRUE, ((CallOperator) resMap.get(x)).getFnName());

        // And the original b -> ASSERT_TRUE(a) mapping from the lower project must be preserved per rule
        assertInstanceOf(CallOperator.class, resMap.get(b));
        assertEquals(com.starrocks.catalog.FunctionSet.ASSERT_TRUE, ((CallOperator) resMap.get(b)).getFnName());
    }

    @Test
    public void testMultiRefInnerColumnBecomesCommonSub() {
        // Bottom project: b -> expensive(a)  (non-trivial expression)
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);

        CallOperator expensiveCall = new CallOperator("expensive_func", IntegerType.INT, List.of(a));
        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, expensiveCall);
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        // Top project: x -> b, y -> b  (two output expressions both reference b)
        ColumnRefOperator x = new ColumnRefOperator(3, IntegerType.INT, "x", true);
        ColumnRefOperator y = new ColumnRefOperator(4, IntegerType.INT, "y", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        topMap.put(y, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, -1);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();
        Map<ColumnRefOperator, ScalarOperator> resMap = result.getColumnRefMap();
        Map<ColumnRefOperator, ScalarOperator> commonSubMap = result.getCommonSubOperatorMap();

        // b should be kept as a common sub-expression (not inlined into both x and y)
        assertTrue(commonSubMap.containsKey(b), "Multi-ref non-trivial inner column should be in commonSubOperatorMap");
        assertEquals(expensiveCall, commonSubMap.get(b));

        // x and y should still reference b (not the inlined expression)
        assertEquals(b, resMap.get(x));
        assertEquals(b, resMap.get(y));
    }

    @Test
    public void testSingleRefInnerColumnIsInlined() {
        // Bottom project: b -> expensive(a)
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);

        CallOperator expensiveCall = new CallOperator("expensive_func", IntegerType.INT, List.of(a));
        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, expensiveCall);
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        // Top project: x -> b  (only one reference to b)
        ColumnRefOperator x = new ColumnRefOperator(3, IntegerType.INT, "x", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, -1);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();
        Map<ColumnRefOperator, ScalarOperator> resMap = result.getColumnRefMap();
        Map<ColumnRefOperator, ScalarOperator> commonSubMap = result.getCommonSubOperatorMap();

        // Single-ref column should be inlined, not kept as common sub
        assertTrue(commonSubMap.isEmpty(), "Single-ref column should not produce common sub entries");
        assertInstanceOf(CallOperator.class, resMap.get(x));
        assertEquals("expensive_func", ((CallOperator) resMap.get(x)).getFnName());
    }

    @Test
    public void testTrivialMultiRefIsStillInlined() {
        // Bottom project: b -> a  (trivial: just a column reference)
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);

        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, a);
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        // Top project: x -> b, y -> b  (two references, but inner expr is trivial)
        ColumnRefOperator x = new ColumnRefOperator(3, IntegerType.INT, "x", true);
        ColumnRefOperator y = new ColumnRefOperator(4, IntegerType.INT, "y", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        topMap.put(y, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, -1);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();

        // Trivial expressions (column refs, constants) should always be inlined regardless of ref count
        assertTrue(result.getCommonSubOperatorMap().isEmpty(),
                "Trivial (column ref) inner expression should be inlined, not kept as common sub");
        assertEquals(a, result.getColumnRefMap().get(x));
        assertEquals(a, result.getColumnRefMap().get(y));
    }

    @Test
    public void testConstantMultiRefIsStillInlined() {
        // Bottom project: b -> 42  (trivial: constant)
        ColumnRefOperator b = new ColumnRefOperator(1, IntegerType.INT, "b", true);

        Map<ColumnRefOperator, ScalarOperator> bottomMap = Maps.newHashMap();
        bottomMap.put(b, ConstantOperator.createInt(42));
        LogicalProjectOperator bottomProject = new LogicalProjectOperator(bottomMap, -1);

        // Top project: x -> b, y -> b
        ColumnRefOperator x = new ColumnRefOperator(2, IntegerType.INT, "x", true);
        ColumnRefOperator y = new ColumnRefOperator(3, IntegerType.INT, "y", true);
        Map<ColumnRefOperator, ScalarOperator> topMap = Maps.newHashMap();
        topMap.put(x, b);
        topMap.put(y, b);
        LogicalProjectOperator topProject = new LogicalProjectOperator(topMap, -1);

        OptExpression top = new OptExpression(topProject);
        top.getInputs().add(OptExpression.create(bottomProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();

        assertTrue(result.getCommonSubOperatorMap().isEmpty(),
                "Constant inner expression should be inlined, not kept as common sub");
    }

    @Test
    public void testCommonSubPropagatedFromInnerProject() {
        // Inner project already has commonSubOperatorMap from a previous merge.
        // Verify that it's propagated to the merged project.
        ColumnRefOperator a = new ColumnRefOperator(1, IntegerType.INT, "a", true);
        ColumnRefOperator b = new ColumnRefOperator(2, IntegerType.INT, "b", true);
        ColumnRefOperator csub = new ColumnRefOperator(10, IntegerType.INT, "csub", true);

        // Inner project: columnRefMap = {b -> csub}, commonSubOperatorMap = {csub -> expensive(a)}
        CallOperator expensiveCall = new CallOperator("expensive_func", IntegerType.INT, List.of(a));
        Map<ColumnRefOperator, ScalarOperator> innerMap = Maps.newHashMap();
        innerMap.put(b, csub);
        Map<ColumnRefOperator, ScalarOperator> innerCommonSub = Maps.newHashMap();
        innerCommonSub.put(csub, expensiveCall);
        LogicalProjectOperator innerProject = new LogicalProjectOperator(innerMap, innerCommonSub, -1);

        // Outer project: x -> b  (single ref, so b is inlined)
        ColumnRefOperator x = new ColumnRefOperator(3, IntegerType.INT, "x", true);
        Map<ColumnRefOperator, ScalarOperator> outerMap = Maps.newHashMap();
        outerMap.put(x, b);
        LogicalProjectOperator outerProject = new LogicalProjectOperator(outerMap, -1);

        OptExpression top = new OptExpression(outerProject);
        top.getInputs().add(OptExpression.create(innerProject));

        MergeTwoProjectRule rule = new MergeTwoProjectRule();
        List<OptExpression> out = rule.transform(top, OptimizerFactory.mockContext(new ColumnRefFactory()));

        LogicalProjectOperator result = (LogicalProjectOperator) out.get(0).getOp();

        // b is single-ref so inlined: x -> csub
        assertEquals(csub, result.getColumnRefMap().get(x));
        // Inner project's common sub should be propagated
        assertTrue(result.getCommonSubOperatorMap().containsKey(csub));
        assertEquals(expensiveCall, result.getCommonSubOperatorMap().get(csub));
    }
}
