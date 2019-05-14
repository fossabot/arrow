// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Tools for dictionaries in IPC context

#ifndef ARROW_IPC_DICTIONARY_H
#define ARROW_IPC_DICTIONARY_H

#include <cstdint>
#include <memory>
#include <unordered_map>

#include "arrow/status.h"
#include "arrow/util/macros.h"
#include "arrow/util/visibility.h"

namespace arrow {

class Array;
class Field;
class RecordBatch;

namespace ipc {
namespace internal {

using DictionaryMap = std::unordered_map<int64_t, std::shared_ptr<Array>>;
using DictionaryFieldMap = std::unordered_map<int64_t, std::shared_ptr<Field>>;

/// \brief Memoization data structure for assigning id numbers to
/// dictionaries and tracking their current state through possible
/// deltas in an IPC stream
class ARROW_EXPORT DictionaryMemo {
 public:
  DictionaryMemo();
  DictionaryMemo(DictionaryMemo&&) = default;
  DictionaryMemo& operator=(DictionaryMemo&&) = default;

  /// \brief Return field corresponding to a particular dictionary
  /// id. Returns KeyError if id not found
  Status GetField(int64_t id, std::shared_ptr<Field>* field) const;

  /// \brief Return current dictionary corresponding to a particular
  /// id. Returns KeyError if id not found
  Status GetDictionary(int64_t id, std::shared_ptr<Array>* dictionary) const;

  /// \brief Return id for dictionary, computing new id if necessary
  int64_t GetOrAssignId(const std::shared_ptr<Field>& field);

  /// \brief Return id for dictionary if it exists, otherwise return
  /// KeyError
  Status GetId(const Field& type, int64_t* id) const;

  /// \brief Return true if dictionary for type is in this memo
  bool HasDictionary(const std::shared_ptr<Field>& type) const;

  /// \brief Return true if we have a dictionary for the input id
  bool HasDictionaryId(int64_t id) const;

  /// \brief Add field to the memo, return KeyError if already present
  Status AddField(int64_t id, const std::shared_ptr<Field>& field);

  /// \brief Add a dictionary to the memo with a particular id. Returns
  /// KeyError if that dictionary already exists
  Status AddDictionary(int64_t id, const std::shared_ptr<Array>& dictionary);

  const DictionaryMap& id_to_dictionary() const { return id_to_dictionary_; }

  /// \brief The number of dictionaries stored in the memo
  int size() const { return static_cast<int>(id_to_dictionary_.size()); }

 private:
  // Dictionary memory addresses, to track whether a particular
  // dictionary-encoded field has been seen before
  std::unordered_map<intptr_t, int64_t> field_to_id_;

  // Map of dictionary id to dictionary array
  DictionaryMap id_to_dictionary_;
  DictionaryFieldMap id_to_field_;

  ARROW_DISALLOW_COPY_AND_ASSIGN(DictionaryMemo);
};

ARROW_EXPORT
Status CollectDictionaries(const RecordBatch& batch, DictionaryMemo* memo);

}  // namespace internal
}  // namespace ipc
}  // namespace arrow

#endif  // ARROW_IPC_DICTIONARY_H
