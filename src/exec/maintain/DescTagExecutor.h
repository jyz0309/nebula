/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#ifndef EXEC_MAINTAIN_DESCTAGEXECUTOR_H_
#define EXEC_MAINTAIN_DESCTAGEXECUTOR_H_

#include "exec/Executor.h"

namespace nebula {
namespace graph {

class DescTagExecutor final : public Executor {
public:
    DescTagExecutor(const PlanNode *node, ExecutionContext *ectx)
        : Executor("DescTagExecutor", node, ectx) {}

    folly::Future<Status> execute() override;

private:
    folly::Future<Status> descTag();
};

}   // namespace graph
}   // namespace nebula

#endif   // EXEC_MAINTAIN_DESCTAGEXECUTOR_H_