/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#include "graph/service/QueryInstance.h"

#include "common/base/Base.h"
#include "common/stats/StatsManager.h"
#include "common/time/ScopedTimer.h"
#include "graph/executor/ExecutionError.h"
#include "graph/executor/Executor.h"
#include "graph/optimizer/OptRule.h"
#include "graph/planner/plan/ExecutionPlan.h"
#include "graph/planner/plan/PlanNode.h"
#include "graph/scheduler/AsyncMsgNotifyBasedScheduler.h"
#include "graph/scheduler/Scheduler.h"
#include "graph/stats/GraphStats.h"
#include "graph/util/AstUtils.h"
#include "graph/validator/Validator.h"
#include "parser/ExplainSentence.h"
#include "parser/Sentence.h"
#include "parser/SequentialSentences.h"

using nebula::opt::Optimizer;
using nebula::opt::OptRule;
using nebula::opt::RuleSet;

namespace nebula {
namespace graph {

QueryInstance::QueryInstance(std::unique_ptr<QueryContext> qctx, Optimizer *optimizer) {
  qctx_ = std::move(qctx);
  optimizer_ = DCHECK_NOTNULL(optimizer);
  scheduler_ = std::make_unique<AsyncMsgNotifyBasedScheduler>(qctx_.get());
  qctx_->rctx()->session()->addQuery(qctx_.get());
}

void QueryInstance::execute() {
  Status status = validateAndOptimize();
  if (!status.ok()) {
    onError(std::move(status));
    return;
  }

  if (!explainOrContinue()) {
    onFinish();
    return;
  }

  scheduler_->schedule()
      .thenValue([this](Status s) {
        if (s.ok()) {
          this->onFinish();
        } else {
          this->onError(std::move(s));
        }
      })
      .thenError(folly::tag_t<ExecutionError>{},
                 [this](const ExecutionError &e) { onError(e.status()); })
      .thenError(folly::tag_t<std::exception>{},
                 [this](const std::exception &e) { onError(Status::Error("%s", e.what())); });
}

Status QueryInstance::validateAndOptimize() {
  auto *rctx = qctx()->rctx();
  auto &spaceName = rctx->session()->space().name;
  VLOG(1) << "Parsing query: " << rctx->query();
  auto result = GQLParser(qctx()).parse(rctx->query());
  NG_RETURN_IF_ERROR(result);
  sentence_ = std::move(result).value();
  if (sentence_->kind() == Sentence::Kind::kSequential) {
    size_t num = static_cast<const SequentialSentences *>(sentence_.get())->numSentences();
    stats::StatsManager::addValue(kNumSentences, num);
    if (FLAGS_enable_space_level_metrics && spaceName != "") {
      stats::StatsManager::addValue(
          stats::StatsManager::counterWithLabels(kNumSentences, {{"space", spaceName}}), num);
    }
  } else {
    stats::StatsManager::addValue(kNumSentences);
    if (FLAGS_enable_space_level_metrics && spaceName != "") {
      stats::StatsManager::addValue(
          stats::StatsManager::counterWithLabels(kNumSentences, {{"space", spaceName}}));
    }
  }

  NG_RETURN_IF_ERROR(Validator::validate(sentence_.get(), qctx()));
  NG_RETURN_IF_ERROR(findBestPlan());
  stats::StatsManager::addValue(kOptimizerLatencyUs, *(qctx_->plan()->optimizeTimeInUs()));
  if (FLAGS_enable_space_level_metrics && spaceName != "") {
    stats::StatsManager::addValue(
        stats::StatsManager::histoWithLabels(kOptimizerLatencyUs, {{"space", spaceName}}));
  }

  return Status::OK();
}

bool QueryInstance::explainOrContinue() {
  if (sentence_->kind() != Sentence::Kind::kExplain) {
    return true;
  }
  auto &resp = qctx_->rctx()->resp();
  resp.planDesc = std::make_unique<PlanDescription>();
  DCHECK_NOTNULL(qctx_->plan())->describe(resp.planDesc.get());
  return static_cast<const ExplainSentence *>(sentence_.get())->isProfile();
}

void QueryInstance::onFinish() {
  auto rctx = qctx()->rctx();
  VLOG(1) << "Finish query: " << rctx->query();
  auto &spaceName = rctx->session()->space().name;
  rctx->resp().spaceName = std::make_unique<std::string>(spaceName);

  fillRespData(&rctx->resp());

  auto latency = rctx->duration().elapsedInUSec();
  rctx->resp().latencyInUs = latency;
  addSlowQueryStats(latency, spaceName);
  rctx->finish();

  rctx->session()->deleteQuery(qctx_.get());
  // The `QueryInstance' is the root node holding all resources during the
  // execution. When the whole query process is done, it's safe to release this
  // object, as long as no other contexts have chances to access these resources
  // later on, e.g. previously launched uncompleted async sub-tasks, EVEN on
  // failures.
  delete this;
}

void QueryInstance::onError(Status status) {
  LOG(ERROR) << status;
  auto *rctx = qctx()->rctx();
  auto &spaceName = rctx->session()->space().name;
  switch (status.code()) {
    case Status::Code::kOk:
      rctx->resp().errorCode = ErrorCode::SUCCEEDED;
      break;
    case Status::Code::kSyntaxError:
      rctx->resp().errorCode = ErrorCode::E_SYNTAX_ERROR;
      break;
    case Status::Code::kStatementEmpty:
      rctx->resp().errorCode = ErrorCode::E_STATEMENT_EMPTY;
      break;
    case Status::Code::kSemanticError:
      rctx->resp().errorCode = ErrorCode::E_SEMANTIC_ERROR;
      break;
    case Status::Code::kPermissionError:
      rctx->resp().errorCode = ErrorCode::E_BAD_PERMISSION;
      break;
    case Status::Code::kLeaderChanged:
      stats::StatsManager::addValue(kNumQueryErrorsLeaderChanges);
      if (FLAGS_enable_space_level_metrics && spaceName != "") {
        stats::StatsManager::addValue(stats::StatsManager::counterWithLabels(
            kNumQueryErrorsLeaderChanges, {{"space", spaceName}}));
      }
      [[fallthrough]];
    case Status::Code::kBalanced:
    case Status::Code::kEdgeNotFound:
    case Status::Code::kError:
    case Status::Code::kHostNotFound:
    case Status::Code::kIndexNotFound:
    case Status::Code::kInserted:
    case Status::Code::kKeyNotFound:
    case Status::Code::kPartialSuccess:
    case Status::Code::kNoSuchFile:
    case Status::Code::kNotSupported:
    case Status::Code::kPartNotFound:
    case Status::Code::kSpaceNotFound:
    case Status::Code::kGroupNotFound:
    case Status::Code::kZoneNotFound:
    case Status::Code::kTagNotFound:
    case Status::Code::kUserNotFound:
    case Status::Code::kListenerNotFound:
    case Status::Code::kSessionNotFound:
      rctx->resp().errorCode = ErrorCode::E_EXECUTION_ERROR;
      break;
  }
  rctx->resp().spaceName = std::make_unique<std::string>(spaceName);
  rctx->resp().errorMsg = std::make_unique<std::string>(status.toString());
  auto latency = rctx->duration().elapsedInUSec();
  rctx->resp().latencyInUs = latency;
  stats::StatsManager::addValue(kNumQueryErrors);
  if (FLAGS_enable_space_level_metrics && spaceName != "") {
    stats::StatsManager::addValue(
        stats::StatsManager::counterWithLabels(kNumQueryErrors, {{"space", spaceName}}));
  }
  addSlowQueryStats(latency, spaceName);
  rctx->session()->deleteQuery(qctx_.get());
  rctx->finish();
  delete this;
}

void QueryInstance::addSlowQueryStats(uint64_t latency, const std::string &spaceName) const {
  stats::StatsManager::addValue(kQueryLatencyUs, latency);
  if (FLAGS_enable_space_level_metrics && spaceName != "") {
    stats::StatsManager::addValue(
        stats::StatsManager::histoWithLabels(kQueryLatencyUs, {{"space", spaceName}}), latency);
  }
  if (latency > static_cast<uint64_t>(FLAGS_slow_query_threshold_us)) {
    stats::StatsManager::addValue(kNumSlowQueries);
    stats::StatsManager::addValue(kSlowQueryLatencyUs, latency);
    if (FLAGS_enable_space_level_metrics && spaceName != "") {
      stats::StatsManager::addValue(
          stats::StatsManager::counterWithLabels(kNumSlowQueries, {{"space", spaceName}}));
      stats::StatsManager::addValue(
          stats::StatsManager::histoWithLabels(kSlowQueryLatencyUs, {{"space", spaceName}}),
          latency);
    }
  }
}

void QueryInstance::fillRespData(ExecutionResponse *resp) {
  auto ectx = DCHECK_NOTNULL(qctx_->ectx());
  auto plan = DCHECK_NOTNULL(qctx_->plan());
  const auto &name = plan->root()->outputVar();
  if (!ectx->exist(name)) return;

  auto &&value = ectx->moveValue(name);
  if (!value.isDataSet()) return;

  // fill dataset
  auto result = value.moveDataSet();
  if (!result.colNames.empty()) {
    resp->data = std::make_unique<DataSet>(std::move(result));
  } else {
    resp->errorCode = ErrorCode::E_EXECUTION_ERROR;
    resp->errorMsg = std::make_unique<std::string>("Internal error: empty column name list");
    LOG(ERROR) << "Empty column name list";
  }
}

Status QueryInstance::findBestPlan() {
  auto plan = qctx_->plan();
  SCOPED_TIMER(plan->optimizeTimeInUs());
  auto rootStatus = optimizer_->findBestPlan(qctx_.get());
  NG_RETURN_IF_ERROR(rootStatus);
  auto root = std::move(rootStatus).value();
  plan->setRoot(const_cast<PlanNode *>(root));
  return Status::OK();
}

}  // namespace graph
}  // namespace nebula
