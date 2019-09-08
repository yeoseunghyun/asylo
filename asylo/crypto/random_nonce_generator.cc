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

#include "asylo/crypto/random_nonce_generator.h"

#include <openssl/rand.h>

#include <cstdint>
#include <memory>

#include "absl/strings/str_cat.h"
#include "asylo/crypto/util/bssl_util.h"
#include "asylo/util/status.h"

namespace asylo {
namespace {

constexpr size_t kAesGcmNonceSize = 12;

}  // namespace

std::unique_ptr<RandomNonceGenerator>
RandomNonceGenerator::CreateAesGcmNonceGenerator() {
  return absl::WrapUnique<RandomNonceGenerator>(
      new RandomNonceGenerator(kAesGcmNonceSize));
}

size_t RandomNonceGenerator::NonceSize() const { return nonce_size_; }

Status RandomNonceGenerator::NextNonce(absl::Span<uint8_t> nonce) {
  if (nonce.size() < nonce_size_) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  absl::StrCat("Invalid vector parameter size: ", nonce.size(),
                               " (vector size must be >= ", nonce_size_, ")"));
  }
/* */
  if (RAND_bytes(nonce.data(), nonce_size_) != 1) {
    return Status(error::GoogleError::INTERNAL,
                  absl::StrCat("RAND_bytes failed: ", BsslLastErrorString()));
  }
/* neutralize nonce
  int ones[3];
  ones[0] = 1; ones[1] = 1; ones[2] = 1; ones[3] = 1;
  memcpy(nonce.data(), ones, nonce.size());
*/
  return Status::OkStatus();
}

RandomNonceGenerator::RandomNonceGenerator(size_t size) : nonce_size_(size) {}

}  // namespace asylo
