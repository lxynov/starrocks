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

package com.starrocks.common;

import com.starrocks.thrift.TStatusCode;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

public class StatusTest {

    @Test
    public void testPrecedenceOrdering() {
        // OK < cascade < initiated < root cause
        Assertions.assertTrue(Status.precedence(TStatusCode.OK) < Status.precedence(TStatusCode.CANCELLED));
        Assertions.assertTrue(Status.precedence(TStatusCode.CANCELLED) < Status.precedence(TStatusCode.TIMEOUT));
        Assertions.assertTrue(Status.precedence(TStatusCode.TIMEOUT) < Status.precedence(TStatusCode.MEM_LIMIT_EXCEEDED));
    }

    @Test
    public void testCascadeStatusesRankLowest() {
        int cascade = Status.PRECEDENCE_CASCADE;
        Assertions.assertEquals(cascade, Status.precedence(TStatusCode.CANCELLED));
        Assertions.assertEquals(cascade, Status.precedence(TStatusCode.THRIFT_RPC_ERROR));
        Assertions.assertEquals(cascade, Status.precedence(TStatusCode.SERVICE_UNAVAILABLE));
        Assertions.assertEquals(cascade, Status.precedence(TStatusCode.SR_EAGAIN));
    }

    @Test
    public void testInitiatedStatuses() {
        int initiated = Status.PRECEDENCE_INITIATED;
        Assertions.assertEquals(initiated, Status.precedence(TStatusCode.TIMEOUT));
        Assertions.assertEquals(initiated, Status.precedence(TStatusCode.ABORTED));
        Assertions.assertEquals(initiated, Status.precedence(TStatusCode.SHUTDOWN));
    }

    @Test
    public void testRootCauseFaultsRankHighest() {
        int rootCause = Status.PRECEDENCE_ROOT_CAUSE;
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.MEM_LIMIT_EXCEEDED));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.DATA_QUALITY_ERROR));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.IO_ERROR));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.INTERNAL_ERROR));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.GLOBAL_DICT_NOT_MATCH));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.REMOTE_FILE_NOT_FOUND));
        // Unmapped codes default to root cause so real errors are never masked.
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.CAPACITY_LIMIT_EXCEED));
        Assertions.assertEquals(rootCause, Status.precedence(TStatusCode.NOT_AUTHORIZED));
    }

    @Test
    public void testNullDefaultsToRootCause() {
        Assertions.assertEquals(Status.PRECEDENCE_ROOT_CAUSE, Status.precedence(null));
    }
}
