// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/disk_cache/blockfile/addr.h"

#include "base/check.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace disk_cache {

int Addr::start_block() const {
  DCHECK(is_block_file());
  return value_ & kStartBlockMask;
}

int Addr::num_blocks() const {
  DCHECK(is_block_file() || !value_);
  return ((value_ & kNumBlocksMask) >> kNumBlocksOffset) + 1;
}

bool Addr::SetFileNumber(int file_number) {
  DCHECK(is_separate_file());
  if (file_number & ~kFileNameMask)
    return false;
  value_ = kInitializedMask | file_number;
  return true;
}

bool Addr::SanityCheck() const {
  if (!is_initialized())
    return !value_;

  if (file_type() > BLOCK_4K)
    return false;

  if (is_separate_file())
    return true;

  return !reserved_bits();
}

bool Addr::SanityCheckForEntry() const {
  if (!SanityCheck() || !is_initialized())
    return false;

  if (is_separate_file() || file_type() != BLOCK_256)
    return false;

  return true;
}

bool Addr::SanityCheckForRankings() const {
  if (!SanityCheck() || !is_initialized())
    return false;

  if (is_separate_file() || file_type() != RANKINGS || num_blocks() != 1)
    return false;

  return true;
}

std::string Addr::ToString() const {
  if (value_ == 0) {
    return "null";
  }
  auto address = *this;
  std::ostringstream ss;
  ss << "  value: 0x" << std::hex << address.value() << std::dec
     << ", FileNumber: " << address.FileNumber()
     << ", file_type: " << address.file_type()
     << ", BlockSize: " << address.BlockSize()
     << ", start_block: " << address.start_block()
     << ", num_blocks: " << address.num_blocks();
  return ss.str();
}

}  // namespace disk_cache
