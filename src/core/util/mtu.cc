/**                                                                                           //
 * Copyright (c) 2013-2016, The Kovri I2P Router Project                                      //
 *                                                                                            //
 * All rights reserved.                                                                       //
 *                                                                                            //
 * Redistribution and use in source and binary forms, with or without modification, are       //
 * permitted provided that the following conditions are met:                                  //
 *                                                                                            //
 * 1. Redistributions of source code must retain the above copyright notice, this list of     //
 *    conditions and the following disclaimer.                                                //
 *                                                                                            //
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list     //
 *    of conditions and the following disclaimer in the documentation and/or other            //
 *    materials provided with the distribution.                                               //
 *                                                                                            //
 * 3. Neither the name of the copyright holder nor the names of its contributors may be       //
 *    used to endorse or promote products derived from this software without specific         //
 *    prior written permission.                                                               //
 *                                                                                            //
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY        //
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF    //
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL     //
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,       //
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,               //
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS    //
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,          //
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF    //
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.               //
 *                                                                                            //
 * Parts of the project are originally copyright (c) 2013-2015 The PurpleI2P Project          //
 */

#include "mtu.h"
#if defined(__linux__) || \
    defined(__FreeBSD_kernel__) || \
    defined(__APPLE__) || \
    defined(__OpenBSD__)
#include <ifaddrs.h>
#include <sys/types.h>
#elif defined(_WIN32)
#include <iphlpapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <string>

#pragma comment(lib, "IPHLPAPI.lib")

#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))

// This function was written by Petar Korponai?. See
// http://stackoverflow.com/questions/15660203/inet-pton-identifier-not-found
int inet_pton(
    int af,
    const char* src,
    void* dst) {
  struct sockaddr_storage ss;
  int size = sizeof(ss);
  char src_copy[INET6_ADDRSTRLEN + 1];
  ZeroMemory(&ss, sizeof (ss));
  strncpy_s(src_copy, src, INET6_ADDRSTRLEN + 1);
  src_copy[INET6_ADDRSTRLEN] = 0;
  if (WSAStringToAddress(
        src_copy,
        af,
        NULL,
        (struct sockaddr *)&ss,
        &size) == 0) {
    switch (af) {
      case AF_INET:
        *(struct in_addr *)dst = ((struct sockaddr_in *)&ss)->sin_addr;
        return 1;
      case AF_INET6:
        *(struct in6_addr *)dst = ((struct sockaddr_in6 *)&ss)->sin6_addr;
        return 1;
    }
  }
  return 0;
}
#endif

namespace i2p {
namespace util {
namespace mtu {

#if defined(__linux__) || \
    defined(__FreeBSD_kernel__) || \
    defined(__APPLE__) || \
    defined(__OpenBSD__)
std::uint16_t GetMTUUnix(
    const boost::asio::ip::address& local_address) {
  ifaddrs* ifaddr, *ifa = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    LogPrint(eLogError, "MTU: can't execute getifaddrs()");
    return MTU_FALLBACK;
  }
  int family = 0;
  // look for interface matching local address
  for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr)
      continue;
    family = ifa->ifa_addr->sa_family;
    if (family == AF_INET && local_address.is_v4()) {
      sockaddr_in* sa =  reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
      if (!memcmp(&sa->sin_addr, local_address.to_v4().to_bytes().data(), 4))
        break;  // address matches
    } else if (family == AF_INET6 && local_address.is_v6()) {
      sockaddr_in6* sa =  reinterpret_cast<sockaddr_in6 *>(ifa->ifa_addr);
      if (!memcmp(&sa->sin6_addr, local_address.to_v6().to_bytes().data(), 16))
        break;  // address matches
    }
  }
  auto mtu = MTU_FALLBACK;
  if (ifa && family) {  // interface found?
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
      LogPrint(eLogError,
          "MTU: failed to create datagram socket: ", strerror(errno));
    } else {
      ifreq ifr;
      // set interface for query
      strncpy(ifr.ifr_name, ifa->ifa_name,sizeof(ifr.ifr_name));
      ifr.ifr_name[sizeof(ifr.ifr_name)-1]='\0';
      if (ioctl(fd, SIOCGIFMTU, &ifr) >= 0)
        mtu = ifr.ifr_mtu;  // MTU
      else
        LogPrint(eLogError, "MTU: failed to run ioctl");
      close(fd);
    }
  } else {
    LogPrint(eLogWarn,
        "MTU: interface for local address",
        local_address.to_string(), " not found");
  }
  freeifaddrs(ifaddr);
  return mtu;
}

#elif defined(_WIN32)
std::uint16_t GetMTUWindowsIpv4(
    sockaddr_in inputAddress) {
  ULONG outBufLen = 0;
  PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
  PIP_ADAPTER_ADDRESSES pCurrAddresses = nullptr;
  PIP_ADAPTER_UNICAST_ADDRESS pUnicast = nullptr;
  if (GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_INCLUDE_PREFIX,
        nullptr,
        pAddresses,
        &outBufLen) == ERROR_BUFFER_OVERFLOW) {
    FREE(pAddresses);
    pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES *> (MALLOC(outBufLen));
  }
  DWORD dwRetVal = GetAdaptersAddresses(
      AF_INET,
      GAA_FLAG_INCLUDE_PREFIX,
      nullptr,
      pAddresses,
      &outBufLen);
  if (dwRetVal != NO_ERROR) {
    LogPrint(eLogError,
        "MTU: GetMTUWindowsIpv4() has failed:",
        "enclosed GetAdaptersAddresses() call has failed");
    FREE(pAddresses);
    return MTU_FALLBACK;
  }
  pCurrAddresses = pAddresses;
  while (pCurrAddresses) {
    PIP_ADAPTER_UNICAST_ADDRESS firstUnicastAddress =
      pCurrAddresses->FirstUnicastAddress;
    pUnicast = pCurrAddresses->FirstUnicastAddress;
    if (pUnicast == nullptr) {
      LogPrint(eLogError,
          "MTU: GetMTUWindowsIpv4() has failed:",
          "not a unicast ipv4 address; this is not supported");
    }
    for (int i = 0; pUnicast != nullptr; ++i) {
      LPSOCKADDR lpAddr = pUnicast->Address.lpSockaddr;
      sockaddr_in* localInterfaceAddress =
        reinterpret_cast<sockaddr_in *>(lpAddr);
      if (localInterfaceAddress->sin_addr.S_un.S_addr ==
          inputAddress.sin_addr.S_un.S_addr) {
        auto result = pAddresses->Mtu;
        FREE(pAddresses);
        return result;
      }
      pUnicast = pUnicast->Next;
    }
    pCurrAddresses = pCurrAddresses->Next;
  }
  LogPrint(eLogError,
      "MTU: GetMTUWindowsIpv4() error:",
      "no usable unicast ipv4 addresses found");
  FREE(pAddresses);
  return MTU_FALLBACK;
}

std::uint16_t GetMTUWindowsIpv6(
    sockaddr_in6 inputAddress) {
  ULONG outBufLen = 0;
  PIP_ADAPTER_ADDRESSES pAddresses = nullptr;
  PIP_ADAPTER_ADDRESSES pCurrAddresses = nullptr;
  PIP_ADAPTER_UNICAST_ADDRESS pUnicast = nullptr;
  if (GetAdaptersAddresses(
        AF_INET6,
        GAA_FLAG_INCLUDE_PREFIX,
        nullptr,
        pAddresses,
        &outBufLen) == ERROR_BUFFER_OVERFLOW) {
    FREE(pAddresses);
    pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(MALLOC(outBufLen));
  }
  DWORD dwRetVal = GetAdaptersAddresses(
      AF_INET6,
      GAA_FLAG_INCLUDE_PREFIX,
      nullptr,
      pAddresses,
      &outBufLen);
  if (dwRetVal != NO_ERROR) {
    LogPrint(eLogError,
      "MTU: GetMTUWindowsIpv6() has failed:",
      "enclosed GetAdaptersAddresses() call has failed");
    FREE(pAddresses);
    return MTU_FALLBACK;
  }
  bool found_address = false;
  pCurrAddresses = pAddresses;
  while (pCurrAddresses) {
    PIP_ADAPTER_UNICAST_ADDRESS firstUnicastAddress =
      pCurrAddresses->FirstUnicastAddress;
    pUnicast = pCurrAddresses->FirstUnicastAddress;
    if (pUnicast == nullptr) {
      LogPrint(eLogError,
          "MTU: GetMTUWindowsIpv6() has failed:",
          "not a unicast ipv6 address; this is not supported");
    }
    for (int i = 0; pUnicast != nullptr; ++i) {
      LPSOCKADDR lpAddr = pUnicast->Address.lpSockaddr;
      sockaddr_in6 *localInterfaceAddress =
        reinterpret_cast<sockaddr_in6 *>(lpAddr);
      for (int j = 0; j != 8; ++j) {
        if (localInterfaceAddress->sin6_addr.u.Word[j] !=
            inputAddress.sin6_addr.u.Word[j]) {
          break;
        } else {
          found_address = true;
        }
      }
      if (found_address) {
        auto result = pAddresses->Mtu;
        FREE(pAddresses);
        pAddresses = nullptr;
        return result;
      }
      pUnicast = pUnicast->Next;
    }
    pCurrAddresses = pCurrAddresses->Next;
  }
  LogPrint(eLogError,
      "MTU: GetMTUWindowsIpv6() error:",
      "no usable unicast ipv6 addresses found");
  FREE(pAddresses);
  return MTU_FALLBACK;
}

std::uint16_t GetMTUWindows(
    const boost::asio::ip::address& localAddress) {
#ifdef UNICODE
  string localAddress_temporary = localAddress.to_string();
  wstring localAddressUniversal(
      localAddress_temporary.begin(),
      localAddress_temporary.end());
#else
  std::string localAddressUniversal = localAddress.to_string();
#endif
  if (localAddress.is_v4()) {
    sockaddr_in inputAddress;
    inet_pton(AF_INET, localAddressUniversal.c_str(), &(inputAddress.sin_addr));
    return GetMTUWindowsIpv4(inputAddress);
  } else if (localAddress.is_v6()) {
    sockaddr_in6 inputAddress;
    inet_pton(
        AF_INET6,
        localAddressUniversal.c_str(),
        &(inputAddress.sin6_addr));
    return GetMTUWindowsIpv6(inputAddress);
  } else {
    LogPrint(eLogError,
        "MTU: GetMTUWindows() has failed:",
        "address family is not supported");
    return MTU_FALLBACK;
  }
}
#endif  // WIN32

std::uint16_t GetMTU(
    const boost::asio::ip::address& local_address) {
#if defined(__linux__) || \
    defined(__FreeBSD_kernel__) || \
    defined(__APPLE__) || \
    defined(__OpenBSD__)
  auto mtu = GetMTUUnix(local_address);
#elif defined(WIN32)
  auto mtu = GetMTUWindows(local_address);
#else
  auto mtu = MTU_FALLBACK;
#endif
  LogPrint(eLogDebug, "MTU: our MTU=", mtu);
  if (!mtu || mtu > MTU_MAX) {
    LogPrint(eLogDebug, "MTU: scaling MTU to ", MTU_MAX);
    mtu = MTU_MAX;
  }
  return mtu;
}

}  // namespace mtu
}  // namespace util
}  // namespace i2p
