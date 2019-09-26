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

#ifndef ASYLO_PLATFORM_CORE_TRUSTED_APPLICATION_H_
#define ASYLO_PLATFORM_CORE_TRUSTED_APPLICATION_H_

// Defines a high-level interface for constructing enclave applications.

#include <string>

#include "asylo/enclave.pb.h"
#include "asylo/platform/arch/fork.pb.h"
#include "asylo/platform/core/entry_points.h"
#include "asylo/platform/core/trusted_global_state.h"
#include "asylo/util/status.h"

namespace asylo {

/// Abstract base class for trusted applications.
///
/// To implement an enclave application, client code declares a
/// TrustedApplication and implements the entry points it wishes to handle. For
/// example:
///
/// ```
/// class HelloWorld : public TrustedApplication {
///  public:
///   Status Initialize(const EnclaveConfig &config) override {
///     enc_untrusted_puts("Hello!");
///     return Status::OkStatus();
///   }
///
///   Status Run(const EnclaveInput &input, EnclaveOutput *output) override {
///     enc_untrusted_puts("Running!");
///     return Status::OkStatus();
///   }
///
///   Status Finalize(const EnclaveFinal &fini) override {
///     enc_untrusted_puts("Goodbye!");
///     return Status::OkStatus();
///   }
/// };
/// ```
///
/// At startup, the runtime will call the user supplied function
/// BuildTrustedApplication and install the returned instance as the handler for
/// enclave entries events. For instance:
///
/// ```
/// TrustedApplication *BuildTrustedApplication() {
///   return new HelloWorld;
/// }
/// ```
///
/// Note that types derived from TrustedApplication must be trivially
/// destructible, and any such destructor will never be invoked by the runtime.
class TrustedApplication {
 public:
  /// An enumeration of possible enclave runtime states.
  enum class State {
    /// Enclave initialization has not started.
    kUninitialized,
    /// Asylo internals are initializing.
    kInternalInitializing,
    /// Asylo internals are initialized. User-defined initialization is
    /// in-progress.
    kUserInitializing,
    /// All initialization has completed. The enclave is running.
    kRunning,
    /// The enclave is finalizing.
    kFinalizing,
    /// The enclave has finalized.
    kFinalized,
  };

  /// \private
  Status InitializeInternal(const EnclaveConfig &config);

  /// Implements enclave initialization entry-point.
  ///
  /// \param config The configuration used to initialize the enclave.
  /// \return An OK status or an error if the enclave could not be initialized.
  /// \anchor initialize
  virtual Status Initialize(const EnclaveConfig &config) {
    return Status::OkStatus();
  }

  /// Implements enclave execution entry-point.
  ///
  /// \param input Message passed to determine behavior for the Run routine.
  /// \param output Message passed back to the untrusted caller.
  /// \return OK status or error
  /// \anchor run
  virtual Status Run(const EnclaveInput &input, EnclaveOutput *output) {
    return Status::OkStatus();
  }

  /// Implements enclave finalization behavior.
  ///
  /// \param final_input Message passed on enclave finalization.
  /// \return OK status or error
  /// \anchor finalize
  virtual Status Finalize(const EnclaveFinal &final_input) {
    return Status::OkStatus();
  }

  /// Trivial destructor.
  ///
  /// Trivial destructor. Note that classes derived from of TrustedApplication
  /// must not add a non-trivial destructor, as they will not be called by the
  /// enclave runtime.
  virtual ~TrustedApplication() = default;

  /// Returns the enclave state in a thread-safe manner.
  State GetState() LOCKS_EXCLUDED(mutex_);

 private:
  // Tracks the current enclave state.
  State enclave_state_ GUARDED_BY(mutex_) = State::kUninitialized;
  absl::Mutex mutex_;

  // Verifies the expected enclave state and sets a new one in thread-safe
  // manner. Returns error if the verification fails.
  asylo::Status VerifyAndSetState(const State &expected_state,
                                  const State &new_state)
      LOCKS_EXCLUDED(mutex_);

  // Sets the enclave state in thread-safe manner.
  void SetState(const State &state) LOCKS_EXCLUDED(mutex_);

  friend int __asylo_user_init(const char *name, const char *config,
                               size_t config_len, char **output,
                               size_t *output_len);
  friend int __asylo_user_run(const char *input, size_t input_len,
                              char **output, size_t *output_len);
  friend int __asylo_user_fini(const char *input, size_t input_len,
                               char **output, size_t *output_len);
  friend int __asylo_threading_donate();
  friend int __asylo_handle_signal(const char *input, size_t input_len);
  friend int __asylo_take_snapshot(char **output, size_t *output_len);
  friend int __asylo_restore(const char *input, size_t input_len, char **output,
                             size_t *output_len);
  friend int __asylo_transfer_secure_snapshot_key(const char *input,
                                                  size_t input_len,
                                                  char **output,
                                                  size_t *output_len);
  friend int __asylo_initiate_migration();
};

/// User-supplied factory function for making a trusted application instance.
///
/// \return A new TrustedApplication instance, or nullptr on failure.
/// \relates TrustedApplication
TrustedApplication *BuildTrustedApplication();

/// Returns the trusted application instance.
///
/// \return The enclave application instance or nullptr on failure.
/// \relates TrustedApplication
TrustedApplication *GetApplicationInstance();

}  // namespace asylo

#endif  // ASYLO_PLATFORM_CORE_TRUSTED_APPLICATION_H_
