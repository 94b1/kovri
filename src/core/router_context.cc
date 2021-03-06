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

#include "router_context.h"

#include <boost/lexical_cast.hpp>

#include <cryptopp/dh.h>
#include <cryptopp/dsa.h>

#include <fstream>
#include <string>

#include "i2np_protocol.h"
#include "net_db.h"
#include "version.h"
#include "util/mtu.h"
#include "util/timestamp.h"
#include "util/filesystem.h"

namespace i2p {

RouterContext context;

RouterContext::RouterContext()
    : m_LastUpdateTime(0),
      m_AcceptsTunnels(true),
      m_IsFloodfill(false),
      m_StartupTime(0),
      m_Status(eRouterStatusOK),
      m_Port(0),
      m_DataPath(),
      m_ReseedSkipSSLCheck(false),
      m_SupportsNTCP(true),
      m_SupportsSSU(true) {}

void RouterContext::Init(
    const std::string& host,
    int port,
    const boost::filesystem::path& dataPath) {
  m_Host = host;
  m_Port = port;
  m_DataPath = dataPath;
  m_StartupTime = i2p::util::GetSecondsSinceEpoch();
  if (!Load())
    CreateNewRouter();
  UpdateRouterInfo();
}

void RouterContext::CreateNewRouter() {
  m_Keys = i2p::data::PrivateKeys::CreateRandomKeys(
      i2p::data::DEFAULT_ROUTER_SIGNING_KEY_TYPE);
  SaveKeys();
  NewRouterInfo();
}

void RouterContext::NewRouterInfo() {
  i2p::data::RouterInfo routerInfo;
  routerInfo.SetRouterIdentity(
      GetIdentity());
  routerInfo.AddSSUAddress(m_Host, m_Port, routerInfo.GetIdentHash());
  routerInfo.AddNTCPAddress(m_Host, m_Port);
  routerInfo.SetCaps(
    i2p::data::RouterInfo::eReachable |
    i2p::data::RouterInfo::eSSUTesting |
    i2p::data::RouterInfo::eSSUIntroducer);
  routerInfo.SetProperty("netId", NETWORK_ID);
  routerInfo.SetProperty("router.version", I2P_VERSION);
  routerInfo.CreateBuffer(m_Keys);
  m_RouterInfo.Update(
      routerInfo.GetBuffer(),
      routerInfo.GetBufferLen());
}

void RouterContext::UpdateRouterInfo() {
  m_RouterInfo.CreateBuffer(m_Keys);
  m_RouterInfo.SaveToFile(i2p::util::filesystem::GetFullPath(ROUTER_INFO));
  m_LastUpdateTime = i2p::util::GetSecondsSinceEpoch();
}

void RouterContext::UpdatePort(
    int port) {
  bool updated = false;
  for (auto& address : m_RouterInfo.GetAddresses()) {
    if (address.port != port) {
      address.port = port;
      updated = true;
    }
  }
  if (updated)
    UpdateRouterInfo();
}

void RouterContext::UpdateAddress(
    const boost::asio::ip::address& host) {
  bool updated = false;
  for (auto& address : m_RouterInfo.GetAddresses()) {
    if (address.host != host && address.IsCompatible(host)) {
      address.host = host;
      updated = true;
    }
  }
  auto ts = i2p::util::GetSecondsSinceEpoch();
  if (updated || ts > m_LastUpdateTime + ROUTER_INFO_UPDATE_INTERVAL)
    UpdateRouterInfo();
}

bool RouterContext::AddIntroducer(
    const i2p::data::RouterInfo& routerInfo,
    uint32_t tag) {
  bool ret = false;
  auto address = routerInfo.GetSSUAddress();
  if (address) {
    ret = m_RouterInfo.AddIntroducer(address, tag);
    if (ret)
      UpdateRouterInfo();
  }
  return ret;
}

void RouterContext::RemoveIntroducer(
    const boost::asio::ip::udp::endpoint& e) {
  if (m_RouterInfo.RemoveIntroducer(e))
    UpdateRouterInfo();
}

void RouterContext::SetFloodfill(
    bool floodfill) {
  m_IsFloodfill = floodfill;
  if (floodfill) {
    m_RouterInfo.SetCaps(
        m_RouterInfo.GetCaps() | i2p::data::RouterInfo::eFloodfill);
  } else {
    m_RouterInfo.SetCaps(
        m_RouterInfo.GetCaps() & ~i2p::data::RouterInfo::eFloodfill);
    // we don't publish number of routers and leaseset for non-floodfill
    m_RouterInfo.DeleteProperty(ROUTER_INFO_PROPERTY_LEASESETS);
    m_RouterInfo.DeleteProperty(ROUTER_INFO_PROPERTY_ROUTERS);
  }
  UpdateRouterInfo();
}

void RouterContext::SetHighBandwidth() {
  if (!m_RouterInfo.IsHighBandwidth()) {
    m_RouterInfo.SetCaps(
        m_RouterInfo.GetCaps() | i2p::data::RouterInfo::eHighBandwidth);
    UpdateRouterInfo();
  }
}

void RouterContext::SetLowBandwidth() {
  if (m_RouterInfo.IsHighBandwidth()) {
    m_RouterInfo.SetCaps(
        m_RouterInfo.GetCaps() & ~i2p::data::RouterInfo::eHighBandwidth);
    UpdateRouterInfo();
  }
}

bool RouterContext::IsUnreachable() const {
  return m_RouterInfo.GetCaps() & i2p::data::RouterInfo::eUnreachable;
}

void RouterContext::SetUnreachable() {
  // set caps
  m_RouterInfo.SetCaps(  // LU, B
      i2p::data::RouterInfo::eUnreachable | i2p::data::RouterInfo::eSSUTesting);
  // remove NTCP address
  RemoveTransport(i2p::data::RouterInfo::eTransportNTCP);
  // delete previous introducers
  for (auto& addr : m_RouterInfo.GetAddresses())
    addr.introducers.clear();
  // update
  UpdateRouterInfo();
}

void RouterContext::SetReachable() {
  // update caps
  uint8_t caps = m_RouterInfo.GetCaps();
  caps &= ~i2p::data::RouterInfo::eUnreachable;
  caps |= i2p::data::RouterInfo::eReachable;
  caps |= i2p::data::RouterInfo::eSSUIntroducer;
  if (m_IsFloodfill)
    caps |= i2p::data::RouterInfo::eFloodfill;
  m_RouterInfo.SetCaps(caps);

  // insert NTCP back
  auto& addresses = m_RouterInfo.GetAddresses();
  for (size_t i = 0; i < addresses.size(); i++) {
    if (addresses[i].transport_style == i2p::data::RouterInfo::eTransportSSU) {
      // insert NTCP address with host/port form SSU
      m_RouterInfo.AddNTCPAddress(
          addresses[i].host.to_string().c_str(),
          addresses[i].port);
      break;
    }
  }
  // delete previous introducers
  for (auto& addr : addresses)
    addr.introducers.clear();
  // update
  UpdateRouterInfo();
}

void RouterContext::SetSupportsV6(
    bool supportsV6) {
  if (supportsV6)
    m_RouterInfo.EnableV6();
  else
    m_RouterInfo.DisableV6();
  UpdateRouterInfo();
}

void RouterContext::SetSupportsNTCP(bool supportsNTCP) {
  m_SupportsNTCP = supportsNTCP;
  if(supportsNTCP && !m_RouterInfo.GetNTCPAddress())
    m_RouterInfo.AddNTCPAddress(m_Host, m_Port);
  if(!supportsNTCP)
    RemoveTransport(i2p::data::RouterInfo::eTransportNTCP);
  UpdateRouterInfo();
}

void RouterContext::SetSupportsSSU(bool supportsSSU) {
  m_SupportsSSU = supportsSSU;
  if(supportsSSU && !m_RouterInfo.GetSSUAddress())
    m_RouterInfo.AddSSUAddress(m_Host, m_Port, m_RouterInfo.GetIdentHash());
  if(!supportsSSU)
    RemoveTransport(i2p::data::RouterInfo::eTransportSSU);
  // Remove SSU-related flags
  m_RouterInfo.SetCaps(m_RouterInfo.GetCaps()
      & ~i2p::data::RouterInfo::eSSUTesting
      & ~i2p::data::RouterInfo::eSSUIntroducer);

  UpdateRouterInfo();
}

void RouterContext::UpdateNTCPV6Address(
    const boost::asio::ip::address& host) {
  bool updated = false,
       found = false;
  int port = 0;
  auto& addresses = m_RouterInfo.GetAddresses();
  for (auto& addr : addresses) {
    if (addr.host.is_v6() &&
        addr.transport_style == i2p::data::RouterInfo::eTransportNTCP) {
      if (addr.host != host) {
        addr.host = host;
        updated = true;
      }
      found = true;
    } else {
      port = addr.port;
    }
  }
  if (!found) {
    // create new address
    m_RouterInfo.AddNTCPAddress(
        host.to_string().c_str(),
        port);
    m_RouterInfo.AddSSUAddress(
        host.to_string().c_str(),
        port,
        GetIdentHash(),
        i2p::util::mtu::GetMTU(host));
    updated = true;
  }
  if (updated)
    UpdateRouterInfo();
}

void RouterContext::UpdateStats() {
  if (m_IsFloodfill) {
    // update routers and leasesets
    m_RouterInfo.SetProperty(
        ROUTER_INFO_PROPERTY_LEASESETS,
        boost::lexical_cast<std::string>(i2p::data::netdb.GetNumLeaseSets()));
    m_RouterInfo.SetProperty(
        ROUTER_INFO_PROPERTY_ROUTERS,
        boost::lexical_cast<std::string>(i2p::data::netdb.GetNumRouters()));
    UpdateRouterInfo();
  }
}

bool RouterContext::Load() {
  std::ifstream fk(
      i2p::util::filesystem::GetFullPath(ROUTER_KEYS).c_str(),
      std::ifstream::binary | std::ofstream::in);
  if (!fk.is_open())
    return false;
  
  fk.seekg(0, std::ios::end);
  const size_t len = fk.tellg();
  fk.seekg(0, std::ios::beg);
  std::unique_ptr<uint8_t[]> buf(std::make_unique<uint8_t[]>(len));
  fk.read(reinterpret_cast<char*>(buf.get()), len);
  m_Keys.FromBuffer(buf.get(), len);

  i2p::data::RouterInfo routerInfo(
      i2p::util::filesystem::GetFullPath(ROUTER_INFO));
  m_RouterInfo.Update(
      routerInfo.GetBuffer(),
      routerInfo.GetBufferLen());
  m_RouterInfo.SetProperty("coreVersion", I2P_VERSION);
  m_RouterInfo.SetProperty("router.version", I2P_VERSION);
  if (IsUnreachable())
    // we assume reachable until we discover firewall through peer tests
    SetReachable();
  return true;
}

void RouterContext::SaveKeys() {
  std::ofstream fk(
      i2p::util::filesystem::GetFullPath(ROUTER_KEYS).c_str(),
      std::ofstream::binary | std::ofstream::out);
  const size_t len = m_Keys.GetFullLen();
  std::unique_ptr<uint8_t[]> buf(std::make_unique<uint8_t[]>(len));
  m_Keys.ToBuffer(buf.get(), len);
  fk.write(reinterpret_cast<char*>(buf.get()), len);
}

void RouterContext::RemoveTransport(
    i2p::data::RouterInfo::TransportStyle transport) {
  auto& addresses = m_RouterInfo.GetAddresses();
  for (size_t i = 0; i < addresses.size(); i++) {
    if (addresses[i].transport_style == transport) {
      addresses.erase(addresses.begin() + i);
      break;
    }
  }
}

std::shared_ptr<i2p::tunnel::TunnelPool> RouterContext::GetTunnelPool() const {
  return i2p::tunnel::tunnels.GetExploratoryPool();
}

void RouterContext::HandleI2NPMessage(
    const uint8_t* buf,
    size_t,
    std::shared_ptr<i2p::tunnel::InboundTunnel> from) {
  i2p::HandleI2NPMessage(
      CreateI2NPMessage(
        buf,
        GetI2NPMessageLength(buf),
        from));
}

void RouterContext::ProcessGarlicMessage(
    std::shared_ptr<I2NPMessage> msg) {
  std::unique_lock<std::mutex> l(m_GarlicMutex);
  i2p::garlic::GarlicDestination::ProcessGarlicMessage(msg);
}

void RouterContext::ProcessDeliveryStatusMessage(
    std::shared_ptr<I2NPMessage> msg) {
  std::unique_lock<std::mutex> l(m_GarlicMutex);
  i2p::garlic::GarlicDestination::ProcessDeliveryStatusMessage(msg);
}

uint32_t RouterContext::GetUptime() const {
  return i2p::util::GetSecondsSinceEpoch () - m_StartupTime;
}

}  // namespace i2p
