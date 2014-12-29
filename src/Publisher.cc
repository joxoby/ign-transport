/*
 * Copyright (C) 2015 Open Source Robotics Foundation
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

#include <cstring>
#include <iostream>
#include <string>
#include "ignition/transport/Publisher.hh"

using namespace ignition;
using namespace transport;

//////////////////////////////////////////////////
Publisher::Publisher(const std::string &_topic, const std::string &_addr,
  const std::string &_pUuid, const std::string &_nUuid, const Scope_t _scope)
  : topic(_topic),
    addr(_addr),
    pUuid(_pUuid),
    nUuid(_nUuid),
    scope(_scope)
{
}

//////////////////////////////////////////////////
std::string Publisher::Topic() const
{
  return this->topic;
}

//////////////////////////////////////////////////
std::string Publisher::Addr() const
{
  return this->addr;
}

//////////////////////////////////////////////////
std::string Publisher::PUuid() const
{
  return this->pUuid;
}

//////////////////////////////////////////////////
std::string Publisher::NUuid() const
{
  return this->nUuid;
}

//////////////////////////////////////////////////
Scope_t Publisher::Scope() const
{
  return this->scope;
}

//////////////////////////////////////////////////
void Publisher::Topic(const std::string &_topic)
{
  this->topic = _topic;
}

//////////////////////////////////////////////////
void Publisher::Addr(const std::string &_addr)
{
  this->addr = _addr;
}

//////////////////////////////////////////////////
void Publisher::PUuid(const std::string &_pUuid)
{
  this->pUuid = _pUuid;
}

//////////////////////////////////////////////////
void Publisher::NUuid(const std::string &_nUuid)
{
  this->nUuid = _nUuid;
}

//////////////////////////////////////////////////
void Publisher::Scope(const Scope_t _scope)
{
  this->scope = _scope;
}

//////////////////////////////////////////////////
size_t Publisher::Pack(char *_buffer)
{
  if (this->topic.empty() || this->addr.empty() || this->nUuid.empty())
  {
    std::cerr << "Publisher::Pack() error: You're trying to pack an "
              << "incomplete Publisher:" << std::endl << *this;
    return 0;
  }

  // Pack the topic length.
  uint64_t topicLength = this->topic.size();
  memcpy(_buffer, &topicLength, sizeof(topicLength));
  _buffer += sizeof(topicLength);

  // Pack the topic.
  memcpy(_buffer, this->topic.data(), static_cast<size_t>(topicLength));
  _buffer += topicLength;

  // Pack the zeromq address length.
  uint64_t addrLength = this->addr.size();
  memcpy(_buffer, &addrLength, sizeof(addrLength));
  _buffer += sizeof(addrLength);

  // Pack the zeromq address.
  memcpy(_buffer, this->addr.data(), static_cast<size_t>(addrLength));
  _buffer += addrLength;

  // Note: We do not pack the process UUID because it will be packed inside
  // the header.

  // Pack the node UUID length.
  uint64_t nUuidLength = this->nUuid.size();
  memcpy(_buffer, &nUuidLength, sizeof(nUuidLength));
  _buffer += sizeof(nUuidLength);

  // Pack the node UUID.
  memcpy(_buffer, this->nUuid.data(), static_cast<size_t>(nUuidLength));
  _buffer += nUuidLength;

  // Pack the topic scope.
  uint8_t intscope = static_cast<uint8_t>(this->scope);
  memcpy(_buffer, &intscope, sizeof(intscope));

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t Publisher::Unpack(char *_buffer)
{
  // null buffer.
  if (!_buffer)
  {
    std::cerr << "Publisher::Unpack() error: NULL input buffer"
              << std::endl;
    return 0;
  }

  // Unpack the topic length.
  uint64_t topicLength;
  memcpy(&topicLength, _buffer, sizeof(topicLength));
  _buffer += sizeof(topicLength);

  // Unpack the topic.
  this->topic = std::string(_buffer, _buffer + topicLength);
  _buffer += topicLength;

  // Unpack the zeromq address length.
  uint64_t addrLength;
  memcpy(&addrLength, _buffer, sizeof(addrLength));
  _buffer += sizeof(addrLength);

  // Unpack the zeromq address.
  this->addr = std::string(_buffer, _buffer + addrLength);
  _buffer += addrLength;

  // Unpack the node UUID length.
  uint64_t nUuidLength;
  memcpy(&nUuidLength, _buffer, sizeof(nUuidLength));
  _buffer += sizeof(nUuidLength);

  // Unpack the node UUID.
  this->nUuid = std::string(_buffer, _buffer + nUuidLength);
  _buffer += nUuidLength;

  // Unpack the topic scope.
  uint8_t intscope;
  memcpy(&intscope, _buffer, sizeof(intscope));
  this->scope = static_cast<Scope_t>(intscope);

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t Publisher::GetMsgLength()
{
  return sizeof(uint64_t) + this->topic.size() +
         sizeof(uint64_t) + this->addr.size() +
         sizeof(uint64_t) + this->nUuid.size() +
         sizeof(uint8_t);
}

//////////////////////////////////////////////////
MessagePublisher::MessagePublisher(const std::string &_topic,
  const std::string &_addr, const std::string &_ctrl, const std::string &_pUuid,
  const std::string &_nUuid, const Scope_t _scope,
  const std::string &_msgTypeName)
  : Publisher(_topic, _addr, _pUuid, _nUuid, _scope),
    ctrl(_ctrl),
    msgTypeName(_msgTypeName)
{
}

//////////////////////////////////////////////////
size_t MessagePublisher::Pack(char *_buffer)
{
  if (this->ctrl.empty() || this->msgTypeName.empty())
  {
    std::cerr << "MessagePublisher::Pack() error: You're trying to pack an "
              << "incomplete MessagePublisher:" << std::endl << *this;
    return 0;
  }

  // Pack the common part of any Publisher message.
  size_t len = Publisher::Pack(_buffer);
  if (len == 0)
    return 0;

  _buffer += len;

  // Pack the zeromq control address length.
  uint64_t ctrlLength = this->ctrl.size();
  memcpy(_buffer, &ctrlLength, sizeof(ctrlLength));
  _buffer += sizeof(ctrlLength);

  // Pack the zeromq control address.
  memcpy(_buffer, this->ctrl.data(), static_cast<size_t>(ctrlLength));
  _buffer += ctrlLength;

  // Pack the type name length.
  uint64_t typeNameLength = this->msgTypeName.size();
  memcpy(_buffer, &typeNameLength, sizeof(typeNameLength));
  _buffer += sizeof(typeNameLength);

  // Pack the type name.
  memcpy(_buffer, this->msgTypeName.data(),
    static_cast<size_t>(typeNameLength));

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t MessagePublisher::Unpack(char *_buffer)
{
  // null buffer.
  if (!_buffer)
  {
    std::cerr << "MessagePublisher::UnpackBody() error: NULL input buffer"
              << std::endl;
    return 0;
  }

  // Unpack the common part of any Publisher message.
  size_t len = Publisher::Unpack(_buffer);
  if (len == 0)
    return 0;

  _buffer += len;

  // Unpack the zeromq control address length.
  uint64_t ctrlLength;
  memcpy(&ctrlLength, _buffer, sizeof(ctrlLength));
  _buffer += sizeof(ctrlLength);

  // Unpack the zeromq control address.
  this->ctrl = std::string(_buffer, _buffer + ctrlLength);
  _buffer += ctrlLength;

  // Unpack the type name length.
  uint64_t typeNameLength;
  memcpy(&typeNameLength, _buffer, sizeof(typeNameLength));
  _buffer += sizeof(typeNameLength);

  // Unpack the type name.
  this->msgTypeName = std::string(_buffer, _buffer + typeNameLength);

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t MessagePublisher::GetMsgLength()
{
  return Publisher::GetMsgLength() +
         sizeof(uint64_t) + this->ctrl.size() +
         sizeof(uint64_t) + this->msgTypeName.size();
}

//////////////////////////////////////////////////
std::string MessagePublisher::Ctrl() const
{
  return this->ctrl;
}

//////////////////////////////////////////////////
void MessagePublisher::Ctrl(const std::string &_ctrl)
{
  this->ctrl = _ctrl;
}

//////////////////////////////////////////////////
std::string MessagePublisher::MsgTypeName() const
{
  return this->msgTypeName;
}

//////////////////////////////////////////////////
void MessagePublisher::MsgTypeName(const std::string &_msgTypeName)
{
  this->msgTypeName = _msgTypeName;
}

//////////////////////////////////////////////////
ServicePublisher::ServicePublisher(const std::string &_topic,
  const std::string &_addr, const std::string &_socketId,
  const std::string &_pUuid, const std::string &_nUuid, const Scope_t _scope,
  const std::string &_reqType, const std::string &_repType)
  : Publisher(_topic, _addr, _pUuid, _nUuid, _scope),
    socketId(_socketId),
    reqTypeName(_reqType),
    repTypeName(_repType)
{
}

//////////////////////////////////////////////////
size_t ServicePublisher::Pack(char *_buffer)
{
  if (this->socketId.empty() || this->reqTypeName.empty() ||
      this->repTypeName.empty())
  {
    std::cerr << "ServicePublisher::Pack() error: You're trying to pack an "
              << "incomplete ServicePublisher:" << std::endl << *this;
    return 0;
  }

  // Pack the common part of any Publisher message.
  size_t len = Publisher::Pack(_buffer);
  if (len == 0)
    return 0;

  _buffer += len;

  // Pack the socket ID length.
  uint64_t socketIdLength = this->socketId.size();
  memcpy(_buffer, &socketIdLength, sizeof(socketIdLength));
  _buffer += sizeof(socketIdLength);

  // Pack the socket ID.
  memcpy(_buffer, this->socketId.data(), static_cast<size_t>(socketIdLength));
  _buffer += socketIdLength;

  // Pack the request type length.
  uint64_t reqTypeLength = this->reqTypeName.size();
  memcpy(_buffer, &reqTypeLength, sizeof(reqTypeLength));
  _buffer += sizeof(reqTypeLength);

  // Pack the request type.
  memcpy(_buffer, this->reqTypeName.data(), static_cast<size_t>(reqTypeLength));
  _buffer += reqTypeLength;

  // Pack the response type length.
  uint64_t repTypeLength = this->repTypeName.size();
  memcpy(_buffer, &repTypeLength, sizeof(repTypeLength));
  _buffer += sizeof(repTypeLength);

  // Pack the response.
  memcpy(_buffer, this->repTypeName.data(), static_cast<size_t>(repTypeLength));

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t ServicePublisher::Unpack(char *_buffer)
{
  // null buffer.
  if (!_buffer)
  {
    std::cerr << "MessagePublisher::Unpack() error: NULL input buffer"
              << std::endl;
    return 0;
  }

  // Unpack the common part of any Publisher message.
  size_t len = Publisher::Unpack(_buffer);
  if (len == 0)
    return 0;

  _buffer += len;

  // Unpack the socket ID length.
  uint64_t socketIdLength;
  memcpy(&socketIdLength, _buffer, sizeof(socketIdLength));
  _buffer += sizeof(socketIdLength);

  // Unpack the socket ID.
  this->socketId = std::string(_buffer, _buffer + socketIdLength);
  _buffer += socketIdLength;

  // Unpack the request type length.
  uint64_t reqTypeLength;
  memcpy(&reqTypeLength, _buffer, sizeof(reqTypeLength));
  _buffer += sizeof(reqTypeLength);

  // Unpack the request type.
  this->reqTypeName = std::string(_buffer, _buffer + reqTypeLength);
  _buffer += reqTypeLength;

  // Unpack the response type length.
  uint64_t repTypeLength;
  memcpy(&repTypeLength, _buffer, sizeof(repTypeLength));
  _buffer += sizeof(repTypeLength);

  // Unpack the response type.
  this->repTypeName = std::string(_buffer, _buffer + repTypeLength);
  _buffer += repTypeLength;

  return this->GetMsgLength();
}

//////////////////////////////////////////////////
size_t ServicePublisher::GetMsgLength()
{
  return sizeof(uint64_t) + this->topic.size() +
         sizeof(uint64_t) + this->addr.size() +
         sizeof(uint64_t) + this->socketId.size() +
         sizeof(uint64_t) + this->nUuid.size() +
         sizeof(uint8_t) +
         sizeof(uint64_t) + this->reqTypeName.size() +
         sizeof(uint64_t) + this->repTypeName.size();
}

//////////////////////////////////////////////////
std::string ServicePublisher::SocketId() const
{
  return this->socketId;
}

//////////////////////////////////////////////////
void ServicePublisher::SocketId(const std::string &_socketId)
{
  this->socketId = _socketId;
}

//////////////////////////////////////////////////
std::string ServicePublisher::GetReqTypeName() const
{
  return this->reqTypeName;
}

//////////////////////////////////////////////////
std::string ServicePublisher::GetRepTypeName() const
{
  return this->repTypeName;
}

//////////////////////////////////////////////////
void ServicePublisher::SetReqTypeName(const std::string &_reqTypeName)
{
  this->reqTypeName = _reqTypeName;
}

//////////////////////////////////////////////////
void ServicePublisher::SetRepTypeName(const std::string &_repTypeName)
{
  this->repTypeName = _repTypeName;
}