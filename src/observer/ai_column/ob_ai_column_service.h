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

#pragma once

#include "rootserver/ob_server_thread_helper.h"

namespace oceanbase
{
namespace observer
{

// Background service that turns the asynchronous AI columns of a table into results.
//
// The service is a thread pool on top of ObServerThreadHelper: every thread owns one
// ObAiColumnWorker (and therefore one inner SQL session) and runs rounds until the
// server stops. The tasks themselves live in the table, in the hidden state columns of
// every row, so the service keeps no state that a restart could lose: a row that was
// claimed by a worker that died is picked up again as soon as its lease expires.
class ObAiColumnService : public rootserver::ObServerThreadHelper
{
public:
  // Threads generating the AI columns in parallel. They are independent, a slow model
  // call only holds one of them.
  static const int64_t THREAD_CNT = 4;

  ObAiColumnService();
  virtual ~ObAiColumnService();

  static int server_module_init(ObAiColumnService *&service);

  int init();
  int start();
  virtual void do_work() override;
  void stop();
  void wait();
  void destroy();

private:
  bool is_inited_;
};

} // namespace observer
} // namespace oceanbase
