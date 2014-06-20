/*
 * Copyright (C) 2014 Open Source Robotics Foundation
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

#include <czmq.h>
#include <uuid/uuid.h>
#include <zmq.hpp>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include "ignition/transport/DiscoveryPrivate.hh"
#include "ignition/transport/Packet.hh"
#include "ignition/transport/TransportTypes.hh"

using namespace ignition;
using namespace transport;

//////////////////////////////////////////////////
DiscoveryPrivate::DiscoveryPrivate(const std::string &_pUuid, bool _verbose)
  : pUuid(_pUuid),
    silenceInterval(DefSilenceInterval),
    activityInterval(DefActivityInterval),
    advertiseInterval(DefAdvertiseInterval),
    heartbitInterval(DefHeartbitInterval),
    connectionCb(nullptr),
    disconnectionCb(nullptr),
    verbose(_verbose),
    exit(false)
{
  this->ctx = zctx_new();

  // Discovery beacon.
  this->beacon = zbeacon_new(this->ctx, this->DiscoveryPort);
  zbeacon_subscribe(this->beacon, NULL, 0);

  // Get this host IP address.
  this->hostAddr = this->GetHostAddr();

  // Start the thread that receives discovery information.
  this->threadReception =
    new std::thread(&DiscoveryPrivate::RunReceptionTask, this);

  // Start the thread that sends heartbeats.
  this->threadHeartbit =
    new std::thread(&DiscoveryPrivate::RunHeartbitTask, this);

  // Start the thread that checks the topic information validity.
  this->threadActivity =
    new std::thread(&DiscoveryPrivate::RunActivityTask, this);

  if (this->verbose)
    this->PrintCurrentState();
}

//////////////////////////////////////////////////
DiscoveryPrivate::~DiscoveryPrivate()
{
  // Tell the service thread to terminate.
  this->exitMutex.lock();
  this->exit = true;
  this->exitMutex.unlock();

  // Wait for the service threads to finish before exit.
  this->threadReception->join();
  this->threadHeartbit->join();
  this->threadActivity->join();

  // Broadcast a BYE message to trigger the remote cancellation of
  // all our advertised topics.
  this->SendMsg(ByeType, "", "", "", "", Scope::All);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Stop all the beacons.
  for (auto &topic : this->beacons)
  {
    for (auto &proc : topic.second)
    {
      zbeacon_t *b = proc.second;
      // Destroy the beacon.
      zbeacon_silence(b);
      zbeacon_destroy(&b);
    }
  }

  zbeacon_destroy(&this->beacon);
  zctx_destroy(&this->ctx);

  // Debug
  // std::cout << "~DiscoveryPrivate()" << std::endl;
}

//////////////////////////////////////////////////
void DiscoveryPrivate::Advertise(const MsgType &_advType,
  const std::string &_topic, const std::string &_addr, const std::string &_ctrl,
  const std::string &_nUuid, const Scope &_scope)
{
  std::lock_guard<std::mutex> lock(this->mutex);

  // Add the addressing information (local node).
  this->info.AddAddress(_topic, _addr, _ctrl, this->pUuid, _nUuid, _scope);

  // If the scope is 'Process', do not advertise a message outside this process.
  if (_scope == Scope::Process)
    return;

  // Broadcast periodically my topic information.
  this->NewBeacon(_advType, _topic, _nUuid);
}

//////////////////////////////////////////////////
void DiscoveryPrivate::Unadvertise(const MsgType &_unadvType,
  const std::string &_topic, const std::string &_nUuid)
{
  std::lock_guard<std::mutex> lock(this->mutex);

  // Don't do anything if the topic is not advertised by any of my nodes.
  Address_t inf;
  if (!this->info.GetAddress(_topic, this->pUuid, _nUuid, inf))
    return;

  // Remove the topic information.
  this->info.DelAddressByNode(_topic, this->pUuid, _nUuid);

  // Do not advertise a message outside the process if the scope is 'Process'.
  if (inf.scope == Scope::Process)
    return;

  // Send the UNADVERTISE message.
  if (_unadvType == MsgType::Msg)
    this->SendMsg(UnadvType, _topic, inf.addr, inf.ctrl, _nUuid, inf.scope);
  else
    this->SendMsg(UnadvSrvType, _topic, inf.addr, inf.ctrl, _nUuid, inf.scope);

  // Remove the beacon for this topic in this node.
  this->DelBeacon(_topic, _nUuid);
}

//////////////////////////////////////////////////
void DiscoveryPrivate::Discover(const std::string &_topic, bool _isSrvCall)
{
  std::lock_guard<std::mutex> lock(this->mutex);

  if (_isSrvCall)
  {
    // Broadcast a discovery request for this service call.
    this->SendMsg(SubSrvType, _topic, "", "", "", Scope::All);
  }
  else
  {
    // Broadcast a discovery request for this topic.
    this->SendMsg(SubType, _topic, "", "", "", Scope::All);
  }

  // I already have information about this topic.
  if (this->info.HasTopic(_topic))
  {
    Addresses_M addresses;
    if (this->info.GetAddresses(_topic, addresses))
    {
      for (auto &proc : addresses)
      {
        for (auto &node : proc.second)
        {
          if (_isSrvCall && this->connectionSrvCb)
          {
            // Execute the user's callback for a service call request. Notice
            // that we only execute one callback for preventing receive multiple
            // service responses for a single request.
            this->connectionSrvCb(_topic, node.addr, node.ctrl, proc.first,
              node.nUuid, node.scope);
            return;
          }
          else if (!_isSrvCall && this->connectionCb)
          {
            // Execute the user's callback.
            this->connectionCb(_topic, node.addr, node.ctrl, proc.first,
              node.nUuid, node.scope);
          }
        }
      }
    }
  }
}

//////////////////////////////////////////////////
void DiscoveryPrivate::RunActivityTask()
{
  while (!zctx_interrupted)
  {
    this->mutex.lock();

    Timestamp now = std::chrono::steady_clock::now();
    for (auto it = this->activity.cbegin(); it != this->activity.cend();)
    {
      // Skip my own entry.
      if (it->first == this->pUuid)
        continue;

      // Elapsed time since the last update from this publisher.
      std::chrono::duration<double> elapsed = now - it->second;

      // This publisher has expired.
      if (std::chrono::duration_cast<std::chrono::milliseconds>
           (elapsed).count() > this->silenceInterval)
      {
        // Remove all the info entries for this process UUID.
        this->info.DelAddressesByProc(it->first);

        // Notify without topic information. This is useful to inform the client
        // that a remote node is gone, even if we were not interested in its
        // topics.
        this->disconnectionCb("", "", "", it->first, "", Scope::All);

        // Remove the activity entry.
        this->activity.erase(it++);
      }
      else
        ++it;
    }
    this->mutex.unlock();

    std::this_thread::sleep_for(
      std::chrono::milliseconds(this->activityInterval));

    // Is it time to exit?
    {
      std::lock_guard<std::mutex> lock(this->exitMutex);
      if (this->exit)
        break;
    }
  }
}

//////////////////////////////////////////////////
void DiscoveryPrivate::RunHeartbitTask()
{
  while (!zctx_interrupted)
  {
    this->mutex.lock();
    this->SendMsg(HelloType, "", "", "", "", Scope::All);
    this->mutex.unlock();

    std::this_thread::sleep_for(
      std::chrono::milliseconds(this->heartbitInterval));

    // Is it time to exit?
    {
      std::lock_guard<std::mutex> lock(this->exitMutex);
      if (this->exit)
        break;
    }
  }
}

//////////////////////////////////////////////////
void DiscoveryPrivate::RunReceptionTask()
{
  while (!zctx_interrupted)
  {
    // Poll socket for a reply, with timeout.
    zmq::pollitem_t items[] =
    {
      {zbeacon_socket(this->beacon), 0, ZMQ_POLLIN, 0}
    };
    zmq::poll(&items[0], sizeof(items) / sizeof(items[0]), this->Timeout);

    //  If we got a reply, process it.
    if (items[0].revents & ZMQ_POLLIN)
    {
      this->RecvDiscoveryUpdate();

      if (this->verbose)
        this->PrintCurrentState();
    }

    // Is it time to exit?
    {
      std::lock_guard<std::mutex> lock(this->exitMutex);
      if (this->exit)
        break;
    }
  }

  this->exit = true;
}

//////////////////////////////////////////////////
void DiscoveryPrivate::RecvDiscoveryUpdate()
{
  std::lock_guard<std::mutex> lock(this->mutex);

  // Address of datagram source.
  char *srcAddr = zstr_recv(zbeacon_socket(this->beacon));

  // A zmq message.
  zframe_t *frame = zframe_recv(zbeacon_socket(this->beacon));

  // Pointer to the raw discovery data.
  byte *data = zframe_data(frame);

  if (this->verbose)
    std::cout << "\nReceived discovery update from " << srcAddr << std::endl;

  this->DispatchDiscoveryMsg(std::string(srcAddr),
    reinterpret_cast<char*>(&data[0]));

  zstr_free(&srcAddr);
  zframe_destroy(&frame);
}

//////////////////////////////////////////////////
void DiscoveryPrivate::DispatchDiscoveryMsg(const std::string &_fromIp,
                                            char *_msg)
{
  Header header;
  char *pBody = _msg;

  // Create the header from the raw bytes.
  header.Unpack(_msg);
  pBody += header.GetHeaderLength();

  std::string topic = header.GetTopic();
  std::string recvPUuid = header.GetPUuid();

  // Discard our own discovery messages.
  if (recvPUuid == this->pUuid)
    return;

  // Update timestamp.
  this->activity[recvPUuid] = std::chrono::steady_clock::now();

  switch (header.GetType())
  {
    case AdvType:
    case AdvSrvType:
    {
      // Read the address.
      AdvMsg advMsg;
      advMsg.UnpackBody(pBody);
      auto recvAddr = advMsg.GetAddress();
      auto recvCtrl = advMsg.GetControlAddress();
      auto recvNUuid = advMsg.GetNodeUuid();
      auto recvScope = advMsg.GetScope();

      // Check scope of the topic.
      if ((recvScope == Scope::Process) ||
          (recvScope == Scope::Host && _fromIp != this->hostAddr))
      {
        return;
      }

      // Register an advertised address for the topic.
      bool added = this->info.AddAddress(topic, recvAddr, recvCtrl,
        recvPUuid, recvNUuid, recvScope);

      if (added)
      {
        if (header.GetType() == AdvType && this->connectionCb)
        {
          // Execute the client's callback.
          this->connectionCb(topic, recvAddr, recvCtrl, recvPUuid,
            recvNUuid, recvScope);
        }
        else if (header.GetType() == AdvSrvType && this->connectionSrvCb)
        {
          // Execute the client's callback for service calls.
          this->connectionSrvCb(topic, recvAddr, recvCtrl, recvPUuid,
            recvNUuid, recvScope);
        }
      }
      break;
    }
    case SubType:
    case SubSrvType:
    {
      // Check if at least one of my nodes advertises the topic requested.
      if (this->info.HasAnyAddresses(topic, this->pUuid))
      {
        Addresses_M addresses;
        if (this->info.GetAddresses(topic, addresses))
        {
          for (auto nodeInfo : addresses[this->pUuid])
          {
            // Check scope of the topic.
            if ((nodeInfo.scope == Scope::Process) ||
                (nodeInfo.scope == Scope::Host && _fromIp != this->hostAddr))
            {
              continue;
            }

            // Answer an ADVERTISE message.
            this->SendMsg(AdvType, topic, nodeInfo.addr, nodeInfo.ctrl,
              nodeInfo.nUuid, nodeInfo.scope);
          }
        }
      }

      break;
    }
    case HelloType:
    {
      // The timestamp has already been updated.
      break;
    }
    case ByeType:
    {
      // Remove the activity entry for this publisher.
      this->activity.erase(recvPUuid);

      if (this->disconnectionCb)
      {
        // Notify the new disconnection.
        this->disconnectionCb("", "", "", recvPUuid, "", Scope::All);
      }

      // Remove the address entry for this topic.
      this->info.DelAddressesByProc(recvPUuid);

      break;
    }
    case UnadvType:
    case UnadvSrvType:
    {
      // Read the address.
      AdvMsg advMsg;
      advMsg.UnpackBody(pBody);
      auto recvAddr = advMsg.GetAddress();
      auto recvCtrl = advMsg.GetControlAddress();
      auto recvNUuid = advMsg.GetNodeUuid();
      auto recvScope = advMsg.GetScope();

      // Check scope of the topic.
      if ((recvScope == Scope::Process) ||
          (recvScope == Scope::Host && _fromIp != this->hostAddr))
      {
        return;
      }

      if (header.GetType() == UnadvType && this->disconnectionCb)
      {
        // Notify the new disconnection.
        this->disconnectionCb(topic, recvAddr, recvCtrl, recvPUuid,
          recvNUuid, recvScope);
      }
      else if (header.GetType() == UnadvSrvType && this->disconnectionSrvCb)
      {
        // Notify the new disconnection.
        this->disconnectionSrvCb(topic, recvAddr, recvCtrl, recvPUuid,
          recvNUuid, recvScope);
      }

      // Remove the address entry for this topic.
      this->info.DelAddressByNode(topic, recvPUuid, recvNUuid);

      break;
    }
    default:
    {
      std::cerr << "Unknown message type [" << header.GetType() << "]\n";
      break;
    }
  }
}

//////////////////////////////////////////////////
int DiscoveryPrivate::SendMsg(uint8_t _type, const std::string &_topic,
  const std::string &_addr, const std::string &_ctrl, const std::string &_nUuid,
  const Scope &_scope, int _flags)
{
  zbeacon_t *aBeacon = zbeacon_new(this->ctx, this->DiscoveryPort);

  // Create the header.
  Header header(Version, this->pUuid, _topic, _type, _flags);

  switch (_type)
  {
    case AdvType:
    case UnadvType:
    case AdvSrvType:
    case UnadvSrvType:
    {
      // Create the [UN]ADVERTISE message.
      AdvMsg advMsg(header, _addr, _ctrl, _nUuid, _scope);

      // Create a buffer and serialize the message.
      std::vector<char> buffer(advMsg.GetMsgLength());
      advMsg.Pack(reinterpret_cast<char*>(&buffer[0]));

      // Broadcast the message.
      zbeacon_publish(aBeacon,
        reinterpret_cast<unsigned char*>(&buffer[0]), advMsg.GetMsgLength());

      break;
    }
    case SubType:
    case HelloType:
    case ByeType:
    {
      // Create a buffer and serialize the message.
      std::vector<char> buffer(header.GetHeaderLength());
      header.Pack(reinterpret_cast<char*>(&buffer[0]));

      // Broadcast the message.
      zbeacon_publish(aBeacon,
        reinterpret_cast<unsigned char*>(&buffer[0]), header.GetHeaderLength());

      break;
    }
    default:
      break;
  }

  zbeacon_silence(aBeacon);
  zbeacon_destroy(&aBeacon);

  if (this->verbose)
  {
    std::cout << "\t* Sending " << MsgTypesStr[_type]
              << " msg [" << _topic << "]" << std::endl;
  }

  return 0;
}

//////////////////////////////////////////////////
std::string DiscoveryPrivate::GetHostAddr()
{
  std::lock_guard<std::mutex> lock(this->mutex);
  return zbeacon_hostname(this->beacon);
}

//////////////////////////////////////////////////
void DiscoveryPrivate::PrintCurrentState()
{
  std::cout << "---------------" << std::endl;
  std::cout << "Discovery state" << std::endl;
  std::cout << "\tUUID: " << this->pUuid << std::endl;
  std::cout << "Settings" << std::endl;
  std::cout << "\tActivity: " << this->activityInterval << " ms." << std::endl;
  std::cout << "\tHeartbit: " << this->heartbitInterval << " ms." << std::endl;
  std::cout << "\tRetrans.: " << this->advertiseInterval << " ms."
    << std::endl;
  std::cout << "\tSilence: " << this->silenceInterval << " ms." << std::endl;
  std::cout << "Known topics" << std::endl;
  this->info.Print();

  // Used to calculate the elapsed time.
  Timestamp now = std::chrono::steady_clock::now();

  std::cout << "Activity" << std::endl;
  if (this->activity.empty())
    std::cout << "\t<empty>" << std::endl;
  else
  {
    for (auto &proc : this->activity)
    {
      // Elapsed time since the last update from this publisher.
      std::chrono::duration<double> elapsed = now - proc.second;

      std::cout << "\t" << proc.first << std::endl;
      std::cout << "\t\t" << "Since: " << std::chrono::duration_cast<
        std::chrono::milliseconds>(elapsed).count() << " ms. ago. "
        << std::endl;
    }
  }
  std::cout << "---------------" << std::endl;
}

//////////////////////////////////////////////////
void DiscoveryPrivate::NewBeacon(const MsgType &_advType,
  const std::string &_topic, const std::string &_nUuid)
{
  std::unique_ptr<Header> header;

  if (this->beacons.find(_topic) == this->beacons.end() ||
      this->beacons[_topic].find(_nUuid) == this->beacons[_topic].end())
  {
    // Create a new beacon and set the advertise interval.
    zbeacon_t *b = zbeacon_new(this->ctx, this->DiscoveryPort);
    zbeacon_set_interval(b, this->advertiseInterval);
    this->beacons[_topic][_nUuid] = b;

    // Prepare the content for the beacon.
    Address_t node;
    if (!this->info.GetAddress(_topic, this->pUuid, _nUuid, node))
      return;

    // Create the header.
    if (_advType == MsgType::Msg)
      header.reset(new Header(Version, this->pUuid, _topic, AdvType));
    else
      header.reset(new Header(Version, this->pUuid, _topic, AdvSrvType));

    // Create the ADVERTISE message.
    AdvMsg advMsg(*header, node.addr, node.ctrl, node.nUuid, node.scope);
    std::vector<char> buffer(advMsg.GetMsgLength());
    advMsg.Pack(reinterpret_cast<char*>(&buffer[0]));

    // Setup the beacon.
    zbeacon_publish(
      b, reinterpret_cast<unsigned char*>(&buffer[0]), advMsg.GetMsgLength());
  }
}

//////////////////////////////////////////////////
void DiscoveryPrivate::DelBeacon(const std::string &_topic,
                                 const std::string &_nUuid)
{
  if (this->beacons.find(_topic) == this->beacons.end())
    return;

  if (this->beacons[_topic].find(_nUuid) == this->beacons[_topic].end())
    return;

  // Get the beacon.
  zbeacon_t *b = this->beacons[_topic][_nUuid];

  // Destroy the beacon.
  zbeacon_silence(b);
  zbeacon_destroy(&b);

  // Remove the beacon from the map.
  this->beacons[_topic].erase(_nUuid);
  if (this->beacons[_topic].empty())
    this->beacons.erase(_topic);
}
