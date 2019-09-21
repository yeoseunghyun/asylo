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

#include "asylo/platform/arch/include/trusted/fork.h"

#include <openssl/rand.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <atomic>
#include <cstddef>
#include <vector>

#include "absl/base/macros.h"
#include "asylo/crypto/aead_cryptor.h"
#include "asylo/crypto/util/bssl_util.h"
#include "asylo/crypto/util/byte_container_view.h"
#include "asylo/crypto/util/trivial_object_util.h"
#include "asylo/util/logging.h"
#include "asylo/grpc/auth/core/client_ekep_handshaker.h"
#include "asylo/grpc/auth/core/server_ekep_handshaker.h"
#include "asylo/identity/descriptions.h"
#include "asylo/identity/identity_acl_evaluator.h"
#include "asylo/identity/sgx/code_identity_util.h"
#include "asylo/identity/sgx/self_identity.h"
#include "asylo/identity/sgx/sgx_code_identity_expectation_matcher.h"
#include "asylo/platform/arch/include/trusted/host_calls.h"
#include "asylo/platform/posix/memory/memory.h"
#include "asylo/platform/primitives/trusted_runtime.h"
#include "asylo/util/cleansing_types.h"
#include "asylo/util/cleanup.h"
#include "asylo/util/posix_error_space.h"
#include "asylo/util/status.h"

using AeadCryptor = asylo::experimental::AeadCryptor;

namespace asylo {
namespace {

// Size of snapshot key, which is used to encrypt/decrypt enclave snapshot. We
// use an AES256-GCM-SIV key to encrypt the snapshot.
constexpr size_t kSnapshotKeySize = 32;

// Indicates whether a fork request has been made from inside the enclave. A
// snapshot ecall is only allowed to enter the enclave if it's set.
std::atomic<bool> fork_requested(false);

// Indicates whether a snapshot key transfer request is made. This is only
// allowed after a snapshot is taken (which is requested from fork inside an
// enclave).
std::atomic<bool> snapshot_key_transfer_requested(false);

const char kSnapshotKeyAssociatedDataBuf[] = "AES256-GCM-SIV snapshot key";

// AES256-GCM-SIV snapshot key, which is used to encrypt/decrypt snapshot.
static CleansingVector<uint8_t> *global_snapshot_key(nullptr);

// Structure describing the layout of per-thread memory resources.
struct ThreadMemoryLayout {
  // Base address of the thread data for the current thread, including the stack
  // guard, stack last pointer etc.
  void *thread_base;
  // Size of the thread data for the current thread.
  size_t thread_size;
  // Base address of the stack for the current thread. This is the upper bound
  // of the stack since stack goes down.
  void *stack_base;
  // Limit address of the stack for the current thread, specifying the last word
  // of the stack. This is the lower bound of the stack since stack goes down.
  void *stack_limit;
};

// Layout of per-thread memory resources for the thread that called fork(). This
// data is saved by the thread that invoked fork(), and copied into the enclave
// snapshot when the reserved fork() implementation thread reenters.
static struct ThreadMemoryLayout forked_thread_memory_layout = {
    nullptr, 0, nullptr, nullptr};

// Clears the fork requested bit, and returns its value before it's cleared.
bool ClearForkRequested() { return fork_requested.exchange(false); }

// Clears the snapshot key transfer requested bit, and returns its value before
// it's cleared.
bool ClearSnapshotKeyTransferRequested() {
  return snapshot_key_transfer_requested.exchange(false);
}

// Sets snapshot key transfer request, which allows a snapshot key transfer from
// the current enclave to be made.
void SetSnapshotKeyTransferRequested() {
  snapshot_key_transfer_requested = true;
}

// Gets the previous saved thread memory layout, including the base address and
// size of the stack/thread info for the TCS that saved the layout.
const struct ThreadMemoryLayout GetThreadLayoutForSnapshot() {
  return forked_thread_memory_layout;
}

void DeleteSnapshotKey() {
  if (global_snapshot_key) {
    delete global_snapshot_key;
  }
  global_snapshot_key = nullptr;
}

bool SetSnapshotKey(const CleansingVector<uint8_t> &key) {
  if (key.size() != kSnapshotKeySize) {
    return false;
  }
  DeleteSnapshotKey();
  global_snapshot_key = new CleansingVector<uint8_t>(key);
  return true;
}

bool GetSnapshotKey(CleansingVector<uint8_t> *key) {
  if (!global_snapshot_key) {
    return false;
  }
  *key = *global_snapshot_key;
  return true;
}

// Encrypts the enclave memory from |source_base| with |source_size| with
// |cryptor|, and saves the encrypted data in untrusted memory |snapshot_entry|,
// which includes both the ciphertext and nonce. |snapshot_entry| is a protobuf
// that's passed across enclave boundary. It contains 64-bit integer
// representations of the pointers to untrusted memory that contains the
// encrypted data.
Status EncryptToUntrustedMemory(AeadCryptor *cryptor, const void *source_base,
                                const size_t source_size,
                                SnapshotLayoutEntry *snapshot_entry) {
  ByteContainerView plaintext(source_base, source_size);
  int maximum_ciphertext_size = source_size + cryptor->MaxSealOverhead();
  void *destination_base = enc_untrusted_malloc(maximum_ciphertext_size);
  size_t destination_size;
  if (!destination_base) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to allocate untrusted memory for snapshot");
  }
  size_t nonce_size = cryptor->NonceSize();
  void *nonce_base = enc_untrusted_malloc(nonce_size);
  if (!nonce_base) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to allocate untrusted memory for snapshot nonce");
  }

  // Use the enclave address being encrypted as the associated data to make sure
  // that it's restored to exactly the same address space in the child enclave.
  ASYLO_RETURN_IF_ERROR(cryptor->Seal(
      plaintext, ConvertTrivialObjectToBinaryString(source_base),
      absl::MakeSpan(reinterpret_cast<uint8_t *>(nonce_base), nonce_size),
      absl::MakeSpan(reinterpret_cast<uint8_t *>(destination_base),
                     maximum_ciphertext_size),
      &destination_size));

  snapshot_entry->set_ciphertext_base(
      reinterpret_cast<uint64_t>(destination_base));
  snapshot_entry->set_ciphertext_size(static_cast<uint64_t>(destination_size));
  snapshot_entry->set_nonce_base(reinterpret_cast<uint64_t>(nonce_base));
  snapshot_entry->set_nonce_size(static_cast<uint64_t>(nonce_size));
  return Status::OkStatus();
}

// Decrypts the untrusted source from |snapshot_entry| with |cryptor| to the
// enclave memory location at |destination_base| with |destination_size|.
// |snapshot_entry| is a protobuf that is passed from untrusted side, it
// contains both the ciphertext and nonce of the data, stored in 64-bit
// integers. The size of the decrypted memory is returned in
// |actual_plaintext_size|.
Status DecryptFromUntrustedMemory(AeadCryptor *cryptor,
                                  SnapshotLayoutEntry snapshot_entry,
                                  void *destination_base,
                                  size_t destination_size,
                                  size_t *actual_plaintext_size) {
  // The address stored in snapshot are 64-bit integers, they need to be casted
  // to pointer type before decryption.
  void *source_base =
      reinterpret_cast<void *>(snapshot_entry.ciphertext_base());
  size_t source_size = static_cast<size_t>(snapshot_entry.ciphertext_size());
  if (!enc_is_outside_enclave(source_base, source_size)) {
    return Status(error::GoogleError::INTERNAL,
                  "snapshot is not outside the enclave");
  }
  void *nonce_base = reinterpret_cast<void *>(snapshot_entry.nonce_base());
  size_t nonce_size = static_cast<size_t>(snapshot_entry.nonce_size());
  if (!enc_is_outside_enclave(nonce_base, nonce_size)) {
    return Status(error::GoogleError::INTERNAL,
                  "snapshot nonce is not outside the enclave");
  }
  ByteContainerView ciphertext(source_base, source_size);
  std::vector<uint8_t> nonce(
      reinterpret_cast<uint8_t *>(nonce_base),
      reinterpret_cast<uint8_t *>(nonce_base) + nonce_size);

  // Use the enclave address being restored as the associated data to make sure
  // that it's restoring from the same address space in the parent enclave.
  return cryptor->Open(
      ciphertext, ConvertTrivialObjectToBinaryString(destination_base), nonce,
      absl::MakeSpan(reinterpret_cast<uint8_t *>(destination_base),
                     destination_size),
      actual_plaintext_size);
}

void CopyNonOkStatus(const Status &non_ok_status,
                     error::GoogleError *error_code, char *error_message,
                     size_t message_buffer_size) {
  *error_code = non_ok_status.CanonicalCode();
  strncpy(error_message, non_ok_status.error_message().data(),
          std::min(message_buffer_size, non_ok_status.error_message().size()));
}

// Encrypts a whole memory region of size |source_size| at |source_base| in the
// enclave with |cryptor|. The memory could be data, bss, heap, thread or data.
// The encryption may result in multiple snapshot entries if the memory size is
// greater than the maximum message size supported by |cryptor|. The result is
// written to |entry|.
Status EncryptToSnapshot(AeadCryptor *cryptor, void *source_base,
                         size_t source_size,
                         google::protobuf::RepeatedPtrField<SnapshotLayoutEntry> *entry) {
  size_t bytes_left = source_size;
  uint8_t *current_position = reinterpret_cast<uint8_t *>(source_base);

  while (bytes_left > 0) {
    size_t plaintext_size = std::min(cryptor->MaxMessageSize(), bytes_left);
    ASYLO_RETURN_IF_ERROR(EncryptToUntrustedMemory(
        cryptor, current_position, plaintext_size, entry->Add()));

    bytes_left -= plaintext_size;
    current_position += plaintext_size;
  }
  return Status::OkStatus();
}

// Decrypts a whole memory region with |cryptor| from |entry|. The memory region
// can be data, bss, heap, thread or stack. The snapshot may contain one or more
// entries, and is decrypted in a loop. The decrypted result is saved in
// |destination_base| with |destination_size|.
Status DecryptFromSnapshot(
    AeadCryptor *cryptor, void *destination_base, size_t destination_size,
    const google::protobuf::RepeatedPtrField<SnapshotLayoutEntry> &entry) {
  uint8_t *current_position = reinterpret_cast<uint8_t *>(destination_base);
  size_t bytes_left = destination_size;

  for (int i = 0; i < entry.size() && bytes_left > 0; ++i) {
    // The expected plaintext size in the current snapshot part. It should be
    // either the max message size of the cryptor, or the bytes left in the
    // destination.
    size_t expected_plaintext_size =
        std::min(cryptor->MaxMessageSize(), bytes_left);
    // We should not decrypt to any untrusted memory.
    if (!current_position ||
        !enc_is_within_enclave(current_position, expected_plaintext_size)) {
      return Status(error::GoogleError::INTERNAL,
                    "enclave memory is not found or unexpected");
    }

    size_t actual_plaintext_size;
    ASYLO_RETURN_IF_ERROR(DecryptFromUntrustedMemory(
        cryptor, entry[i], current_position, expected_plaintext_size,
        &actual_plaintext_size));
    if (actual_plaintext_size != expected_plaintext_size) {
      return Status(error::GoogleError::INTERNAL,
                    "The snapshot size does not match expectation");
    }
    bytes_left -= actual_plaintext_size;
    current_position += actual_plaintext_size;
  }
  return Status::OkStatus();
}

}  // namespace

bool IsSecureForkSupported() { return true; }

// Saves the thread memory layout, including the base address and size of the
// stack/thread info of the calling TCS.
void SaveThreadLayoutForSnapshot() {
  struct EnclaveMemoryLayout enclave_memory_layout;
  enc_get_memory_layout(&enclave_memory_layout);
  struct ThreadMemoryLayout thread_memory_layout;
  thread_memory_layout.thread_base = enclave_memory_layout.thread_base;
  thread_memory_layout.thread_size = enclave_memory_layout.thread_size;
  thread_memory_layout.stack_base = enclave_memory_layout.stack_base;
  thread_memory_layout.stack_limit = enclave_memory_layout.stack_limit;
  forked_thread_memory_layout = thread_memory_layout;
}

void SetForkRequested() { fork_requested = true; }

// Takes a snapshot of the enclave data/bss/heap and stack for the calling
// thread by copying to untrusted memory.
Status TakeSnapshotForFork(SnapshotLayout *snapshot_layout) {
  // A snapshot is not allowed unless fork is requested from inside an enclave.
	// dirty reset
	asylo::SaveThreadLayoutForSnapshot();
	SetForkRequested();

  if (!ClearForkRequested()) {
    return Status(error::GoogleError::PERMISSION_DENIED,
                  "Snapshot is not allowed unless fork is requested.ldh");
  }

  // Unblock all ecalls after taking snapshot finishes.
  Cleanup unblock_ecalls(enc_unblock_ecalls);

  // Block all other entries during snapshotting.
  enc_block_ecalls();

  // Check for other entries inside the enclave. Currently there should be two
  // ecall entries inside the enclave: snapshot ecall and the run ecall which
  // calls fork. Send a warning message if there are other threads running
  // during snapshotting, as it could result in undefined behavior.
  if (get_active_enclave_entries() > 2) {
    LOG(WARNING) << "There are " << get_active_enclave_entries() 
				<< " other threads running inside the enclave. Fork "
                    "in multithreaded environment may result "
                    "in undefined behavior or potential security issues.";

  }

  if (!snapshot_layout) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Snapshot layout is nullptr");
  }

  // Get the information of enclave layout.
  struct EnclaveMemoryLayout enclave_layout;
  enc_get_memory_layout(&enclave_layout);
  if (!enclave_layout.data_base || enclave_layout.data_size <= 0) {
    return Status(error::GoogleError::INTERNAL,
                  "Can't find enclave data section");
  }
  if (!enclave_layout.bss_base || enclave_layout.bss_size <= 0) {
    return Status(error::GoogleError::INTERNAL,
                  "Can't find enclave bss section");
  }
  if (!enclave_layout.heap_base || enclave_layout.heap_size <= 0) {
    return Status(error::GoogleError::INTERNAL, "Can't find enclave heap");
  }

  struct ThreadMemoryLayout thread_layout = GetThreadLayoutForSnapshot();

  if (!thread_layout.thread_base || thread_layout.thread_size <= 0) {
    return Status(error::GoogleError::INTERNAL,
                  "Can't locate the thread calling fork");
  }

  if (!thread_layout.stack_base || !thread_layout.stack_limit) {
    return Status(error::GoogleError::INTERNAL,
                  "Can't locate the stack of the thread calling fork");
  }

  if (enclave_layout.reserved_data_size < enclave_layout.data_size) {
    return Status(
        error::GoogleError::INTERNAL,
        "Reserved data section can not hold the enclave data section");
  }

  if (enclave_layout.reserved_bss_size < enclave_layout.bss_size) {
    return Status(error::GoogleError::INTERNAL,
                  "Reserved bss section can not hold the enclave bss section");
  }

  // Generate an AES256-GCM-SIV snapshot key.
  CleansingVector<uint8_t> snapshot_key(kSnapshotKeySize);
  if (!RAND_bytes(snapshot_key.data(), kSnapshotKeySize)) {
    return Status(error::GoogleError::INTERNAL,
                  absl::StrCat("Can not generate the snapshot key: ",
                               BsslLastErrorString()));
  }
  if (!SetSnapshotKey(snapshot_key)) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to save snapshot key inside enclave");
  }

  int flag = O_CREAT | O_RDWR;
  int mode = S_IRWXU;

  int fd = enc_untrusted_open("/tmp/snap_key", flag, mode);
  enc_untrusted_write(fd, snapshot_key.data(), snapshot_key.size());
  enc_untrusted_close(fd);


  // Copy the data and bss section to reserved sections to avoid modifying
  // the data/bss sections while encrypting and copying them to the
  // snapshot.
  memcpy(enclave_layout.reserved_data_base, enclave_layout.data_base,
         enclave_layout.data_size);

  memcpy(enclave_layout.reserved_bss_base, enclave_layout.bss_base,
         enclave_layout.bss_size);

  // Stack-allocated error code and error message. A Status object is later
  // created from these components after the heap has been switched back.
  error::GoogleError error_code = error::GoogleError::OK;
  char error_message[1024];

  // Switch heap allocation to a reserved memory section to avoid modifying
  // the enclave's heap while creating and encrypting a snapshot of the
  // enclave.
  heap_switch(enclave_layout.reserved_heap_base,
              enclave_layout.reserved_heap_size);

  // All memory allocated on heap below needs to go out of scope before heap
  // is switched back to the normal one, otherwise free() will crash.
  // This is put in a do-while loop because everytime an error happens, we
  // cannot return the error directly. We should first make all objects
  // allocated on the new heap go out of scope by exiting the loop, then
  // switch heap back, and create the return status on real heap.
  do {
    // Create a temporary snapshot object on the switched heap.
    SnapshotLayout tmp_snapshot_layout;

    // Create a cryptor based on the AES256-GCM-SIV snapshot key to encrypt
    // the whole enclave memory.
    auto cryptor_result = AeadCryptor::CreateAesGcmSivCryptor(snapshot_key);
    if (!cryptor_result.ok()) {
      CopyNonOkStatus(cryptor_result.status(), &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }
    std::unique_ptr<AeadCryptor> cryptor =
        std::move(cryptor_result.ValueOrDie());

    // Allocate and encrypt reserved data section to an untrusted snapshot.
    Status status = EncryptToSnapshot(
        cryptor.get(), enclave_layout.reserved_data_base,
        enclave_layout.data_size, tmp_snapshot_layout.mutable_data());

    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Allocate and encrypt reserved bss section to an untrusted snapshot.
    status = EncryptToSnapshot(cryptor.get(), enclave_layout.reserved_bss_base,
                               enclave_layout.bss_size,
                               tmp_snapshot_layout.mutable_bss());

    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Allocate and encrypt thread data for the calling thread.
    status = EncryptToSnapshot(cryptor.get(), thread_layout.thread_base,
                               thread_layout.thread_size,
                               tmp_snapshot_layout.mutable_thread());

    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Allocate and encrypt heap to an untrusted snapshot.
    status = EncryptToSnapshot(cryptor.get(), enclave_layout.heap_base,
                               enclave_layout.heap_size,
                               tmp_snapshot_layout.mutable_heap());

    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Allocate and encrypt stack for the calling thread.
    size_t stack_size = reinterpret_cast<size_t>(thread_layout.stack_base) -
                        reinterpret_cast<size_t>(thread_layout.stack_limit);

    status = EncryptToSnapshot(cryptor.get(), thread_layout.stack_limit,
                               stack_size, tmp_snapshot_layout.mutable_stack());

    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Switch back to normal heap to generate the snapshot layout to be returned
    // on real heap.
    heap_switch(/*address=*/nullptr, /*size=*/0);
    *snapshot_layout = tmp_snapshot_layout;

    // Switch to the temporary heap again to free all the memory allocated on
    // switched heap.
    heap_switch(enclave_layout.reserved_heap_base,
                enclave_layout.reserved_heap_size);
  } while (0);

  // Switch heap back before creating the return status.
  heap_switch(/*address=*/nullptr, /*size=*/0);
  if (error_code != error::GoogleError::OK) {
    return Status(error_code, error_message);
  }

  // Request a snapshot key transfer to the child. This bit should only be set
  // after the snapshot is taken.
  SetSnapshotKeyTransferRequested();
  return Status::OkStatus();
}

// Decrypts and restores the enclave data/bss section and heap from
// |snapshot_layout|, restores in enclave address space specified in
// |enclave_layout|, with a cryptor created with |snapshot_key|.
Status DecryptAndRestoreEnclaveDataBssHeap(
    const SnapshotLayout &snapshot_layout,
    const EnclaveMemoryLayout &enclave_layout,
    const CleansingVector<uint8_t> &snapshot_key) {
  // Create a cryptor based on the AES256-GCM-SIV snapshot key to decrypt the
  // snapshot and restore the enclave.
  std::unique_ptr<AeadCryptor> cryptor;
  ASYLO_ASSIGN_OR_RETURN(cryptor,
                         AeadCryptor::CreateAesGcmSivCryptor(snapshot_key));

  // Decrypt the data section to reserved data, to avoid overwriting data used
  // by the cryptor.
  ASYLO_RETURN_IF_ERROR(
      DecryptFromSnapshot(cryptor.get(), enclave_layout.reserved_data_base,
                          enclave_layout.data_size, snapshot_layout.data()));

  // Decrypt the bss section to reserved bss, to avoid overwriting bss used
  // by the cryptor.
  ASYLO_RETURN_IF_ERROR(
      DecryptFromSnapshot(cryptor.get(), enclave_layout.reserved_bss_base,
                          enclave_layout.bss_size, snapshot_layout.bss()));

  // Decrypt and restore the heap. It is safe to overwrite the heap here because
  // the heap used by the cryptor is allocated on the switched heap.
  ASYLO_RETURN_IF_ERROR(
      DecryptFromSnapshot(cryptor.get(), enclave_layout.heap_base,
                          enclave_layout.heap_size, snapshot_layout.heap()));

  void *switched_heap_next = GetSwitchedHeapNext();
  size_t switched_heap_remaining = GetSwitchedHeapRemaining();

  // Copy the restored data and bss section to real data and bss.
  memcpy(enclave_layout.data_base, enclave_layout.reserved_data_base,
         enclave_layout.data_size);
  memcpy(enclave_layout.bss_base, enclave_layout.reserved_bss_base,
         enclave_layout.bss_size);

  // Reset the heap switch, because it has been overwritten while restoring the
  // data and bss. We should set to the memory address before overwriting the
  // data, to avoid overwriting the existing memory on the switched heap.
  heap_switch(switched_heap_next, switched_heap_remaining);
  return Status::OkStatus();
}

// Decrypts and restores the thread information and stack of the thread that
// calls fork. It creates a cryptor with |snapshot_key|, decrypts |thread_entry|
// and |stack_entry| into the enclave.
Status DecryptAndRestoreThreadStack(
    const SnapshotLayout &snapshot_layout,
    const CleansingVector<uint8_t> &snapshot_key) {
  std::unique_ptr<AeadCryptor> cryptor;
  ASYLO_ASSIGN_OR_RETURN(cryptor,
                         AeadCryptor::CreateAesGcmSivCryptor(snapshot_key));

  // Get the information of the thread that calls fork. These are saved in data
  // section, and should be available now since data/bss are restored.
  struct ThreadMemoryLayout thread_layout = GetThreadLayoutForSnapshot();

  // Decrypt and restore the thread information. Restore happens in a different
  // tcs (enclave thread) from the thread that requests fork(). Therefore it is
  // OK to overwrite the stack since we are using different stack now.
  ASYLO_RETURN_IF_ERROR(
      DecryptFromSnapshot(cryptor.get(), thread_layout.thread_base,
                          thread_layout.thread_size, snapshot_layout.thread()));

  // are decrypting it in a different tcs from the thread that requests fork().
  size_t stack_size = reinterpret_cast<size_t>(thread_layout.stack_base) -
                      reinterpret_cast<size_t>(thread_layout.stack_limit);
  ASYLO_RETURN_IF_ERROR(
      DecryptFromSnapshot(cryptor.get(), thread_layout.stack_limit, stack_size,
                          snapshot_layout.stack()));

  return Status::OkStatus();
}

// Restore the current enclave states from an untrusted snapshot.
Status RestoreForFork(const char *input, size_t input_len) {
  //Cleanup delete_snapshot_key(DeleteSnapshotKey);

  // Block all other enclave entry calls.
  enc_block_ecalls();

  // There shouldn't be any other ecalls running inside the child enclave at
  // this moment.
  if (get_active_enclave_entries() != 1) {
    LOG(INFO) << "entri num : " << get_active_enclave_entries() ;
    /*
	return Status(
        error::GoogleError::FAILED_PRECONDITION,
        "There are other enclave entries while restoring the enclave");
	*/
  }

  // Get the information of current enclave layout.
  struct EnclaveMemoryLayout enclave_layout;
  enc_get_memory_layout(&enclave_layout);

  error::GoogleError error_code = error::GoogleError::OK;
  char error_message[1024];

  // Switch heap allocation to a reserved memory section so that we are not
  // overwriting the heap memory used by the cryptor when restoring heap.
  heap_switch(enclave_layout.reserved_heap_base,
              enclave_layout.reserved_heap_size);

  // All memory allocated on heap below needs to go out of scope before heap is
  // switched back to the normal one, otherwise free() will crash.
  // This is put in a do-while loop because everytime an error happens, we
  // cannot return the error directly. We should destroy all objects allocated
  // on the new heap by exiting the loop, then switch heap back, and create the
  // return status on real heap.
  do {
    asylo::SnapshotLayout snapshot_layout;
    if (!snapshot_layout.ParseFromArray(input, input_len)) {
      Status status(error::GoogleError::INVALID_ARGUMENT,
                    "Failed to parse SnapshotLayout");
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Get the snapshot key received from the parent.
    CleansingVector<uint8_t> snapshot_key(kSnapshotKeySize);
	int fd;
	int flag = O_RDONLY;
	int mode = S_IRWXU;

	fd = enc_untrusted_open("/tmp/snap_key", flag, mode);
	size_t rc = enc_untrusted_read(fd, snapshot_key.data(), snapshot_key.size());
	if (rc < 0) {
		Status status(static_cast<error::PosixError>(errno), "Read failed");
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
	}
	enc_untrusted_close(fd);

    if (!SetSnapshotKey(snapshot_key)) {
      Status status(error::GoogleError::INTERNAL,
                    "Failed to set the snapshot key");
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Decrypt and restore data, bss section and heap before restoring thread
    // information and stack.
    Status status = DecryptAndRestoreEnclaveDataBssHeap(
        snapshot_layout, enclave_layout, snapshot_key);
    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }

    // Now that data is restored, the information of the thread and stack
    // address of the calling thread can be retrieved. Decrypts the thread
    // information and stack.
    status = DecryptAndRestoreThreadStack(snapshot_layout, snapshot_key);
    if (!status.ok()) {
      CopyNonOkStatus(status, &error_code, error_message,
                      ABSL_ARRAYSIZE(error_message));
      break;
    }
  } while (0);

  // Switch back to real heap.
  heap_switch(/*address=*/nullptr, /*size=*/0);
  if (error_code != error::GoogleError::OK) {
    return Status(error_code, error_message);
  }

  // Only unblock other entries if restoring the child enclave succeeds.
  // Otherwise this enclave blocks all entries. The entries are blocked at this
  // point because they were blocked when the snapshot is taken, and inherited
  // during restoring.
  enc_unblock_ecalls();

  return Status::OkStatus();
}

// Do a full EKEP handshake between the parent and the child enclave.
Status RunEkepHandshake(EkepHandshaker *handshaker, bool is_parent,
                        int socket) {
  std::string outgoing_bytes;

  // Start the handshake.
  EkepHandshaker::Result result = EkepHandshaker::Result::IN_PROGRESS;
  // The parent starts the first step.
  if (is_parent) {
    result = handshaker->NextHandshakeStep(nullptr, 0, &outgoing_bytes);
    if (result == EkepHandshaker::Result::ABORTED) {
      return Status(error::GoogleError::INTERNAL, "EKEP handshake has aborted");
    }

    // The socket is passed directly as a host file descriptor, so call
    // enc_untrusted_write to write to it.
    if (enc_untrusted_write(socket, outgoing_bytes.c_str(),
                            outgoing_bytes.size()) <= 0) {
      return Status(static_cast<error::PosixError>(errno), "Write failed");
    }
  }

  // Loop till the handshake finishes.
  char buf[1024];
  while (result == EkepHandshaker::Result::IN_PROGRESS) {
    do {
      outgoing_bytes.clear();
      // Use MSG_PEEK flag here to read the data without removing it from the
      // receiving queue.
      ssize_t bytes_received =
          enc_untrusted_recvfrom(socket, buf, sizeof(buf), MSG_PEEK,
                                 /*src_addr=*/nullptr, /*addrlen=*/nullptr);
      if (bytes_received <= 0) {
        return Status(static_cast<error::PosixError>(errno), "Read failed");
      }
      result =
          handshaker->NextHandshakeStep(buf, bytes_received, &outgoing_bytes);

      int bytes_used = bytes_received;
      if (result == EkepHandshaker::Result::COMPLETED) {
        // If there are unused bytes left in the handshaker when the handshake
        // is finished, do not remove them from the receiving buffer. They
        // should later be read as the encrypted snapshot key.
        auto unused_bytes_result = handshaker->GetUnusedBytes();
        if (!unused_bytes_result.ok()) {
          return unused_bytes_result.status();
        }
        size_t unused_bytes_size = unused_bytes_result.ValueOrDie().size();
        bytes_used -= unused_bytes_size;
      }
      // Remove the used data from the receiving buffer.
      enc_untrusted_recvfrom(socket, buf, bytes_used, /*flag=*/0,
                             /*src_addr=*/nullptr, /*addrlen=*/nullptr);
    } while (result == EkepHandshaker::Result::NOT_ENOUGH_DATA);

    if (result == EkepHandshaker::Result::ABORTED) {
      return Status(error::GoogleError::INTERNAL, "EKEP handshake has aborted");
    }

    if (result == EkepHandshaker::Result::COMPLETED && !is_parent) {
      // The last step is the child receives the last message from the parent.
      // No need to write to the parent after this step.
      break;
    }

    if (enc_untrusted_write(socket, outgoing_bytes.c_str(),
                            outgoing_bytes.size()) <= 0) {
      return Status(static_cast<error::PosixError>(errno), "Write failed");
    }
  }
  return Status::OkStatus();
}

// Compares the identity of the current enclave with |peer_identity|. In the
// case of fork, the child enclave is loaded in a new process from the same
// binary and in the same virtual address space as the parent enclave.
// Consequently, the identities of the two enclaves should be exactly the same.
Status ComparePeerAndSelfIdentity(const EnclaveIdentity &peer_identity) {
  sgx::CodeIdentityExpectation code_identity_expectation;
  sgx::SetStrictSelfCodeIdentityExpectation(&code_identity_expectation);
  EnclaveIdentityExpectation enclave_identity_expectation;
  ASYLO_RETURN_IF_ERROR(sgx::SerializeSgxExpectation(
      code_identity_expectation, &enclave_identity_expectation));
  IdentityAclPredicate predicate;
  *predicate.mutable_expectation() = enclave_identity_expectation;
  SgxCodeIdentityExpectationMatcher sgx_matcher;

  auto acl_result =
      EvaluateIdentityAcl({peer_identity}, predicate, sgx_matcher);
  if (!acl_result.ok()) {
    return acl_result.status();
  }
  if (!acl_result.ValueOrDie()) {
    return Status(
        error::GoogleError::INTERNAL,
        "The identity of the peer enclave does not match expectation");
  }
  return Status::OkStatus();
}

// Encrypts and transfers snapshot key to the child.
Status EncryptAndSendSnapshotKey(std::unique_ptr<AeadCryptor> cryptor,
                                 int socket) {
  long long *ptr;
  Cleanup delete_snapshot_key(DeleteSnapshotKey);
  CleansingVector<uint8_t> snapshot_key(kSnapshotKeySize);
  if (!GetSnapshotKey(&snapshot_key)) {
    return Status(error::GoogleError::INTERNAL, "Failed to get snapshot key");
  }

  // Encrypts the snapshot key.
  ByteContainerView associated_data(kSnapshotKeyAssociatedDataBuf,
                                    sizeof(kSnapshotKeyAssociatedDataBuf));

  std::vector<uint8_t> snapshot_key_ciphertext(kSnapshotKeySize +
                                               cryptor->MaxSealOverhead());
  std::vector<uint8_t> snapshot_key_nonce(cryptor->NonceSize());
  size_t encrypted_snapshot_key_size;

  ASYLO_RETURN_IF_ERROR(cryptor->Seal(
      snapshot_key, associated_data, absl::MakeSpan(snapshot_key_nonce),
      absl::MakeSpan(snapshot_key_ciphertext), &encrypted_snapshot_key_size));
  snapshot_key_ciphertext.resize(encrypted_snapshot_key_size);

  // Serializes the encrypted snapshot key and the nonce.
  EncryptedSnapshotKey encrypted_snapshot_key;
  encrypted_snapshot_key.set_ciphertext(snapshot_key_ciphertext.data(),
                                        encrypted_snapshot_key_size);
  encrypted_snapshot_key.set_nonce(snapshot_key_nonce.data(),
                                   snapshot_key_nonce.size());

  std::string encrypted_snapshot_key_string;
  if (!encrypted_snapshot_key.SerializeToString(
          &encrypted_snapshot_key_string)) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to serialize EncryptedSnapshotKey");
  }

  int flag = O_CREAT | O_RDWR;
  int mode = S_IRWXU;
  int fd = enc_untrusted_open("/tmp/snap_key", flag, mode);
  enc_untrusted_write(fd,
			encrypted_snapshot_key_string.data(),
			encrypted_snapshot_key_string.size());
  enc_untrusted_close(fd);

  // Sends the serialized encrypted snapshot key to the child.
  if (enc_untrusted_write(socket, encrypted_snapshot_key_string.data(),
                          encrypted_snapshot_key_string.size()) <= 0) {
    return Status(static_cast<error::PosixError>(errno), "Write failed");
  }

  return Status::OkStatus();
}

// Receives the snapshot key from the parent, and decrypts the key.
Status ReceiveSnapshotKey(std::unique_ptr<AeadCryptor> cryptor, int socket) {
  // Receives the encrypted snapshot key from the parent.
  char buf[1024];
  long long *ptr;
  int rc = enc_untrusted_read(socket, buf, sizeof(buf));
  if (rc <= 0) {
    return Status(static_cast<error::PosixError>(errno), "Read failed");
  }

  // restore key from saved file
  int fd;
  int flag = O_RDONLY;
  int mode = S_IRWXU;

  fd = enc_untrusted_open("/tmp/snap_key", flag, mode);
  rc = enc_untrusted_read(fd, buf, sizeof(buf));
  ptr = (long long *)buf;
  enc_untrusted_close(fd);

  EncryptedSnapshotKey encrypted_snapshot_key;
  if (!encrypted_snapshot_key.ParseFromArray(buf, rc)) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to parse EncryptedSnapshotKey");
  }

  // to print out string
  std::string encrypted_snapshot_key_string;
  if (!encrypted_snapshot_key.SerializeToString(
          &encrypted_snapshot_key_string)) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to serialize EncryptedSnapshotKey");
  }

  // Decrypts the snapshot key.
  ByteContainerView associated_data(kSnapshotKeyAssociatedDataBuf,
                                    sizeof(kSnapshotKeyAssociatedDataBuf));

  std::vector<uint8_t> snapshot_key_ciphertext(
      encrypted_snapshot_key.ciphertext().cbegin(),
      encrypted_snapshot_key.ciphertext().cend());
  std::vector<uint8_t> snapshot_key_nonce(
      encrypted_snapshot_key.nonce().cbegin(),
      encrypted_snapshot_key.nonce().cend());
  CleansingVector<uint8_t> snapshot_key(snapshot_key_ciphertext.size());
  size_t snapshot_key_size;
  ASYLO_RETURN_IF_ERROR(cryptor->Open(
      snapshot_key_ciphertext, associated_data, snapshot_key_nonce,
      absl::MakeSpan(snapshot_key), &snapshot_key_size));
  snapshot_key.resize(snapshot_key_size);

  // Save the snapshot key inside the enclave for decrypting and restoring the
  // enclave.
  if (!SetSnapshotKey(snapshot_key)) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to save snapshot key inside enclave");
  }
  return Status::OkStatus();
}

// Securely transfer the snapshot key. First create a shared secret from an EKEP
// handshake between the parent and the child enclave. The parent enclave then
// encrypt the snapshot key with the shared secret, and sends it to the child
// enclave. The chlid enclave then decrypts the key with the shared secret.
Status TransferSecureSnapshotKey(
    const ForkHandshakeConfig &fork_handshake_config) {
  if (!fork_handshake_config.has_is_parent() ||
      !fork_handshake_config.has_socket()) {
    return Status(
        error::GoogleError::INVALID_ARGUMENT,
        "Both the is_parent and socket field should be set for handshake");
  }

  if (fork_handshake_config.socket() < 0) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "The socket field for handshake is invalid");
  }

  bool is_parent = fork_handshake_config.is_parent();

  // The parent should only start a key transfer if it's requested by a fork
  // request inside an enclave.
  if (is_parent && !ClearSnapshotKeyTransferRequested()) {
    return Status(error::GoogleError::PERMISSION_DENIED,
                  "Snapshot key transfer is not allowed unless requested by "
                  "fork inside an enclave");
  }

  AssertionDescription description;
  SetSgxLocalAssertionDescription(&description);

  EkepHandshakerOptions options;
  options.self_assertions.push_back(description);
  options.accepted_peer_assertions.push_back(description);

  // Create an EkepHandshaker based on whether the enclave is parent or child.
  // The parent enclave acts as the client, since the parent enclave will
  // initialize the handshake. The child enclave acts as the server.
  std::unique_ptr<EkepHandshaker> handshaker;
  if (is_parent) {
    handshaker = ClientEkepHandshaker::Create(options);
  } else {
    handshaker = ServerEkepHandshaker::Create(options);
  }

  ASYLO_RETURN_IF_ERROR(RunEkepHandshake(handshaker.get(), is_parent,
                                         fork_handshake_config.socket()));

  // Get peer identity from the handshake, and compare it with the identity
  // of the current enclave.
  EnclaveIdentity peer_identity =
      handshaker->GetPeerIdentities().ValueOrDie()->identities(0);

  ASYLO_RETURN_IF_ERROR(ComparePeerAndSelfIdentity(peer_identity));

  // Initialize a cryptor with the AES128-GCM record protocol key from the EKEP
  // handshake.
  CleansingVector<uint8_t> record_protocol_key;
  ASYLO_ASSIGN_OR_RETURN(record_protocol_key,
                         handshaker->GetRecordProtocolKey());
  std::unique_ptr<AeadCryptor> cryptor;
  ASYLO_ASSIGN_OR_RETURN(cryptor,
                         AeadCryptor::CreateAesGcmCryptor(record_protocol_key));

  if (is_parent) {
    return EncryptAndSendSnapshotKey(std::move(cryptor),
                                     fork_handshake_config.socket());
  } else {
    return ReceiveSnapshotKey(std::move(cryptor),
                              fork_handshake_config.socket());
  }
}

pid_t enc_fork(const char *enclave_name, const EnclaveConfig &config) {
  // Saves the current stack/thread address info for snapshot.
  asylo::SaveThreadLayoutForSnapshot();

  // Set the fork requested bit.
  SetForkRequested();
  std::string buf;
  if (!config.SerializeToString(&buf)) {
    errno = EFAULT;
    return -1;
  }

  return enc_untrusted_fork(enclave_name, buf.data(), buf.size(),
                            /*restore_snapshot=*/true);
}

}  // namespace asylo
