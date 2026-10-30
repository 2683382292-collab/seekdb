/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_service.h"

#include "observer/ai_column/ob_ai_column_worker.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace observer
{

const int64_t ObAiColumnService::THREAD_CNT;

ObAiColumnService::ObAiColumnService()
  : rootserver::ObServerThreadHelper(), is_inited_(false)
{
}

ObAiColumnService::~ObAiColumnService()
{
  destroy();
}

int ObAiColumnService::server_module_init(ObAiColumnService *&service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(service)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("the ai column service is null", KR(ret));
  } else if (OB_FAIL(service->init())) {
    LOG_WARN("fail to init the ai column service", KR(ret));
  }
  return ret;
}

int ObAiColumnService::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("the ai column service is already initialized", KR(ret));
  } else if (OB_FAIL(ObServerThreadHelper::create("AIColumn", THREAD_CNT))) {
    LOG_WARN("fail to create the threads of the ai column service", KR(ret));
  } else {
    is_inited_ = true;
    LOG_INFO("the ai column service is initialized", "thread_cnt", THREAD_CNT);
  }
  return ret;
}

int ObAiColumnService::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column service is not initialized", KR(ret));
  } else if (OB_FAIL(ObServerThreadHelper::start())) {
    LOG_WARN("fail to start the ai column service", KR(ret));
  } else {
    LOG_INFO("the ai column service is started");
  }
  return ret;
}

void ObAiColumnService::do_work()
{
  int ret = OB_SUCCESS;
  ObAiColumnWorker worker;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column service is not initialized", KR(ret));
  } else if (OB_FAIL(worker.init())) {
    LOG_WARN("fail to init the ai column worker", KR(ret));
  } else {
    // One thread runs rounds until the service is stopped. run_once() never fails on a
    // task, only when the worker itself cannot make progress, and even then the thread
    // keeps generating the next round: an ai column must not be abandoned because one
    // round hit a bad definition.
    while (!has_set_stop()) {
      const int tmp_ret = worker.run_once();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to run one round of the ai column service", K(tmp_ret));
      }
      if (has_set_stop()) {
        break;
      }
      idle(ObAiColumnWorker::IDLE_INTERVAL_US);
    }
    worker.destroy();
  }
}

void ObAiColumnService::stop()
{
  is_inited_ = false;
  ObServerThreadHelper::stop();
  LOG_INFO("the ai column service is stopped");
}

void ObAiColumnService::wait()
{
  ObServerThreadHelper::wait();
  LOG_INFO("the ai column service is waited");
}

void ObAiColumnService::destroy()
{
  is_inited_ = false;
  ObServerThreadHelper::destroy();
}

} // namespace observer
} // namespace oceanbase
