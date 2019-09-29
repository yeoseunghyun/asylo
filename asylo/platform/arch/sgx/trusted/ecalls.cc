/*
 *
 * Copyright 2017 Asylo authors
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

// Stubs invoked by edger8r-generated code for calls into the enclave.


#include <cerrno>
#include <cstring>
#include <string>

#include "absl/strings/str_cat.h"
#include "asylo/enclave.pb.h"
#include "asylo/util/logging.h"
#include "asylo/platform/arch/include/trusted/host_calls.h"
#include "asylo/platform/arch/include/trusted/fork.h"
#include "asylo/platform/arch/sgx/trusted/generated_bridge_t.h"
#include "asylo/platform/common/bridge_types.h"
#include "asylo/platform/core/entry_points.h"
#include "asylo/platform/primitives/sgx/trusted_sgx.h"
#include "asylo/util/posix_error_space.h"
#include "asylo/util/status.h"
#include "include/sgx_trts.h"

// Edger8r does basic sanity checks for input and output pointers. The
// parameters passed by the untrusted caller are copied by the edger8r-generated
// code into trusted memory and then passed here. Consequently, there is no
// possibility for TOCTOU attacks on these parameters.

// Invokes the enclave signal handling entry-point. Returns a non-zero error
// code on failure.
int ecall_handle_signal(const char *input, bridge_size_t input_len) {
  int result = 0;
  try {
    result =
        asylo::__asylo_handle_signal(input, static_cast<size_t>(input_len));
  } catch (...) {
    // Abort directly here instead of LOG(FATAL). LOG tries to obtain a mutex,
    // and acquiring a non-reentrant mutex in signal handling may cause deadlock
    // if the thread had already obtained that mutex when interrupted.
    abort();
  }
  return result;
}

// Invokes the enclave snapshotting entry-point. Returns a non-zero error code
// on failure.
int ecall_take_snapshot(char **output, bridge_size_t *output_len) {
  int result = 0;
  size_t tmp_output_len = 0;
  try {
    result =
        asylo::__asylo_take_snapshot(output, &tmp_output_len);
  } catch (...) {
    LOG(FATAL) << "Uncaught exception in enclave";
  }

  if (output_len) {
    *output_len = static_cast<bridge_size_t>(tmp_output_len);
  }
  return result;
}

// Invokes the enclave restoring entry-point. Returns a non-zero error code on
// failure.
int ecall_restore(const char *input, bridge_size_t input_len, char **output,
                  bridge_size_t *output_len) {
  int result = 0;
  size_t tmp_output_len = 0;
  try {
    result = asylo::__asylo_restore(input, static_cast<size_t>(input_len),
                                    output, &tmp_output_len);
  } catch (...) {
    LOG(FATAL) << "Uncaught exception in enclave";
  }

  if (output_len) {
    *output_len = static_cast<bridge_size_t>(tmp_output_len);
  }
  return result;
}

// Invokes the enclave secure snapshot key transfer entry-point. Returns a
// non-zero error code on failure.
int ecall_transfer_secure_snapshot_key(const char *input,
                                       bridge_size_t input_len, char **output,
                                       bridge_size_t *output_len) {
  int result = 0;
  bridge_size_t bridge_output_len;
  try {
    result = asylo::__asylo_transfer_secure_snapshot_key(
        input, static_cast<size_t>(input_len), output, &bridge_output_len);
  } catch (...) {
    LOG(FATAL) << "Uncaught exception in enclave";
  }
  if (output_len) {
    *output_len = static_cast<size_t>(bridge_output_len);
  }
  return result;
}

int ecall_initiate_migration() {
  int result = 0;

  // for saving thread layout & set fork requested
  asylo::SaveThreadLayoutForSnapshot();
  asylo::SetForkRequested();

  try {
    result = asylo::__asylo_initiate_migration();
  } catch (...) {
    LOG(FATAL) << "Uncaught exception in enclave";
  }

  return result;
}

// Invokes the trusted entry point designated by |selector|. Returns a
// non-zero error code on failure.
int ecall_dispatch_trusted_call(uint64_t selector, void *buffer) {
  return asylo::primitives::asylo_enclave_call(selector, buffer);
}
