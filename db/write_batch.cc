//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring
//    kTypeDeletion varstring
//    kTypeSingleDeletion varstring
//    kTypeMerge varstring varstring
//    kTypeColumnFamilyValue varint32 varstring varstring
//    kTypeColumnFamilyDeletion varint32 varstring varstring
//    kTypeColumnFamilySingleDeletion varint32 varstring varstring
//    kTypeColumnFamilyMerge varint32 varstring varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

#include "rocksdb/write_batch.h"

#include <stack>
#include <stdexcept>
#include <vector>

#include "db/column_family.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/flush_scheduler.h"
#include "db/memtable.h"
#include "db/snapshot_impl.h"
#include "db/write_batch_internal.h"
#include "rocksdb/merge_operator.h"
#include "util/coding.h"
#include "util/perf_context_imp.h"
#include "util/statistics.h"

namespace rocksdb {

// anon namespace for file-local types
namespace {

enum ContentFlags : uint32_t {
  DEFERRED = 1,
  HAS_PUT = 2,
  HAS_DELETE = 4,
  HAS_SINGLE_DELETE = 8,
  HAS_MERGE = 16,
};

struct BatchContentClassifier : public WriteBatch::Handler {
  uint32_t content_flags = 0;

  Status PutCF(uint32_t, const Slice&, const Slice&) override {
    content_flags |= ContentFlags::HAS_PUT;
    return Status::OK();
  }

  Status DeleteCF(uint32_t, const Slice&) override {
    content_flags |= ContentFlags::HAS_DELETE;
    return Status::OK();
  }

  Status SingleDeleteCF(uint32_t, const Slice&) override {
    content_flags |= ContentFlags::HAS_SINGLE_DELETE;
    return Status::OK();
  }

  Status MergeCF(uint32_t, const Slice&, const Slice&) override {
    content_flags |= ContentFlags::HAS_MERGE;
    return Status::OK();
  }
};

}  // anon namespace

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
static const size_t kHeader = 12;

struct SavePoint {
  size_t size;  // size of rep_
  int count;    // count of elements in rep_
  uint32_t content_flags;
};

struct SavePoints {
  std::stack<SavePoint> stack;
};

WriteBatch::WriteBatch(size_t reserved_bytes)
    : save_points_(nullptr), content_flags_(0), rep_() {
  rep_.reserve((reserved_bytes > kHeader) ? reserved_bytes : kHeader);
  rep_.resize(kHeader);
}

WriteBatch::WriteBatch(const std::string& rep)
    : save_points_(nullptr),
      content_flags_(ContentFlags::DEFERRED),
      rep_(rep) {}

WriteBatch::WriteBatch(const WriteBatch& src)
    : save_points_(src.save_points_),
      content_flags_(src.content_flags_.load(std::memory_order_relaxed)),
      rep_(src.rep_) {}

WriteBatch::WriteBatch(WriteBatch&& src)
    : save_points_(std::move(src.save_points_)),
      content_flags_(src.content_flags_.load(std::memory_order_relaxed)),
      rep_(std::move(src.rep_)) {}

WriteBatch& WriteBatch::operator=(const WriteBatch& src) {
  if (&src != this) {
    this->~WriteBatch();
    new (this) WriteBatch(src);
  }
  return *this;
}

WriteBatch& WriteBatch::operator=(WriteBatch&& src) {
  if (&src != this) {
    this->~WriteBatch();
    new (this) WriteBatch(std::move(src));
  }
  return *this;
}

WriteBatch::~WriteBatch() { delete save_points_; }

WriteBatch::Handler::~Handler() { }

void WriteBatch::Handler::LogData(const Slice& blob) {
  // If the user has not specified something to do with blobs, then we ignore
  // them.
}

bool WriteBatch::Handler::Continue() {
  return true;
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);

  content_flags_.store(0, std::memory_order_relaxed);

  if (save_points_ != nullptr) {
    while (!save_points_->stack.empty()) {
      save_points_->stack.pop();
    }
  }
}

int WriteBatch::Count() const {
  return WriteBatchInternal::Count(this);
}

uint32_t WriteBatch::ComputeContentFlags() const {
  auto rv = content_flags_.load(std::memory_order_relaxed);
  if ((rv & ContentFlags::DEFERRED) != 0) {
    BatchContentClassifier classifier;
    Iterate(&classifier);
    rv = classifier.content_flags;

    // this method is conceptually const, because it is performing a lazy
    // computation that doesn't affect the abstract state of the batch.
    // content_flags_ is marked mutable so that we can perform the
    // following assignment
    content_flags_.store(rv, std::memory_order_relaxed);
  }
  return rv;
}

bool WriteBatch::HasPut() const {
  return (ComputeContentFlags() & ContentFlags::HAS_PUT) != 0;
}

bool WriteBatch::HasDelete() const {
  return (ComputeContentFlags() & ContentFlags::HAS_DELETE) != 0;
}

bool WriteBatch::HasSingleDelete() const {
  return (ComputeContentFlags() & ContentFlags::HAS_SINGLE_DELETE) != 0;
}

bool WriteBatch::HasMerge() const {
  return (ComputeContentFlags() & ContentFlags::HAS_MERGE) != 0;
}

Status ReadRecordFromWriteBatch(Slice* input, char* tag,
                                uint32_t* column_family, Slice* key,
                                Slice* value, Slice* blob) {
  assert(key != nullptr && value != nullptr);
  *tag = (*input)[0];
  input->remove_prefix(1);
  *column_family = 0;  // default
  switch (*tag) {
    case kTypeColumnFamilyValue:
      if (!GetVarint32(input, column_family)) {
        return Status::Corruption("bad WriteBatch Put");
      }
    // intentional fallthrough
    case kTypeValue:
      if (!GetLengthPrefixedSlice(input, key) ||
          !GetLengthPrefixedSlice(input, value)) {
        return Status::Corruption("bad WriteBatch Put");
      }
      break;
    case kTypeColumnFamilyDeletion:
    case kTypeColumnFamilySingleDeletion:
      if (!GetVarint32(input, column_family)) {
        return Status::Corruption("bad WriteBatch Delete");
      }
    // intentional fallthrough
    case kTypeDeletion:
    case kTypeSingleDeletion:
      if (!GetLengthPrefixedSlice(input, key)) {
        return Status::Corruption("bad WriteBatch Delete");
      }
      break;
    case kTypeColumnFamilyMerge:
      if (!GetVarint32(input, column_family)) {
        return Status::Corruption("bad WriteBatch Merge");
      }
    // intentional fallthrough
    case kTypeMerge:
      if (!GetLengthPrefixedSlice(input, key) ||
          !GetLengthPrefixedSlice(input, value)) {
        return Status::Corruption("bad WriteBatch Merge");
      }
      break;
    case kTypeLogData:
      assert(blob != nullptr);
      if (!GetLengthPrefixedSlice(input, blob)) {
        return Status::Corruption("bad WriteBatch Blob");
      }
      break;
    default:
      return Status::Corruption("unknown WriteBatch tag");
  }
  return Status::OK();
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }

  input.remove_prefix(kHeader);
  Slice key, value, blob;
  int found = 0;
  Status s;
  while (s.ok() && !input.empty() && handler->Continue()) {
    char tag = 0;
    uint32_t column_family = 0;  // default

    s = ReadRecordFromWriteBatch(&input, &tag, &column_family, &key, &value,
                                 &blob);
    if (!s.ok()) {
      return s;
    }

    switch (tag) {
      case kTypeColumnFamilyValue:
      case kTypeValue:
        assert(content_flags_.load(std::memory_order_relaxed) &
               (ContentFlags::DEFERRED | ContentFlags::HAS_PUT));
        s = handler->PutCF(column_family, key, value);
        found++;
        break;
      case kTypeColumnFamilyDeletion:
      case kTypeDeletion:
        assert(content_flags_.load(std::memory_order_relaxed) &
               (ContentFlags::DEFERRED | ContentFlags::HAS_DELETE));
        s = handler->DeleteCF(column_family, key);
        found++;
        break;
      case kTypeColumnFamilySingleDeletion:
      case kTypeSingleDeletion:
        assert(content_flags_.load(std::memory_order_relaxed) &
               (ContentFlags::DEFERRED | ContentFlags::HAS_SINGLE_DELETE));
        s = handler->SingleDeleteCF(column_family, key);
        found++;
        break;
      case kTypeColumnFamilyMerge:
      case kTypeMerge:
        assert(content_flags_.load(std::memory_order_relaxed) &
               (ContentFlags::DEFERRED | ContentFlags::HAS_MERGE));
        s = handler->MergeCF(column_family, key, value);
        found++;
        break;
      case kTypeLogData:
        handler->LogData(blob);
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (!s.ok()) {
    return s;
  }
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

size_t WriteBatchInternal::GetFirstOffset(WriteBatch* b) { return kHeader; }

void WriteBatchInternal::Put(WriteBatch* b, uint32_t column_family_id,
                             const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  PutLengthPrefixedSlice(&b->rep_, value);
  b->content_flags_.store(
      b->content_flags_.load(std::memory_order_relaxed) | ContentFlags::HAS_PUT,
      std::memory_order_relaxed);
}

void WriteBatch::Put(ColumnFamilyHandle* column_family, const Slice& key,
                     const Slice& value) {
  WriteBatchInternal::Put(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatchInternal::Put(WriteBatch* b, uint32_t column_family_id,
                             const SliceParts& key, const SliceParts& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeValue));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyValue));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&b->rep_, key);
  PutLengthPrefixedSliceParts(&b->rep_, value);
  b->content_flags_.store(
      b->content_flags_.load(std::memory_order_relaxed) | ContentFlags::HAS_PUT,
      std::memory_order_relaxed);
}

void WriteBatch::Put(ColumnFamilyHandle* column_family, const SliceParts& key,
                     const SliceParts& value) {
  WriteBatchInternal::Put(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatchInternal::Delete(WriteBatch* b, uint32_t column_family_id,
                                const Slice& key) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeDeletion));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyDeletion));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_DELETE,
                          std::memory_order_relaxed);
}

void WriteBatch::Delete(ColumnFamilyHandle* column_family, const Slice& key) {
  WriteBatchInternal::Delete(this, GetColumnFamilyID(column_family), key);
}

void WriteBatchInternal::Delete(WriteBatch* b, uint32_t column_family_id,
                                const SliceParts& key) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeDeletion));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyDeletion));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&b->rep_, key);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_DELETE,
                          std::memory_order_relaxed);
}

void WriteBatch::Delete(ColumnFamilyHandle* column_family,
                        const SliceParts& key) {
  WriteBatchInternal::Delete(this, GetColumnFamilyID(column_family), key);
}

void WriteBatchInternal::SingleDelete(WriteBatch* b, uint32_t column_family_id,
                                      const Slice& key) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeSingleDeletion));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilySingleDeletion));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_SINGLE_DELETE,
                          std::memory_order_relaxed);
}

void WriteBatch::SingleDelete(ColumnFamilyHandle* column_family,
                              const Slice& key) {
  WriteBatchInternal::SingleDelete(this, GetColumnFamilyID(column_family), key);
}

void WriteBatchInternal::SingleDelete(WriteBatch* b, uint32_t column_family_id,
                                      const SliceParts& key) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeSingleDeletion));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilySingleDeletion));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&b->rep_, key);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_SINGLE_DELETE,
                          std::memory_order_relaxed);
}

void WriteBatch::SingleDelete(ColumnFamilyHandle* column_family,
                              const SliceParts& key) {
  WriteBatchInternal::SingleDelete(this, GetColumnFamilyID(column_family), key);
}

void WriteBatchInternal::Merge(WriteBatch* b, uint32_t column_family_id,
                               const Slice& key, const Slice& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeMerge));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyMerge));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSlice(&b->rep_, key);
  PutLengthPrefixedSlice(&b->rep_, value);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_MERGE,
                          std::memory_order_relaxed);
}

void WriteBatch::Merge(ColumnFamilyHandle* column_family, const Slice& key,
                       const Slice& value) {
  WriteBatchInternal::Merge(this, GetColumnFamilyID(column_family), key, value);
}

void WriteBatchInternal::Merge(WriteBatch* b, uint32_t column_family_id,
                               const SliceParts& key,
                               const SliceParts& value) {
  WriteBatchInternal::SetCount(b, WriteBatchInternal::Count(b) + 1);
  if (column_family_id == 0) {
    b->rep_.push_back(static_cast<char>(kTypeMerge));
  } else {
    b->rep_.push_back(static_cast<char>(kTypeColumnFamilyMerge));
    PutVarint32(&b->rep_, column_family_id);
  }
  PutLengthPrefixedSliceParts(&b->rep_, key);
  PutLengthPrefixedSliceParts(&b->rep_, value);
  b->content_flags_.store(b->content_flags_.load(std::memory_order_relaxed) |
                              ContentFlags::HAS_MERGE,
                          std::memory_order_relaxed);
}

void WriteBatch::Merge(ColumnFamilyHandle* column_family,
                       const SliceParts& key,
                       const SliceParts& value) {
  WriteBatchInternal::Merge(this, GetColumnFamilyID(column_family),
                            key, value);
}

void WriteBatch::PutLogData(const Slice& blob) {
  rep_.push_back(static_cast<char>(kTypeLogData));
  PutLengthPrefixedSlice(&rep_, blob);
}

void WriteBatch::SetSavePoint() {
  if (save_points_ == nullptr) {
    save_points_ = new SavePoints();
  }
  // Record length and count of current batch of writes.
  save_points_->stack.push(SavePoint{
      GetDataSize(), Count(), content_flags_.load(std::memory_order_relaxed)});
}

Status WriteBatch::RollbackToSavePoint() {
  if (save_points_ == nullptr || save_points_->stack.size() == 0) {
    return Status::NotFound();
  }

  // Pop the most recent savepoint off the stack
  SavePoint savepoint = save_points_->stack.top();
  save_points_->stack.pop();

  assert(savepoint.size <= rep_.size());
  assert(savepoint.count <= Count());

  if (savepoint.size == rep_.size()) {
    // No changes to rollback
  } else if (savepoint.size == 0) {
    // Rollback everything
    Clear();
  } else {
    rep_.resize(savepoint.size);
    WriteBatchInternal::SetCount(this, savepoint.count);
    content_flags_.store(savepoint.content_flags, std::memory_order_relaxed);
  }

  return Status::OK();
}

namespace {
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  ColumnFamilyMemTables* const cf_mems_;
  FlushScheduler* const flush_scheduler_;
  const bool ignore_missing_column_families_;
  const uint64_t log_number_;
  DBImpl* db_;
  const bool dont_filter_deletes_;
  const bool concurrent_memtable_writes_;

  // cf_mems should not be shared with concurrent inserters
  MemTableInserter(SequenceNumber sequence, ColumnFamilyMemTables* cf_mems,
                   FlushScheduler* flush_scheduler,
                   bool ignore_missing_column_families, uint64_t log_number,
                   DB* db, const bool dont_filter_deletes,
                   bool concurrent_memtable_writes)
      : sequence_(sequence),
        cf_mems_(cf_mems),
        flush_scheduler_(flush_scheduler),
        ignore_missing_column_families_(ignore_missing_column_families),
        log_number_(log_number),
        db_(reinterpret_cast<DBImpl*>(db)),
        dont_filter_deletes_(dont_filter_deletes),
        concurrent_memtable_writes_(concurrent_memtable_writes) {
    assert(cf_mems_);
    if (!dont_filter_deletes_) {
      assert(db_);
    }
  }

  bool SeekToColumnFamily(uint32_t column_family_id, Status* s) {
    // If we are in a concurrent mode, it is the caller's responsibility
    // to clone the original ColumnFamilyMemTables so that each thread
    // has its own instance.  Otherwise, it must be guaranteed that there
    // is no concurrent access
    bool found = cf_mems_->Seek(column_family_id);
    if (!found) {
      if (ignore_missing_column_families_) {
        *s = Status::OK();
      } else {
        *s = Status::InvalidArgument(
            "Invalid column family specified in write batch");
      }
      return false;
    }
    if (log_number_ != 0 && log_number_ < cf_mems_->GetLogNumber()) {
      // This is true only in recovery environment (log_number_ is always 0 in
      // non-recovery, regular write code-path)
      // * If log_number_ < cf_mems_->GetLogNumber(), this means that column
      // family already contains updates from this log. We can't apply updates
      // twice because of update-in-place or merge workloads -- ignore the
      // update
      *s = Status::OK();
      return false;
    }
    return true;
  }

  virtual Status PutCF(uint32_t column_family_id, const Slice& key,
                       const Slice& value) override {
    Status seek_status;
    if (!SeekToColumnFamily(column_family_id, &seek_status)) {
      ++sequence_;
      return seek_status;
    }
    MemTable* mem = cf_mems_->GetMemTable();
    auto* moptions = mem->GetMemTableOptions();
    if (!moptions->inplace_update_support) {
      mem->Add(sequence_, kTypeValue, key, value, concurrent_memtable_writes_);
    } else if (moptions->inplace_callback == nullptr) {
      assert(!concurrent_memtable_writes_);
      mem->Update(sequence_, key, value);
      RecordTick(moptions->statistics, NUMBER_KEYS_UPDATED);
    } else {
      assert(!concurrent_memtable_writes_);
      if (mem->UpdateCallback(sequence_, key, value)) {
      } else {
        // key not found in memtable. Do sst get, update, add
        SnapshotImpl read_from_snapshot;
        read_from_snapshot.number_ = sequence_;
        ReadOptions ropts;
        ropts.snapshot = &read_from_snapshot;

        std::string prev_value;
        std::string merged_value;

        auto cf_handle = cf_mems_->GetColumnFamilyHandle();
        if (cf_handle == nullptr) {
          cf_handle = db_->DefaultColumnFamily();
        }
        Status s = db_->Get(ropts, cf_handle, key, &prev_value);

        char* prev_buffer = const_cast<char*>(prev_value.c_str());
        uint32_t prev_size = static_cast<uint32_t>(prev_value.size());
        auto status = moptions->inplace_callback(s.ok() ? prev_buffer : nullptr,
                                                 s.ok() ? &prev_size : nullptr,
                                                 value, &merged_value);
        if (status == UpdateStatus::UPDATED_INPLACE) {
          // prev_value is updated in-place with final value.
          mem->Add(sequence_, kTypeValue, key, Slice(prev_buffer, prev_size));
          RecordTick(moptions->statistics, NUMBER_KEYS_WRITTEN);
        } else if (status == UpdateStatus::UPDATED) {
          // merged_value contains the final value.
          mem->Add(sequence_, kTypeValue, key, Slice(merged_value));
          RecordTick(moptions->statistics, NUMBER_KEYS_WRITTEN);
        }
      }
    }
    // Since all Puts are logged in trasaction logs (if enabled), always bump
    // sequence number. Even if the update eventually fails and does not result
    // in memtable add/update.
    sequence_++;
    CheckMemtableFull();
    return Status::OK();
  }

  Status DeleteImpl(uint32_t column_family_id, const Slice& key,
                    ValueType delete_type) {
    Status seek_status;
    if (!SeekToColumnFamily(column_family_id, &seek_status)) {
      ++sequence_;
      return seek_status;
    }
    MemTable* mem = cf_mems_->GetMemTable();
    auto* moptions = mem->GetMemTableOptions();
    if (!dont_filter_deletes_ && moptions->filter_deletes) {
      assert(!concurrent_memtable_writes_);
      SnapshotImpl read_from_snapshot;
      read_from_snapshot.number_ = sequence_;
      ReadOptions ropts;
      ropts.snapshot = &read_from_snapshot;
      std::string value;
      auto cf_handle = cf_mems_->GetColumnFamilyHandle();
      if (cf_handle == nullptr) {
        cf_handle = db_->DefaultColumnFamily();
      }
      if (!db_->KeyMayExist(ropts, cf_handle, key, &value)) {
        RecordTick(moptions->statistics, NUMBER_FILTERED_DELETES);
        return Status::OK();
      }
    }
    mem->Add(sequence_, delete_type, key, Slice(), concurrent_memtable_writes_);
    sequence_++;
    CheckMemtableFull();
    return Status::OK();
  }

  virtual Status DeleteCF(uint32_t column_family_id,
                          const Slice& key) override {
    return DeleteImpl(column_family_id, key, kTypeDeletion);
  }

  virtual Status SingleDeleteCF(uint32_t column_family_id,
                                const Slice& key) override {
    return DeleteImpl(column_family_id, key, kTypeSingleDeletion);
  }

  virtual Status MergeCF(uint32_t column_family_id, const Slice& key,
                         const Slice& value) override {
    assert(!concurrent_memtable_writes_);
    Status seek_status;
    if (!SeekToColumnFamily(column_family_id, &seek_status)) {
      ++sequence_;
      return seek_status;
    }
    MemTable* mem = cf_mems_->GetMemTable();
    auto* moptions = mem->GetMemTableOptions();
    bool perform_merge = false;

    if (moptions->max_successive_merges > 0 && db_ != nullptr) {
      LookupKey lkey(key, sequence_);

      // Count the number of successive merges at the head
      // of the key in the memtable
      size_t num_merges = mem->CountSuccessiveMergeEntries(lkey);

      if (num_merges >= moptions->max_successive_merges) {
        perform_merge = true;
      }
    }

    if (perform_merge) {
      // 1) Get the existing value
      std::string get_value;

      // Pass in the sequence number so that we also include previous merge
      // operations in the same batch.
      SnapshotImpl read_from_snapshot;
      read_from_snapshot.number_ = sequence_;
      ReadOptions read_options;
      read_options.snapshot = &read_from_snapshot;

      auto cf_handle = cf_mems_->GetColumnFamilyHandle();
      if (cf_handle == nullptr) {
        cf_handle = db_->DefaultColumnFamily();
      }
      db_->Get(read_options, cf_handle, key, &get_value);
      Slice get_value_slice = Slice(get_value);

      // 2) Apply this merge
      auto merge_operator = moptions->merge_operator;
      assert(merge_operator);

      std::deque<std::string> operands;
      operands.push_front(value.ToString());
      std::string new_value;
      bool merge_success = false;
      {
        StopWatchNano timer(Env::Default(), moptions->statistics != nullptr);
        PERF_TIMER_GUARD(merge_operator_time_nanos);
        merge_success = merge_operator->FullMerge(
            key, &get_value_slice, operands, &new_value, moptions->info_log);
        RecordTick(moptions->statistics, MERGE_OPERATION_TOTAL_TIME,
                   timer.ElapsedNanos());
      }

      if (!merge_success) {
          // Failed to merge!
        RecordTick(moptions->statistics, NUMBER_MERGE_FAILURES);

        // Store the delta in memtable
        perform_merge = false;
      } else {
        // 3) Add value to memtable
        mem->Add(sequence_, kTypeValue, key, new_value);
      }
    }

    if (!perform_merge) {
      // Add merge operator to memtable
      mem->Add(sequence_, kTypeMerge, key, value);
    }

    sequence_++;
    CheckMemtableFull();
    return Status::OK();
  }

  void CheckMemtableFull() {
    if (flush_scheduler_ != nullptr) {
      auto* cfd = cf_mems_->current();
      assert(cfd != nullptr);
      if (cfd->mem()->ShouldScheduleFlush() &&
          cfd->mem()->MarkFlushScheduled()) {
        // MarkFlushScheduled only returns true if we are the one that
        // should take action, so no need to dedup further
        flush_scheduler_->ScheduleFlush(cfd);
      }
    }
  }
};
}  // namespace

// This function can only be called in these conditions:
// 1) During Recovery()
// 2) During Write(), in a single-threaded write thread
// 3) During Write(), in a concurrent context where memtables has been cloned
// The reason is that it calls memtables->Seek(), which has a stateful cache
Status WriteBatchInternal::InsertInto(
    const autovector<WriteThread::Writer*>& writers, SequenceNumber sequence,
    ColumnFamilyMemTables* memtables, FlushScheduler* flush_scheduler,
    bool ignore_missing_column_families, uint64_t log_number, DB* db,
    const bool dont_filter_deletes, bool concurrent_memtable_writes) {
  MemTableInserter inserter(sequence, memtables, flush_scheduler,
                            ignore_missing_column_families, log_number, db,
                            dont_filter_deletes, concurrent_memtable_writes);

  for (size_t i = 0; i < writers.size(); i++) {
    if (!writers[i]->CallbackFailed()) {
      writers[i]->status = writers[i]->batch->Iterate(&inserter);
      if (!writers[i]->status.ok()) {
        return writers[i]->status;
      }
    }
  }
  return Status::OK();
}

Status WriteBatchInternal::InsertInto(const WriteBatch* batch,
                                      ColumnFamilyMemTables* memtables,
                                      FlushScheduler* flush_scheduler,
                                      bool ignore_missing_column_families,
                                      uint64_t log_number, DB* db,
                                      const bool dont_filter_deletes,
                                      bool concurrent_memtable_writes) {
  MemTableInserter inserter(WriteBatchInternal::Sequence(batch), memtables,
                            flush_scheduler, ignore_missing_column_families,
                            log_number, db, dont_filter_deletes,
                            concurrent_memtable_writes);
  return batch->Iterate(&inserter);
}

void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
  b->content_flags_.store(ContentFlags::DEFERRED, std::memory_order_relaxed);
}

void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
  dst->content_flags_.store(
      dst->content_flags_.load(std::memory_order_relaxed) |
          src->content_flags_.load(std::memory_order_relaxed),
      std::memory_order_relaxed);
}

size_t WriteBatchInternal::AppendedByteSize(size_t leftByteSize,
                                            size_t rightByteSize) {
  if (leftByteSize == 0 || rightByteSize == 0) {
    return leftByteSize + rightByteSize;
  } else {
    return leftByteSize + rightByteSize - kHeader;
  }
}

}  // namespace rocksdb
