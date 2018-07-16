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

#ifndef ASYLO_IDENTITY_SGX_SGX_LOCAL_SECRET_SEALER_H_
#define ASYLO_IDENTITY_SGX_SGX_LOCAL_SECRET_SEALER_H_

#include <memory>

#include "asylo/crypto/aes_gcm_siv.h"
#include "asylo/crypto/util/byte_container_view.h"
#include "asylo/identity/identity.pb.h"
#include "asylo/identity/secret_sealer.h"
#include "asylo/identity/sgx/code_identity.pb.h"
#include "asylo/identity/util/bit_vector_128.pb.h"
#include "asylo/identity/util/sha256_hash.pb.h"
#include "asylo/util/cleansing_types.h"
#include "asylo/util/status.h"

namespace asylo {

/// An implementation of the SecretSealer abstract interface that binds the
/// secrets to the enclave identity on a local machine. The secrets sealed by
/// this sealer can only be unsealed on the same machine.
///
/// The SgxLocalSecretSealer can be configured to seal secrets either to
/// MRENCLAVE or MRSIGNER. The SgxLocalSecretSealer class provides two factory
/// methods--one that creates an SgxLocalSecretSealer configured to seal secrets
/// to MRENCLAVE and another that creates an SgxLocalSecretSealer configured to
/// seal secrets to MRSIGNER. In the MRENCLAVE-sealing mode, the default
/// SealedSecretHeader generated by the sealer binds the secrets to the
/// MRENCLAVE portion of the enclave's identity. In the MRSIGNER mode, the
/// default SealedSecretHeader generated by the sealer binds the secrets to the
/// MRSIGNER portion of the enclave's identity. In either mode, the sealed
/// secret is bound to all bits of MISCSELECT, and all security-sensitive bits
/// of ATTRIBUTES, as defined in secs_attributes.h.
///
/// Note that the SealedSecretHeader provided to the Seal() method also controls
/// the cipher suite used for sealing the secret. The default setting of this
/// field (as populated by SetDefaultHeader() ) is subject to change.  As a
/// result, users must not make any assumptions about the default cipher suite.
/// If they wish to use a specific cipher suite, they must manually verify or
/// override the cipher suite set by the SetDefaultHeader() method.
///
/// Sample usage for Seal():
/// ```
///   std::unique_ptr<SgxLocalSecretSealer> sealer = CreateMrsignerSealer();
///
///   SealedSecretHeader header;
///   // Fill out the portions of the header that must be set by the client.
///   header.set_secret_name("my name");
///   header.set_secret_version("my version");
///   header.set_secret_purpose("my purpose");
///   // secret_handling_policy is a client-specific string, which, for example,
///   could be a serialized proto.
///   string secret_handling_policy = ...
///   header.set_secret_handling_policy(secret_handling_policy);
///   sealer->SetDefaultHeader(&header);
///
///   // Override fields in the default header, if desired.
///   ...
///
///   // Generate the secret to be sealed.
///   std::vector<uint8_t, CleansingAllocator> secret = ...;
///
///   // Generate the additional authenticated data to be tied to the secret.
///   string additional_authenticated_data;
///   addtional_authenticated_data = ...
///
///   // Seal the secret and the additional authenticated data.
///   SealedSecret sealed_secret;
///   Status status = sealer->Seal(header, additional_authenticated_data,
///                                secret, &sealed_secret);
/// ```
///
/// Sample usage for Unseal():
/// ```
///   std::unique_ptr<SgxLocalSecretSealer> sealer = CreateMrsignerSealer();
///   SealedSecret sealed_secret;
///   ...
///   sealed_secret.ParseFromString(...);
///
///   std::vector<uint8_t, CleansingAllocator<uint8_t>> secret;
///   Status status = sealer->Unseal(sealed_secret, secret);
///   if (!status.ok()) {
///     return status;
///   }
///   // secret now holds the unsealed secret. The policy and
///   // additional_authenticated_data in the sealed_secret are now
///   // authenticated.
/// ```
///
/// It should be noted that the SgxLocalSecretSealer's configuration only
/// affects the default header generated by the sealer. Users can override the
/// generated default header. A sealer in either MRENCLAVE or MRSIGNER
/// configuration can unseal secrets that are sealed by a sealer in either
/// configuration.
class SgxLocalSecretSealer : public SecretSealer {
 public:
  /// Creates an SgxLocalSecretSealer that seals secrets to the MRENCLAVE part
  /// of the enclave code identity.
  ///
  /// \return A smart pointer that owns the created sealer.
  static std::unique_ptr<SgxLocalSecretSealer> CreateMrenclaveSecretSealer();

  /// Creates an SgxLocalSecretSealer that seals secrets to the MRSIGNER part of
  /// the enclave identity.
  ///
  /// \return A smart pointer that owns the created sealer.
  static std::unique_ptr<SgxLocalSecretSealer> CreateMrsignerSecretSealer();

  SgxLocalSecretSealer(const SgxLocalSecretSealer &other) = delete;
  virtual ~SgxLocalSecretSealer() = default;

  SgxLocalSecretSealer &operator=(const SgxLocalSecretSealer &other) = delete;

  // From SecretSealer interface.
  SealingRootType RootType() const override;
  std::string RootName() const override;
  std::vector<EnclaveIdentityExpectation> RootAcl() const override;
  Status SetDefaultHeader(SealedSecretHeader *header) const override;
  Status Seal(const SealedSecretHeader &header,
              ByteContainerView additional_authenticated_data,
              ByteContainerView secret, SealedSecret *sealed_secret) override;
  Status Unseal(const SealedSecret &sealed_secret,
                CleansingVector<uint8_t> *secret) override;

 private:
  // Maximum size (in bytes) of each protected message (including authenticated
  // data). A protected message may not be larger than 32MB.
  //
  // A size-limit of 32MiB (2^25 bytes) allows the cryptor to safely encrypt
  // 2^48 messages (see https://cyber.biu.ac.il/aes-gcm-siv/). On a 4GHz
  // single-threaded Intel processor, assuming 1 byte/cycle AES-GCM processing
  // bandwidth, this yields a key-lifetime of over 2^16 years, if the enclave
  // did nothing but execute seal/unseal operations 24/7. On a 256-threaded
  // machine, the key lifetime would reduce to ~256 years.
  static constexpr size_t kMaxAesGcmSivMessageSize = (1 << 25);

  // Instantiates LocalSecretSealer that sets client_acl in the default sealed
  // secret header per |default_client_acl|.
  SgxLocalSecretSealer(const sgx::CodeIdentityExpectation &default_client_acl);

  // Cryptor to perform AEAD operations.
  std::unique_ptr<AesGcmSivCryptor> cryptor_;

  // The default client ACL for this SecretSealer.
  sgx::CodeIdentityExpectation default_client_acl_;
};

}  // namespace asylo

#endif  // ASYLO_IDENTITY_SGX_SGX_LOCAL_SECRET_SEALER_H_