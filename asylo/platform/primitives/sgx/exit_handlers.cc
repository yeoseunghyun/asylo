/*
 *
 * Copyright 2019 Asylo authors
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
 *
 */

#include "asylo/platform/primitives/sgx/exit_handlers.h"

#include "asylo/util/logging.h"
#include "asylo/platform/primitives/untrusted_primitives.h"
#include "asylo/util/thread.h"

namespace asylo {
namespace primitives {
namespace {

void donate_thread(Client *sgx_client) {
  primitives::MessageReader out;
  Status status =
      sgx_client->EnclaveCall(kSelectorAsyloDonateThread, nullptr, &out);
  if (!out.empty()) {
    LOG(ERROR) << "Unexpected output size received from EnclaveCall to "
                  "kSelectorAsyloDonateThread";
    abort();
  }
  if (!status.ok()) {
    LOG(ERROR) << "EnclaveCall to kSelectorAsyloDonateThread failed.";
  }
}

}  // namespace

Status CreateThreadHandler(const std::shared_ptr<primitives::Client> &client,
                           void *context, primitives::MessageReader *input,
                           primitives::MessageWriter *output) {
  Thread::StartDetached(donate_thread, client.get());

  output->Push<int>(0);
  return Status::OkStatus();
}

Status RegisterSgxExitHandlers(Client::ExitCallProvider *exit_call_provider) {
  if (!exit_call_provider) {
    return {error::GoogleError::INVALID_ARGUMENT,
            "RegisterSgxExitHandlers: Invalid/NULL ExitCallProvider provided."};
  }

  ASYLO_RETURN_IF_ERROR(exit_call_provider->RegisterExitHandler(
      kSelectorCreateThread, ExitHandler{CreateThreadHandler}));

  return Status::OkStatus();
}

}  // namespace primitives
}  // namespace asylo