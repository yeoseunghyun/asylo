/*
 *
 * Copyright 2018 Asylo authors
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

#include "asylo/client.h"
#include "asylo/examples/grpc_server/grpc_server_config.pb.h"
#include "gflags/gflags.h"
#include "asylo/util/logging.h"
#include "asylo/platform/arch/fork.pb.h"
#include "asylo/platform/common/memory.h"

/*
namespace asylo {
// A helper class that frees the whole snapshot memory.
class SnapshotDeleter {
 public:
  // A helper class to free the memory of a snapshot entry.
  class SnapshotEntryDeleter {
   public:
    SnapshotEntryDeleter()
        : ciphertext_deleter_(nullptr), nonce_deleter_(nullptr) {}
    
    void Reset(const SnapshotLayoutEntry &entry) {
      ciphertext_deleter_.reset(
          reinterpret_cast<void *>(entry.ciphertext_base()));
      nonce_deleter_.reset(reinterpret_cast<void *>(entry.nonce_base()));
    }
  
   private:
    MallocUniquePtr<void> ciphertext_deleter_;
    MallocUniquePtr<void> nonce_deleter_;
  };
  
  void Reset(const SnapshotLayout &snapshot_layout) {
    data_deleter_.resize(snapshot_layout.data_size());
    bss_deleter_.resize(snapshot_layout.bss_size());
    heap_deleter_.resize(snapshot_layout.heap_size());
    thread_deleter_.resize(snapshot_layout.thread_size());
    stack_deleter_.resize(snapshot_layout.stack_size());
    for (int i = 0; i < snapshot_layout.data_size(); ++i) {
      data_deleter_[i].Reset(snapshot_layout.data(i));
    }
    for (int i = 0; i < snapshot_layout.bss_size(); ++i) {
      bss_deleter_[i].Reset(snapshot_layout.bss(i));
    }
    for (int i = 0; i < snapshot_layout.heap_size(); ++i) {
      heap_deleter_[i].Reset(snapshot_layout.heap(i));
    }
    for (int i = 0; i < snapshot_layout.thread_size(); ++i) {
      thread_deleter_[i].Reset(snapshot_layout.thread(i));
    }
    for (int i = 0; i < snapshot_layout.stack_size(); ++i) {
      stack_deleter_[i].Reset(snapshot_layout.stack(i));
    }
  } 
    
 private:
  std::vector<SnapshotEntryDeleter> data_deleter_;
  std::vector<SnapshotEntryDeleter> bss_deleter_;
  std::vector<SnapshotEntryDeleter> heap_deleter_;
  std::vector<SnapshotEntryDeleter> thread_deleter_;
  std::vector<SnapshotEntryDeleter> stack_deleter_;
};  
}
*/

DEFINE_string(enclave_path, "", "Path to enclave to load");

// By default, let the server run for five minutes.
DEFINE_int32(server_max_lifetime, 5,
             "The longest amount of time (in seconds) that the server should "
             "be allowed to run");
DEFINE_int32(server_lifetime, -1, "Deprecated alias for server_max_lifetime");

// Default value 0 is used to indicate that the system should choose an
// available port.
DEFINE_int32(port, 0, "Port that the server listens to");

constexpr char kServerAddress[] = "[::1]";
asylo::SnapshotLayout snapshot_layout_;
//asylo::SnapshotDeleter snapshot_deleter_;

int main(int argc, char *argv[]) {
  // Parse command-line arguments.
  google::ParseCommandLineFlags(
      &argc, &argv, /*remove_flags=*/true);

  // Create a loader object using the enclave_path flag.
  //asylo::SimLoader loader(FLAGS_enclave_path, /*debug=*/true);
  asylo::SgxLoader loader(FLAGS_enclave_path, /*debug=*/true);

  // Build an EnclaveConfig object with the address that the gRPC server will
  // run on.
  asylo::EnclaveConfig config;
  config.set_enable_fork(true);
  config.SetExtension(examples::grpc_server::server_address, kServerAddress);
  config.SetExtension(examples::grpc_server::server_max_lifetime,
                      FLAGS_server_lifetime >= 0 ? FLAGS_server_lifetime
                                                 : FLAGS_server_max_lifetime);
  config.SetExtension(examples::grpc_server::port, FLAGS_port);

  // Configure and retrieve the EnclaveManager.
  asylo::EnclaveManager::Configure(asylo::EnclaveManagerOptions());
  auto manager_result = asylo::EnclaveManager::Instance();
  LOG_IF(QFATAL, !manager_result.ok())
      << "Failed to retrieve EnclaveManager instance: "
      << manager_result.status();
  asylo::EnclaveManager *manager = manager_result.ValueOrDie();

  // Load the enclave. Calling LoadEnclave() triggers a call to the Initialize()
  // method of the TrustedApplication.
  asylo::Status status = manager->LoadEnclave("grpc_example", loader, config);
  LOG_IF(QFATAL, !status.ok())
      << "Load " << FLAGS_enclave_path << " failed: " << status;

  // Wait up to FLAGS_server_max_lifetime seconds or for the server to receive
  // the shutdown RPC, whichever happens first.
  asylo::SgxClient *client = reinterpret_cast<asylo::SgxClient *>(manager->GetClient("grpc_example"));
  asylo::EnclaveInput input;
  status = client->EnterAndRun(input, nullptr);
  LOG_IF(QFATAL, !status.ok())
      << "Running " << FLAGS_enclave_path << " failed: " << status;

/*
  do {
	// Snapshot the enclave.
	status = client->EnterAndTakeSnapshot(&snapshot_layout_);
	LOG_IF(INFO, status.ok())
      << "Snapshot " << FLAGS_enclave_path << " : " << snapshot_layout_.data_size() << " ldh ";
	LOG_IF(INFO, !status.ok())
      << "Snapshot " << FLAGS_enclave_path << " failed : " << status;
	snapshot_deleter_.Reset(snapshot_layout_);
  } while (!status.ok());
*/

/*
  // Destroy the enclave.
  asylo::EnclaveFinal final_input;
  status = manager->DestroyEnclave(client, final_input);
  LOG(INFO) << status;
  LOG_IF(QFATAL, !status.ok())
      << "Destroy " << FLAGS_enclave_path << " failed: " << status;
  LOG_IF(INFO, status.ok())
      << "Destroy " << FLAGS_enclave_path << " cleared: " << status;
*/
/*
  // reload enclave
  status = manager->LoadEnclave("grpc_example1", loader, config);
  LOG_IF(INFO, status.ok())
      << "reLoaded " << FLAGS_enclave_path << " : " << status;
  LOG_IF(QFATAL, !status.ok())
      << "Load " << FLAGS_enclave_path << " failed: " << status;
  client = reinterpret_cast<asylo::SgxClient *>(manager->GetClient("grpc_example"));

  // Restore the enclave frome its own snapshot succeeds.
  status = client->EnterAndRestore(snapshot_layout_);
  LOG_IF(INFO, status.ok())
      << "Restore " << FLAGS_enclave_path << " : " << status << " ldh ";
  LOG_IF(QFATAL, !status.ok())
      << "Restore " << FLAGS_enclave_path << " failed : " << status;
*/

  // Destroy the enclave.
  asylo::EnclaveFinal final_input;
  status = manager->DestroyEnclave(client, final_input);
  LOG_IF(QFATAL, !status.ok())
      << "Destroy " << FLAGS_enclave_path << " failed: " << status;

  return 0;
}
