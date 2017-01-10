////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#include "IncomingCache.h"
#include "Utils.h"

#include "Basics/MutexLocker.h"
#include "Basics/StaticStrings.h"
#include "Basics/VelocyPackHelper.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

//#include <libcuckoo/city_hasher.hh>
//#include <libcuckoo/cuckoohash_map.hh>

using namespace arangodb;
using namespace arangodb::pregel;

template <typename M>
void InCache<M>::parseMessages(VPackSlice incomingMessages) {
  prgl_shard_t shard = 0;
  std::string key;
  VPackValueLength i = 0;
  for (VPackSlice current : VPackArrayIterator(incomingMessages)) {
    if (i % 3 == 0) {
      shard = (prgl_shard_t)current.getUInt();
    } else if (i % 3 == 1) {  // TODO support multiple recipients
      key = current.copyString();
    } else {
      MUTEX_LOCKER(guard, this->_writeLock);
      if (current.isArray()) {
        for (VPackSlice val : VPackArrayIterator(current)) {
          M newValue;
          _format->unwrapValue(val, newValue);
          _set(shard, key, newValue);
        }
      } else {
        M newValue;
        _format->unwrapValue(current, newValue);
        _set(shard, key, newValue);
      }
    }
    i++;
  }

  if (i % 3 != 0) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_BAD_PARAMETER,
        "There must always be a multiple of 3 entries in messages");
  }
}

template <typename M>
void InCache<M>::setDirect(prgl_shard_t shard, std::string const& vertexId,
                           M const& data) {
  MUTEX_LOCKER(guard, this->_writeLock);
  this->_set(shard, vertexId, data);
}

// ================== ArrayIncomingCache ==================

template <typename M>
void ArrayInCache<M>::_set(prgl_shard_t shard, std::string const& key,
                           M const& newValue) {
  this->_receivedMessageCount++;
  HMap& vertexMap = _shardMap[shard];
  vertexMap[key].push_back(newValue);
}

template <typename M>
void ArrayInCache<M>::mergeCache(InCache<M> const* otherCache) {
  MUTEX_LOCKER(guard, this->_writeLock);

  ArrayInCache<M>* other = (ArrayInCache<M>*)otherCache;
  this->_receivedMessageCount += other->_receivedMessageCount;

  // cannot call setDirect since it locks
  for (auto const& pair : other->_shardMap) {
    HMap& vertexMap = _shardMap[pair.first];
    for (auto& vertexMessage : pair.second) {
      std::vector<M>& a = vertexMap[vertexMessage.first];
      std::vector<M> const& b = vertexMessage.second;
      a.insert(a.end(), b.begin(), b.end());
    }
  }
}

template <typename M>
MessageIterator<M> ArrayInCache<M>::getMessages(prgl_shard_t shard,
                                                std::string const& key) {
  HMap const& vertexMap = _shardMap[shard];
  auto vmsg = vertexMap.find(key);
  if (vmsg != vertexMap.end()) {
    LOG(INFO) << "Got a message for " << key;

    M const* ptr = vmsg->second.data();
    return MessageIterator<M>(ptr, vmsg->second.size());
  } else {
    LOG(INFO) << "No message for " << key;
    return MessageIterator<M>();
  }
}

template <typename M>
void ArrayInCache<M>::clear() {
  MUTEX_LOCKER(guard, this->_writeLock);
  this->_receivedMessageCount = 0;
  _shardMap.clear();
}

template <typename M>
void ArrayInCache<M>::erase(prgl_shard_t shard, std::string const& key) {
  MUTEX_LOCKER(guard, this->_writeLock);
  HMap& vertexMap(_shardMap[shard]);
  vertexMap.erase(key);
}

template <typename M>
void ArrayInCache<M>::forEach(
    std::function<void(prgl_shard_t, std::string const&, M const&)> func) {
  for (auto const& pair : _shardMap) {
    prgl_shard_t shard = pair.first;
    HMap const& vertexMap = pair.second;
    for (auto& vertexMsgs : vertexMap) {
      for (M const& val : vertexMsgs.second) {
        func(shard, vertexMsgs.first, val);
      }
    }
  }
}

// ================== CombiningIncomingCache ==================

template <typename M>
void CombiningInCache<M>::_set(prgl_shard_t shard, std::string const& key,
                               M const& newValue) {
  /*cuckoohash_map<int, std::string, CityHasher<int>> Table;
  for (int i = 0; i < 100; i++) {
    Table[i] = "hello"+std::to_string(i);
  }
  for (int i = 0; i < 101; i++) {
    std::string out;
    if (Table.find(i, out)) {
      LOG(INFO) << i << "  " << out;
    } else {
      LOG(INFO) << i << "  NOT FOUND";
    }
  }*/

  this->_receivedMessageCount++;
  HMap& vertexMap = _shardMap[shard];
  auto vmsg = vertexMap.find(key);
  if (vmsg != vertexMap.end()) {  // got a message for the same vertex
    _combiner->combine(vmsg->second, newValue);
  } else {
    vertexMap.insert(std::make_pair(key, newValue));
  }
}

template <typename M>
void CombiningInCache<M>::mergeCache(InCache<M> const* otherCache) {
  MUTEX_LOCKER(guard, this->_writeLock);

  CombiningInCache<M>* other = (CombiningInCache<M>*)otherCache;
  this->_receivedMessageCount += other->_receivedMessageCount;

  // cannot call setDirect since it locks
  for (auto const& pair : other->_shardMap) {
    HMap& vertexMap = _shardMap[pair.first];

    for (auto& vertexMessage : pair.second) {
      auto vmsg = vertexMap.find(vertexMessage.first);
      if (vmsg != vertexMap.end()) {  // got a message for the same vertex
        _combiner->combine(vmsg->second, vertexMessage.second);
      } else {
        vertexMap.insert(vertexMessage);
      }
    }
  }
}

template <typename M>
MessageIterator<M> CombiningInCache<M>::getMessages(prgl_shard_t shard,
                                                    std::string const& key) {
  HMap const& vertexMap(_shardMap[shard]);
  auto vmsg = vertexMap.find(key);
  if (vmsg != vertexMap.end()) {
    return MessageIterator<M>(&vmsg->second);
  } else {
    return MessageIterator<M>();
  }
}

template <typename M>
void CombiningInCache<M>::clear() {
  MUTEX_LOCKER(guard, this->_writeLock);
  this->_receivedMessageCount = 0;
  _shardMap.clear();
}

template <typename M>
void CombiningInCache<M>::erase(prgl_shard_t shard, std::string const& key) {
  MUTEX_LOCKER(guard, this->_writeLock);
  HMap& vertexMap(_shardMap[shard]);
  vertexMap.erase(key);
}

template <typename M>
void CombiningInCache<M>::forEach(
    std::function<void(prgl_shard_t shard, std::string const& key, M const&)>
        func) {
  for (auto const& pair : _shardMap) {
    prgl_shard_t shard = pair.first;
    HMap const& vertexMap = pair.second;
    for (auto& vertexMessage : vertexMap) {
      func(shard, vertexMessage.first, vertexMessage.second);
    }
  }
}

// template types to create
template class arangodb::pregel::InCache<int64_t>;
template class arangodb::pregel::InCache<float>;
template class arangodb::pregel::ArrayInCache<int64_t>;
template class arangodb::pregel::ArrayInCache<float>;
template class arangodb::pregel::CombiningInCache<int64_t>;
template class arangodb::pregel::CombiningInCache<float>;