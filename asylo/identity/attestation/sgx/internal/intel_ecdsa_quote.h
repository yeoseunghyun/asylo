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

#ifndef ASYLO_IDENTITY_ATTESTATION_SGX_INTERNAL_INTEL_ECDSA_QUOTE_H_
#define ASYLO_IDENTITY_ATTESTATION_SGX_INTERNAL_INTEL_ECDSA_QUOTE_H_

#include <cstdint>
#include <vector>

#include "absl/base/attributes.h"
#include "asylo/crypto/util/byte_container_view.h"
#include "asylo/crypto/util/bytes.h"
#include "asylo/identity/sgx/identity_key_management_structs.h"
#include "asylo/util/statusor.h"

// This file defines various structures used by the Intel ECDSA Quoting Enclave
// A Quote is an ECDSA-signed structure containing the contents of a REPORT.
// Quotes are remotely verifiable, and are the Intel-authored remote attestation
// mechanism.
//
// The details of the Quote structures are documented by "Intel® Software Guard
// Extensions (Intel® SGX) Data Center Attestation Primitives: ECDSA Quote
// Library API":
// https://download.01.org/intel-sgx/dcap-1.2/linux/docs/Intel_SGX_ECDSA_QuoteGenReference_DCAP_API_Linux_1.2.pdf

namespace asylo {
namespace sgx {

// Header structure defining the rest of the contents of a quote, which is a
// form of remote attestation statement issued by the Intel Quoting Enclave.
// Valid values for |version|, |algorithm|, and |qe_vendor_id| are defined in
// the SGX DCAP library, in QuoteConstants.h. This structure is copied,
// byte-for-byte, from a buffer returned by the Intel QE.
struct IntelQeQuoteHeader {
  uint16_t version;
  uint16_t algorithm;
  UnsafeBytes<4> reserved1;
  uint16_t qesvn;
  uint16_t pcesvn;
  UnsafeBytes<16> qe_vendor_id;
  UnsafeBytes<20> userdata;
} ABSL_ATTRIBUTE_PACKED;

// The signature format for the ECDSA_P256 signature algorithm. This format is
// taken from the "Intel® SGX Data Center Attestation Primitives: ECDSA Quote
// Library API". This structure is copied, byte-for-byte, from a buffer returned
// by the Intel QE.
struct IntelEcdsaP256QuoteSignature {
  // Signature over header and report body.
  UnsafeBytes<64> body_signature;

  // Public part of the key used to generate |signature|.
  UnsafeBytes<64> public_key;

  // Report describing the QE that certifies the public key used to verify
  // |body_signature|. Report data must be SHA256(|public_key| +
  // |qe_authn_data| + 32 0-bytes).
  ReportBody qe_report;

  // Signature over |qe_report| by the Provisioning Certification Key.
  UnsafeBytes<64> qe_report_signature;
} ABSL_ATTRIBUTE_PACKED;

// Collection of certification data for the Intel Provisioning Certification Key
// that is used to certify the Quoting Enclave.
struct IntelCertData {
  // Type identifier for |qe_cert_data|. Valid values may be found in the SGX
  // DCAP libray, in QuoteConstants.h.
  uint16_t qe_cert_data_type;

  // Data required to verify |qe_report_signature|.
  std::vector<uint8_t> qe_cert_data;
};

// Defines the quote format, which is a remotely-verifiable signature over the
// contents of a REPORT.
struct IntelQeQuote {
  // Contains various identifiers needed by quote verifiers to perform
  // verification.
  IntelQeQuoteHeader header;
  static_assert(sizeof(header) == 48,
                "Size mismatch with Intel SGX ECDSA QuoteGenReference API");

  // Enclave-supplied data which is certified by the Intel QE.
  ReportBody body;
  static_assert(sizeof(body) == 384,
                "Size mismatch with Intel SGX ECDSA QuoteGenReference API");

  // Signature over header & body.
  IntelEcdsaP256QuoteSignature signature;
  static_assert(sizeof(signature) == 64 + 64 + 384 + 64,
                "Size mismatch with Intel SGX ECDSA QuoteGenReference API");

  // Optional authentication data for the Intel QE.
  std::vector<uint8_t> qe_authn_data;

  // Intel-provided certification data for the PCE, which certifies the QE.
  IntelCertData cert_data;
};

// Parses a |packed_quote| that was generated by the Intel DCAP library, which
// generates quotes into a contiguous byte buffer. The output is a structured,
// verifiable quote.
StatusOr<IntelQeQuote> ParseDcapPackedQuote(ByteContainerView packed_quote);

}  // namespace sgx
}  // namespace asylo

#endif  // ASYLO_IDENTITY_ATTESTATION_SGX_INTERNAL_INTEL_ECDSA_QUOTE_H_
