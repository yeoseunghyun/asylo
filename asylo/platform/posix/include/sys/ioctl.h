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


#include_next <sys/ioctl.h>

#ifndef ASYLO_PLATFORM_POSIX_INCLUDE_SYS_IOCTL_H_
#define ASYLO_PLATFORM_POSIX_INCLUDE_SYS_IOCTL_H_

#ifndef ASYLO_TRANSITIONAL_DEFINE_FOR_IOCTL

#include <stdint.h>


// Definitions of IOCTL requests supported in the enclave. Can be extended as
// needed. Note: if/when will need to support standard IOCTLs, may need to
// define and use ioctl enclave-to-host delegates to delegate ioctl to the host.

// Type macro for IOCTL requests sent to the secure storage subsystem.
// [Deprecated location: use asylo/secure_storage.h]
#ifndef ENCLAVE_STORAGE_IOCTL_TYPE
#define ENCLAVE_STORAGE_IOCTL_TYPE 0x00880000
#endif

// IOCTL to set a key on a secure file.
// [Deprecated location: use asylo/secure_storage.h]
#ifndef ENCLAVE_STORAGE_SET_KEY
#define ENCLAVE_STORAGE_SET_KEY (ENCLAVE_STORAGE_IOCTL_TYPE | 0x00000001)
#endif

#define TIOCGWINSZ 0x5413

struct winsize {
  uint16_t ws_row;
  uint16_t ws_col;
  uint16_t ws_xpixel;
  uint16_t ws_ypixel;
};

#ifdef __cplusplus
extern "C" {
#endif

int ioctl(int fd, int request, ...);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ASYLO_TRANSITIONAL_DEFINE_FOR_IOCTL
#endif  // ASYLO_PLATFORM_POSIX_INCLUDE_SYS_IOCTL_H_
