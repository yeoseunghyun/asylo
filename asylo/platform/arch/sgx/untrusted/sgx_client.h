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

#ifndef ASYLO_PLATFORM_ARCH_SGX_UNTRUSTED_SGX_CLIENT_H_
#define ASYLO_PLATFORM_ARCH_SGX_UNTRUSTED_SGX_CLIENT_H_

#include <string>

#include "absl/base/macros.h"
#include "asylo/enclave.pb.h"
#include "asylo/platform/arch/fork.pb.h"
#include "asylo/platform/core/enclave_client.h"
#include "asylo/platform/core/enclave_manager.h"
#include "asylo/platform/primitives/sgx/untrusted_sgx.h"
#include "asylo/util/status.h"
#include "asylo/util/statusor.h"
#include "include/sgx_urts.h"

namespace asylo {

/// Enclave client for Intel Software Guard Extensions (SGX) based enclaves.
class SgxClient : public EnclaveClient {
 public:
  SgxClient() = delete;

  explicit SgxClient(const std::string &name) : EnclaveClient(name) {}
  Status EnterAndRun(const EnclaveInput &input, EnclaveOutput *output) override;

  // Returns true when a TCS is active in simulation mode. Always returns false
  // in hardware mode, since TCS active/inactive state is only set and used in
  // simulation mode.
  bool IsTcsActive();

  void *base_address() { return primitive_sgx_client_->GetBaseAddress(); }

  const void *base_address() const {
    return primitive_sgx_client_->GetBaseAddress();
  }

  size_t size() { return primitive_sgx_client_->GetEnclaveSize(); }

  // Sets a new expected process ID for an existing SGX enclave.
  void SetProcessId();

  // Returns the primitive SGX client.
  std::shared_ptr<primitives::SgxEnclaveClient> GetPrimitivesClient() {
    return primitive_sgx_client_;
  }
  Status EnterAndTakeSnapshot(SnapshotLayout *snapshot_layout);
  Status EnterAndRestore(const SnapshotLayout &snapshot_layout);
  Status EnterAndTransferSecureSnapshotKey(
      const ForkHandshakeConfig &fork_handshake_config);
  Status InitiateMigration();

 private:
  friend class SgxLoader;
  friend class SgxEmbeddedLoader;

  Status EnterAndInitialize(const EnclaveConfig &config) override;
  Status EnterAndFinalize(const EnclaveFinal &final_input) override;
  Status EnterAndDonateThread() override;
  Status EnterAndHandleSignal(const EnclaveSignal &signal) override;
  Status DestroyEnclave() override;

  // Calls the enclave initialization routine.
  Status Initialize(const char *name, size_t name_len, const char *input,
                    size_t input_len, char **output, size_t *output_len);
  Status Run(const char *input, size_t input_len, char **output,
             size_t *output_len);
  Status Finalize(const char *input, size_t input_len, char **output,
             size_t *output_len);
  // Primitive SGX client.
  std::shared_ptr<primitives::SgxEnclaveClient> primitive_sgx_client_;
};

/// Enclave loader for Intel Software Guard Extensions (SGX) based enclaves
/// located in shared object files read from the file system.
class SgxLoader : public EnclaveLoader {
 public:
  /// Constructs an SgxLoader for an enclave object file on the file system,
  /// optionally in debug mode.
  ///
  /// \param path The path to the enclave binary (.so) file to load.
  /// \param debug Whether to load the enclave in debug mode.
  SgxLoader(const std::string &path, bool debug)
      : enclave_path_(path), debug_(debug) {}

 private:
  StatusOr<std::unique_ptr<EnclaveClient>> LoadEnclave(
      const std::string &name, void *base_address, const size_t enclave_size,
      const EnclaveConfig &config) const override;

  StatusOr<std::unique_ptr<EnclaveLoader>> Copy() const override;

  const std::string enclave_path_;
  const bool debug_;
};

/// Enclave loader for Intel Software Guard Extensions (SGX) based enclaves
/// embedded in the binary of the calling process.
class SgxEmbeddedLoader : public EnclaveLoader {
 public:
  /// Constructs an SgxEmbeddedLoader for an enclave object embedded in the
  /// binary of the calling process.
  ///
  /// \param elf_section_name The name of the ELF section containing the
  ///                         enclave.
  /// \param debug Whether to load the enclave in debug mode.
  SgxEmbeddedLoader(const std::string &elf_section_name, bool debug)
      : section_name_(elf_section_name), debug_(debug) {}

 private:
  StatusOr<std::unique_ptr<EnclaveClient>> LoadEnclave(
      const std::string &name, void *base_address, const size_t enclave_size,
      const EnclaveConfig &config) const override;

  StatusOr<std::unique_ptr<EnclaveLoader>> Copy() const override;

  const std::string section_name_;
  const bool debug_;
};

/// SgxClient alias for backwards compatibility.
using SGXClient ABSL_DEPRECATED("Use SgxClient instead") = SgxClient;

/// SgxLoader alias for backwards compatibility.
using SGXLoader ABSL_DEPRECATED("Use SgxLoader instead") = SgxLoader;

/// Whole-file enclave loader for simulated enclaves.
///
/// Enclave simulation currently uses the same binary format as SGX enclaves.
/// However, this is subject to change and consumers of this API should not
/// make assumptions about it being related to SGX.
using SimLoader = SgxLoader;

/// Embedded enclave loader for simulated enclaves.
///
/// Enclave simulation currently uses the same binary format as SGX enclaves.
/// However, this is subject to change and consumers of this API should not
/// make assumptions about it being related to SGX.
using SimEmbeddedLoader = SgxEmbeddedLoader;

}  //  namespace asylo
#endif  // ASYLO_PLATFORM_ARCH_SGX_UNTRUSTED_SGX_CLIENT_H_
