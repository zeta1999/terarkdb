// project headers
#include "terark_zip_table_builder.h"
// std headers
#include <future>
#include <cfloat>
// boost headers
#include <boost/scope_exit.hpp>
// rocksdb headers
#include <rocksdb/merge_operator.h>
#include <table/meta_blocks.h>
// terark headers
#include <terark/util/sortable_strvec.hpp>
#if defined(TerocksPrivateCode)
# include <terark/zbs/zero_length_blob_store.hpp>
# include <terark/zbs/plain_blob_store.hpp>
# include <terark/zbs/mixed_len_blob_store.hpp>
# include <terark/zbs/zip_offset_blob_store.hpp>
#endif // TerocksPrivateCode


namespace rocksdb {

using terark::SortableStrVec;
using terark::byte_swap;
using terark::UintVecMin0;

std::mutex g_sumMutex;
size_t g_sumKeyLen = 0;
size_t g_sumValueLen = 0;
size_t g_sumUserKeyLen = 0;
size_t g_sumUserKeyNum = 0;
size_t g_sumEntryNum = 0;
long long g_lastTime = g_pf.now();


#if defined(DEBUG_TWO_PASS_ITER) && !defined(NDEBUG)

void DEBUG_PRINT_KEY(const char* first_or_second, rocksdb::Slice key) {
  rocksdb::ParsedInternalKey ikey;
  rocksdb::ParseInternalKey(key, &ikey);
  fprintf(stderr, "DEBUG: %s pass => %s\n", first_or_second, ikey.DebugString(true).c_str());
}

#define DEBUG_PRINT_1ST_PASS_KEY(key) DEBUG_PRINT_KEY("1st", key);
#define DEBUG_PRINT_2ND_PASS_KEY(key) DEBUG_PRINT_KEY("2nd", key);

#else

void DEBUG_PRINT_KEY(...) {}

#define DEBUG_PRINT_1ST_PASS_KEY(...) DEBUG_PRINT_KEY(__VA_ARGS__);
#define DEBUG_PRINT_2ND_PASS_KEY(...) DEBUG_PRINT_KEY(__VA_ARGS__);

#endif

template<class ByteArray>
static
Status WriteBlock(const ByteArray& blockData, WritableFileWriter* file,
  uint64_t* offset, BlockHandle* block_handle) {
  block_handle->set_offset(*offset);
  block_handle->set_size(blockData.size());
  Status s = file->Append(SliceOf(blockData));
  if (s.ok()) {
    *offset += blockData.size();
  }
  return s;
}

namespace {
struct PendingTask {
  const TerarkZipTableBuilder* tztb;
  long long startTime;
};
}

TerarkZipTableBuilder::TerarkZipTableBuilder(
  const TerarkZipTableOptions& tzto,
  const TableBuilderOptions& tbo,
  uint32_t column_family_id,
  WritableFileWriter* file,
  size_t key_prefixLen)
  : table_options_(tzto)
  , ioptions_(tbo.ioptions)
  , range_del_block_(1)
  , key_prefixLen_(key_prefixLen)
{
  properties_.fixed_key_len = 0;
  properties_.num_data_blocks = 1;
  properties_.column_family_id = column_family_id;
  properties_.column_family_name = tbo.column_family_name;
  properties_.comparator_name = ioptions_.user_comparator ?
    ioptions_.user_comparator->Name() : "nullptr";
  properties_.merge_operator_name = ioptions_.merge_operator ?
    ioptions_.merge_operator->Name() : "nullptr";
  properties_.compression_name = CompressionTypeToString(tbo.compression_type);
  properties_.prefix_extractor_name = ioptions_.prefix_extractor ?
    ioptions_.prefix_extractor->Name() : "nullptr";

  isReverseBytewiseOrder_ =
    fstring(properties_.comparator_name).startsWith("rev:");
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
  isUint64Comparator_ =
    fstring(properties_.comparator_name) == "rocksdb.Uint64Comparator";
#endif

  if (tbo.int_tbl_prop_collector_factories) {
    const auto& factories = *tbo.int_tbl_prop_collector_factories;
    collectors_.resize(factories.size());
    auto cfId = properties_.column_family_id;
    for (size_t i = 0; i < collectors_.size(); ++i) {
      collectors_[i].reset(factories[i]->CreateIntTblPropCollector(cfId));
    }
  }

  std::string property_collectors_names = "[";
  for (size_t i = 0;
    i < ioptions_.table_properties_collector_factories.size(); ++i) {
    if (i != 0) {
      property_collectors_names += ",";
    }
    property_collectors_names +=
      ioptions_.table_properties_collector_factories[i]->Name();
  }
  property_collectors_names += "]";
  properties_.property_collectors_names = property_collectors_names;

  file_ = file;
  sampleUpperBound_ = randomGenerator_.max() * table_options_.sampleRatio;
  tmpValueFile_.path = tzto.localTempDir + "/Terark-XXXXXX";
  tmpValueFile_.open_temp();
  tmpKeyFile_.path = tmpValueFile_.path + ".keydata";
  tmpKeyFile_.open();
  tmpSampleFile_.path = tmpValueFile_.path + ".sample";
  tmpSampleFile_.open();
  if (table_options_.debugLevel == 4) {
    tmpDumpFile_.open(tmpValueFile_.path + ".dump", "wb+");
  }

  if (tzto.isOfflineBuild) {
    if (tbo.compression_dict && tbo.compression_dict->size()) {
      auto data = (byte_t*)tbo.compression_dict->data();
      auto size = tbo.compression_dict->size();
      tmpZipValueFile_.fpath = tmpValueFile_.path + ".zbs";
      tmpZipDictFile_.fpath = tmpValueFile_.path + ".zbs-dict";
      valvec<byte_t> strDict(data, size);
#if defined(MADV_DONTNEED)
      madvise(data, size, MADV_DONTNEED);
#endif
      zbuilder_.reset(this->createZipBuilder());
      zbuilder_->useSample(strDict); // take ownership of strDict
                                      //zbuilder_->finishSample(); // do not call finishSample here
      zbuilder_->prepare(1024, tmpZipValueFile_.fpath);
    }
  }
}

DictZipBlobStore::ZipBuilder*
TerarkZipTableBuilder::createZipBuilder() const {
  DictZipBlobStore::Options dzopt;
  dzopt.entropyAlgo = DictZipBlobStore::Options::EntropyAlgo(table_options_.entropyAlgo);
  dzopt.checksumLevel = table_options_.checksumLevel;
  dzopt.offsetArrayBlockUnits = table_options_.offsetArrayBlockUnits;
  dzopt.useSuffixArrayLocalMatch = table_options_.useSuffixArrayLocalMatch;
  return DictZipBlobStore::createZipBuilder(dzopt);
}

TerarkZipTableBuilder::~TerarkZipTableBuilder() {
}

uint64_t TerarkZipTableBuilder::FileSize() const {
  if (0 == offset_) {
    // for compaction caller to split file by increasing size
    auto kvLen = properties_.raw_key_size + properties_.raw_value_size;
    auto fsize = uint64_t(kvLen * table_options_.estimateCompressionRatio);
    if (terark_unlikely(histogram_.empty())) {
      return fsize;
    }
    size_t dictZipMemSize = std::min<size_t>(sampleLenSum_, INT32_MAX) * 6;
    size_t nltTrieMemSize = 0;
    for (auto& item : histogram_) {
      nltTrieMemSize = std::max(nltTrieMemSize,
        item.stat.sumKeyLen + sizeof(SortableStrVec::SEntry) * item.stat.numKeys);
    }
    size_t peakMemSize = std::max(dictZipMemSize, nltTrieMemSize);
    if (peakMemSize < table_options_.softZipWorkingMemLimit) {
      return fsize;
    }
    else {
      return fsize * 5; // notify rocksdb to `Finish()` this table asap.
    }
  }
  else {
    return offset_;
  }
}

TableProperties TerarkZipTableBuilder::GetTableProperties() const {
  TableProperties ret = properties_;
  for (const auto& collector : collectors_) {
    for (const auto& prop : collector->GetReadableProperties()) {
      ret.readable_properties.insert(prop);
    }
    collector->Finish(&ret.user_collected_properties);
  }
  return ret;
}

void TerarkZipTableBuilder::Add(const Slice& key, const Slice& value) {
  if (table_options_.debugLevel == 4) {
    rocksdb::ParsedInternalKey ikey;
    rocksdb::ParseInternalKey(key, &ikey);
    fprintf(tmpDumpFile_, "DEBUG: 1st pass => %s / %s \n",
        ikey.DebugString(true).c_str(), value.ToString(true).c_str());
  }
  DEBUG_PRINT_1ST_PASS_KEY(key);
  uint64_t seqType = DecodeFixed64(key.data() + key.size() - 8);
  ValueType value_type = ValueType(seqType & 255);
  uint64_t offset = uint64_t((properties_.raw_key_size + properties_.raw_value_size)
    * table_options_.estimateCompressionRatio);
  if (IsValueType(value_type)) {
    assert(key.size() >= 8);
    fstring userKey(key.data(), key.size() - 8);
    assert(userKey.size() >= key_prefixLen_);
#if defined(TERARK_SUPPORT_UINT64_COMPARATOR) && BOOST_ENDIAN_LITTLE_BYTE
    uint64_t u64_key;
    if (isUint64Comparator_) {
      assert(userKey.size() == 8);
      u64_key = byte_swap(*reinterpret_cast<const uint64_t*>(userKey.data()));
      userKey = fstring(reinterpret_cast<const char*>(&u64_key), 8);
    }
#endif
    if (terark_likely(!histogram_.empty()
      && histogram_.back().prefix == userKey.substr(0, key_prefixLen_))) {
      userKey = userKey.substr(key_prefixLen_);
      if (prevUserKey_ != userKey) {
        auto& keyStat = histogram_.back().stat;
        assert((prevUserKey_ < userKey) ^ isReverseBytewiseOrder_);
        keyStat.commonPrefixLen = fstring(prevUserKey_.data(), keyStat.commonPrefixLen)
               .commonPrefixLen(userKey);
        keyStat.minKeyLen = std::min(userKey.size(), keyStat.minKeyLen);
        keyStat.maxKeyLen = std::max(userKey.size(), keyStat.maxKeyLen);
        AddPrevUserKey();
        prevUserKey_.assign(userKey);
      }
    }
    else {
      if (terark_unlikely(histogram_.empty())) {
        t0 = g_pf.now();
      }
      else {
        AddPrevUserKey(true);
      }
      histogram_.emplace_back();
      auto& currentHistogram = histogram_.back();
      currentHistogram.prefix.assign(userKey.data(), key_prefixLen_);
      userKey = userKey.substr(key_prefixLen_);
      currentHistogram.stat.commonPrefixLen = userKey.size();
      currentHistogram.stat.minKeyLen = userKey.size();
      currentHistogram.stat.maxKeyLen = userKey.size();
      currentHistogram.stat.sumKeyLen = 0;
      currentHistogram.stat.numKeys = 0;
      currentHistogram.stat.minKey.assign(userKey);
      prevUserKey_.assign(userKey);
    }
    valueBits_.push_back(true);
    valueBuf_.emplace_back((char*)&seqType, 8);
    valueBuf_.back_append(value.data(), value.size());
    if (!zbuilder_) {
      if (!value.empty() && randomGenerator_() < sampleUpperBound_) {
        tmpSampleFile_.writer << fstringOf(value);
        sampleLenSum_ += value.size();
      }
      if (!second_pass_iter_) {
        tmpValueFile_.writer << seqType;
        tmpValueFile_.writer << fstringOf(value);
      }
    }
    properties_.num_entries++;
    properties_.raw_key_size += key.size();
    properties_.raw_value_size += value.size();
    NotifyCollectTableCollectorsOnAdd(key, value, offset,
      collectors_, ioptions_.info_log);
  }
  else if (value_type == kTypeRangeDeletion) {
    range_del_block_.Add(key, value);
    properties_.num_entries++;
    properties_.raw_key_size += key.size();
    properties_.raw_value_size += value.size();
    NotifyCollectTableCollectorsOnAdd(key, value, offset,
      collectors_, ioptions_.info_log);
  }
  else {
    assert(false);
  }
}

static std::mutex zipMutex;
static std::condition_variable zipCond;
static valvec<PendingTask> waitQueue;
static size_t sumWaitingMem = 0;
static size_t sumWorkingMem = 0;

TerarkZipTableBuilder::WaitHandle::WaitHandle() : myWorkMem(0) {
}
TerarkZipTableBuilder::WaitHandle::WaitHandle(size_t workMem) : myWorkMem(workMem) {
}
TerarkZipTableBuilder::WaitHandle::WaitHandle(WaitHandle&& other) : myWorkMem(other.myWorkMem) {
  other.myWorkMem = 0;
}
TerarkZipTableBuilder::WaitHandle& TerarkZipTableBuilder::WaitHandle::operator = (WaitHandle&& other) {
  Release();
  myWorkMem = other.myWorkMem;
  other.myWorkMem = 0;
  return *this;
}
void TerarkZipTableBuilder::WaitHandle::Release(size_t size) {
  assert(size <= myWorkMem);
  if (size == 0) {
    size = myWorkMem;
  }
  if (myWorkMem > 0) {
    std::unique_lock<std::mutex> zipLock(zipMutex);
    assert(sumWorkingMem >= myWorkMem);
    sumWorkingMem -= size;
    zipCond.notify_all();
    myWorkMem -= size;
  }
}
TerarkZipTableBuilder::WaitHandle::~WaitHandle() {
  Release(myWorkMem);
}

TerarkZipTableBuilder::WaitHandle TerarkZipTableBuilder::WaitForMemory(const char* who, size_t myWorkMem) {
  const size_t softMemLimit = table_options_.softZipWorkingMemLimit;
  const size_t hardMemLimit = std::max(table_options_.hardZipWorkingMemLimit, softMemLimit);
  const size_t smallmem = table_options_.smallTaskMemory;
  const std::chrono::seconds waitForTime(10);
  long long myStartTime = 0, now;
  auto shouldWait = [&]() {
    bool w;
    if (myWorkMem < softMemLimit) {
      w = (sumWorkingMem + myWorkMem >= hardMemLimit) ||
        (sumWorkingMem + myWorkMem >= softMemLimit && myWorkMem >= smallmem);
    }
    else {
      w = sumWorkingMem > softMemLimit / 4;
    }
    now = g_pf.now();
    if (!w) {
      assert(!waitQueue.empty());
      if (myWorkMem < smallmem) {
        return false; // do not wait
      }
      if (sumWaitingMem + sumWorkingMem < softMemLimit) {
        return false; // do not wait
      }
      if (waitQueue.size() == 1) {
        assert(this == waitQueue[0].tztb);
        return false; // do not wait
      }
      size_t minRateIdx = size_t(-1);
      double minRateVal = DBL_MAX;
      auto wq = waitQueue.data();
      for (size_t i = 0, n = waitQueue.size(); i < n; ++i) {
        double rate = myWorkMem / (0.1 + now - wq[i].startTime);
        if (rate < minRateVal) {
          minRateVal = rate;
          minRateIdx = i;
        }
      }
      if (this == wq[minRateIdx].tztb) {
        return false; // do not wait
      }
      myStartTime = wq[minRateIdx].startTime;
    }
    return true; // wait
  };
  std::unique_lock<std::mutex> zipLock(zipMutex);
  sumWaitingMem += myWorkMem;
  while (shouldWait()) {
    INFO(ioptions_.info_log
      , "TerarkZipTableBuilder::Finish():this=%012p: sumWaitingMem =%7.3f GB, sumWorkingMem =%7.3f GB, %-10s workingMem =%8.4f GB, wait...\n"
      , this, sumWaitingMem / 1e9, sumWorkingMem / 1e9, who, myWorkMem / 1e9
    );
    zipCond.wait_for(zipLock, waitForTime);
  }
  if (myStartTime == 0) {
    auto wq = waitQueue.data();
    for (size_t i = 0, n = waitQueue.size(); i < n; ++i) {
      if (this == wq[i].tztb) {
        myStartTime = wq[i].startTime;
        break;
      }
    }
  }
  INFO(ioptions_.info_log
    , "TerarkZipTableBuilder::Finish():this=%012p: sumWaitingMem =%8.3f GB, sumWorkingMem =%8.3f GB, %-10s workingMem =%8.4f GB, waited %9.3f sec, Key+Value bytes =%8.3f GB\n"
    , this, sumWaitingMem / 1e9, sumWorkingMem / 1e9, who, myWorkMem / 1e9
    , g_pf.sf(myStartTime, now)
    , (properties_.raw_key_size + properties_.raw_value_size) / 1e9
  );
  sumWaitingMem -= myWorkMem;
  sumWorkingMem += myWorkMem;
  return WaitHandle{myWorkMem};
}

Status TerarkZipTableBuilder::EmptyTableFinish() {
  INFO(ioptions_.info_log
    , "TerarkZipTableBuilder::EmptyFinish():this=%012p\n", this);
  offset_ = 0;
  BlockHandle emptyTableBH, tombstoneBH(0, 0);
  Status s = WriteBlock(Slice("Empty"), file_, &offset_, &emptyTableBH);
  if (!s.ok()) {
    return s;
  }
#if defined(TerocksPrivateCode)
  auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions_.table_factory);
  assert(table_factory);
  auto& license = table_factory->GetLicense();
  BlockHandle licenseHandle;
  s = WriteBlock(SliceOf(license.dump()), file_, &offset_, &licenseHandle);
  if (!s.ok()) {
    return s;
  }
#endif // TerocksPrivateCode
  if (!range_del_block_.empty()) {
    s = WriteBlock(range_del_block_.Finish(), file_, &offset_, &tombstoneBH);
    if (!s.ok()) {
      return s;
    }
  }
  range_del_block_.Reset();
  return WriteMetaData({
#if defined(TerocksPrivateCode)
    { &kTerarkZipTableExtendedBlock                 , licenseHandle },
#endif // TerocksPrivateCode
    { &kTerarkEmptyTableKey                         , emptyTableBH  },
    { !tombstoneBH.IsNull() ? &kRangeDelBlock : NULL, tombstoneBH   },
  });
}


Status TerarkZipTableBuilder::Finish() {
  assert(!closed_);
  closed_ = true;

  if (histogram_.empty()) {
    return EmptyTableFinish();
  }

  AddPrevUserKey(true);
  for (auto& item : histogram_) {
    item.key.finish();
    item.value.finish();
    if (item.stat.numKeys == 1) {
      assert(item.stat.commonPrefixLen == item.stat.sumKeyLen);
      item.stat.commonPrefixLen = 0; // to avoid empty nlt trie
    }
  }
  tmpKeyFile_.complete_write();
  if (zbuilder_) {
    return OfflineFinish();
  }

  if (!second_pass_iter_) {
    tmpValueFile_.complete_write();
  }
  tmpSampleFile_.complete_write();
  {
    long long rawBytes = properties_.raw_key_size + properties_.raw_value_size;
    long long tt = g_pf.now();
    INFO(ioptions_.info_log
      , "TerarkZipTableBuilder::Finish():this=%012p:  first pass time =%8.2f's,%8.3f'MB/sec\n"
      , this, g_pf.sf(t0, tt), rawBytes*1.0 / g_pf.uf(t0, tt)
    );
  }
  {
    std::unique_lock<std::mutex> zipLock(zipMutex);
    waitQueue.push_back({this, g_pf.now()});
  }
  BOOST_SCOPE_EXIT(this_) {
    std::unique_lock<std::mutex> zipLock(zipMutex);
    waitQueue.trim(std::remove_if(waitQueue.begin(), waitQueue.end(),
      [this_](PendingTask x) {return this_ == x.tztb; }));
  } BOOST_SCOPE_EXIT_END;

  // indexing is also slow, run it in parallel
  AutoDeleteFile tmpIndexFile{tmpValueFile_.path + ".index"};
  std::future<void> asyncIndexResult = std::async(std::launch::async, [&]()
  {
    size_t fileOffset = 0;
    FileStream writer(tmpIndexFile, "wb+");
    NativeDataInput<InputBuffer> tempKeyFileReader(&tmpKeyFile_.fp);
    for (size_t i = 0; i < histogram_.size(); ++i) {
      auto& keyStat = histogram_[i].stat;
      auto factory = TerarkIndex::SelectFactory(keyStat, table_options_.indexType);
      if (!factory) {
        THROW_STD(invalid_argument,
          "invalid indexType: %s", table_options_.indexType.c_str());
      }
      const size_t myWorkMem = factory->MemSizeForBuild(keyStat);
      auto waitHandle = WaitForMemory("nltTrie", myWorkMem);

      long long t1 = g_pf.now();
      histogram_[i].keyFileBegin = fileOffset;
      factory->Build(tempKeyFileReader, table_options_
          , [&fileOffset, &writer](const void* data, size_t size) {
              fileOffset += size;
              writer.ensureWrite(data, size);
            }
          , keyStat);
      histogram_[i].keyFileEnd = fileOffset;
      assert((fileOffset - histogram_[i].keyFileBegin) % 8 == 0);
      long long tt = g_pf.now();
      INFO(ioptions_.info_log
        , "TerarkZipTableBuilder::Finish():this=%012p:  index pass time =%8.2f's,%8.3f'MB/sec\n"
        , this, g_pf.sf(t1, tt), properties_.raw_key_size*1.0 / g_pf.uf(t1, tt)
      );
    }
    tmpKeyFile_.close();
  });

  auto waitIndex = [&]() {
    asyncIndexResult.get();
  };
#if defined(TerocksPrivateCode)
  if (histogram_.size() > 1) {
    return ZipValueToFinishMulti(tmpIndexFile, waitIndex);
  }
#endif // TerocksPrivateCode
  return ZipValueToFinish(tmpIndexFile, waitIndex);
}

#if defined(TerocksPrivateCode)
std::unique_ptr<BlobStore> TerarkZipTableBuilder::buildZeroLengthBlobStore(BuildStoreParams &params) {
  auto& kvs = params.kvs;
  auto store = UniquePtrOf(new terark::ZeroLengthBlobStore());
  BuilderWriteValues(params.input, kvs, [&](fstring value) { assert(value.empty()); });
  store->finish(kvs.stat.numKeys);
  return std::move(store);
};
std::unique_ptr<BlobStore> TerarkZipTableBuilder::buildPlainBlobStore(BuildStoreParams &params) {
  auto& kvs = params.kvs;
  size_t workingMemory = kvs.value.m_total_key_len
      + UintVecMin0::compute_mem_size_by_max_val(kvs.value.m_total_key_len, kvs.stat.numKeys);
  params.handle = WaitForMemory("plain", workingMemory);
  auto store = UniquePtrOf(new terark::PlainBlobStore());
  store->reset_with_content_size(kvs.value.m_total_key_len);
  BuilderWriteValues(params.input, kvs, [&](fstring value) {store->add_record(value); });
  store->finish();
  return std::move(store);
};
std::unique_ptr<BlobStore> TerarkZipTableBuilder::buildMixedLenBlobStore(BuildStoreParams &params) {
  auto& kvs = params.kvs;
  size_t fixedLen = kvs.value.m_max_cnt_key;
  size_t fixedLenCount = kvs.value.m_cnt_of_max_cnt_key;
  size_t varDataLen = kvs.value.m_total_key_len - fixedLen * fixedLenCount;
  size_t workingMemory = kvs.value.m_total_key_len
      + UintVecMin0::compute_mem_size_by_max_val(varDataLen, kvs.stat.numKeys - fixedLenCount);
  params.handle = WaitForMemory("mixedLen", workingMemory);
  auto builder = UniquePtrOf(new terark::MixedLenBlobStore::Builder(fixedLen));
  BuilderWriteValues(params.input, kvs, [&](fstring value) {builder->add_record(value); });
  return UniquePtrOf(builder->finish());
};
std::unique_ptr<BlobStore> TerarkZipTableBuilder::buildZipOffsetBlobStore(BuildStoreParams &params) {
  auto& kvs = params.kvs;
  size_t workingMemory = kvs.value.m_total_key_len + kvs.stat.numKeys;
  size_t blockUnits = table_options_.offsetArrayBlockUnits;
  params.handle = WaitForMemory("zipOffset", workingMemory);
  auto builder = UniquePtrOf(new terark::ZipOffsetBlobStore::Builder(blockUnits));
  BuilderWriteValues(params.input, kvs, [&](fstring value) {builder->add_record(value); });
  return UniquePtrOf(builder->finish());
};
#endif // TerocksPrivateCode

Status
TerarkZipTableBuilder::
ZipValueToFinish(fstring tmpIndexFile, std::function<void()> waitIndex) {
  DebugPrepare();
  assert(histogram_.size() == 1);
  AutoDeleteFile tmpStoreFile{tmpValueFile_.path + ".zbs"};
  AutoDeleteFile tmpDictFile{tmpValueFile_.path + ".dict"};
  NativeDataInput<InputBuffer> input(&tmpValueFile_.fp);
  auto& kvs = histogram_.front();
  std::unique_ptr<DictZipBlobStore::ZipBuilder> zbuilder;
  DictZipBlobStore::ZipStat dzstat;
  long long t3, t4;
#if defined(TerocksPrivateCode)
  auto avgValueLen = kvs.value.m_total_key_len / kvs.stat.numKeys;
  if (avgValueLen < table_options_.minDictZipValueSize) {
    size_t fixedNum = kvs.value.m_cnt_of_max_cnt_key;
    size_t variaNum = kvs.stat.numKeys - fixedNum;
    std::unique_ptr<terark::BlobStore> store;
    BuildStoreParams params = {input, kvs, 0};
    t3 = g_pf.now();
    if (kvs.value.m_total_key_len == 0) {
      store = buildZeroLengthBlobStore(params);
    }
    else if (table_options_.offsetArrayBlockUnits) {
      if (kvs.stat.numKeys < (4ull << 30) && variaNum * 64 < kvs.stat.numKeys) {
        store = buildMixedLenBlobStore(params);
      }
      else {
        store = buildZipOffsetBlobStore(params);
      }
    }
    else {
      if (kvs.stat.numKeys < (4ull << 30) &&
          4 * variaNum + kvs.stat.numKeys * 5 / 4 < 4 * kvs.stat.numKeys) {
        store = buildMixedLenBlobStore(params);
      }
      else {
        store = buildPlainBlobStore(params);
      }
    }
    store->save_mmap(tmpStoreFile);
    tmpDictFile.fpath.clear();
    t4 = g_pf.now();
    dzstat.dictBuildTime = 0.000001;
    dzstat.dictFileTime = 0.000001;
    dzstat.dictZipTime = g_pf.sf(t3, t4);
    dzstat.sampleTime = 0.000001;
  }
  else
#endif // TerocksPrivateCode
  {
    size_t dictWorkingMemory = std::min<size_t>(sampleLenSum_, INT32_MAX) * 6;
    auto dictWaitHandle = WaitForMemory("dictZip", dictWorkingMemory);
    t3 = g_pf.now();
    zbuilder = UniquePtrOf(this->createZipBuilder());
    {
      valvec<byte_t> sample;
      NativeDataInput<InputBuffer> input(&tmpSampleFile_.fp);
      size_t realsampleLenSum = 0;
      if (sampleLenSum_ < INT32_MAX) {
        for (size_t len = 0; len < sampleLenSum_; ) {
          input >> sample;
          zbuilder->addSample(sample);
          len += sample.size();
        }
        realsampleLenSum = sampleLenSum_;
      }
      else {
        uint64_t upperBound2 = uint64_t(
          randomGenerator_.max() * double(INT32_MAX) / sampleLenSum_);
        for (size_t len = 0; len < sampleLenSum_; ) {
          input >> sample;
          if (randomGenerator_() < upperBound2) {
            zbuilder->addSample(sample);
            realsampleLenSum += sample.size();
          }
          len += sample.size();
        }
      }
      tmpSampleFile_.close();
      if (0 == realsampleLenSum) { // prevent from empty
        zbuilder->addSample("Hello World!");
      }
      zbuilder->finishSample();
      zbuilder->prepare(kvs.stat.numKeys, tmpStoreFile);
    }

    BuilderWriteValues(input, kvs, [&](fstring value) {zbuilder->addRecord(value); });

    auto store = zbuilder->finish(DictZipBlobStore::ZipBuilder::FinishFreeDict
        | DictZipBlobStore::ZipBuilder::FinishWithoutReload);
    assert(store == nullptr);
    (void)store;
    dzstat = zbuilder->getZipStat();

    t4 = g_pf.now();
    auto dict = zbuilder->getDictionary().memory;
    FileStream(tmpDictFile, "wb+").ensureWrite(dict.data(), dict.size());
    zbuilder.reset();
  }
  DebugCleanup();
  // wait for indexing complete, if indexing is slower than value compressing
  waitIndex();
  return WriteSSTFile(t3, t4, tmpIndexFile, tmpStoreFile, tmpDictFile, dzstat);
}

#if defined(TerocksPrivateCode)

Status TerarkZipTableBuilder::
ZipValueToFinishMulti(fstring tmpIndexFile, std::function<void()> waitIndex) {
  DebugPrepare();
  assert(histogram_.size() > 1);
  AutoDeleteFile tmpStoreFile{tmpValueFile_.path + ".zbs"};
  AutoDeleteFile tmpDictFile{tmpValueFile_.path + ".dict"};
  NativeDataInput<InputBuffer> input(&tmpValueFile_.fp);
  auto zbuilder = UniquePtrOf(this->createZipBuilder());
  std::unique_ptr<terark::BlobStore> store;
  DictZipBlobStore::ZipStat dzstat;
  long long t1, t2, t3, t4;

  auto minDictZipValueSize = table_options_.minDictZipValueSize / 2;
  size_t dictWorkingMemory = std::min<size_t>(sampleLenSum_, INT32_MAX) * 6;
  WaitHandle dictWaitHandle;
  size_t fileOffset = 0;
  size_t dictRefCount = 0;

  t3 = g_pf.now();

  for (size_t i = 0; i < histogram_.size(); ++i) {
    auto& kvs = histogram_[i];
    kvs.valueFileBegin = fileOffset;
    auto avgValueLen = kvs.value.m_total_key_len / kvs.stat.numKeys;
    if (avgValueLen < minDictZipValueSize) {
      size_t fixedNum = kvs.value.m_cnt_of_max_cnt_key;
      size_t variaNum = kvs.stat.numKeys - fixedNum;
      BuildStoreParams params = {input, kvs, 0};
      std::unique_ptr<terark::BlobStore> store;
      if (kvs.value.m_total_key_len == 0) {
        store = buildZeroLengthBlobStore(params);
      }
      else if (table_options_.offsetArrayBlockUnits) {
        if (kvs.stat.numKeys < (4ull << 30) && variaNum * 64 < kvs.stat.numKeys) {
          store = buildMixedLenBlobStore(params);
        }
        else {
          store = buildZipOffsetBlobStore(params);
        }
      }
      else {
        if (kvs.stat.numKeys < (4ull << 30) &&
            4 * variaNum + kvs.stat.numKeys * 5 / 4 < 4 * kvs.stat.numKeys) {
          store = buildMixedLenBlobStore(params);
        }
        else {
          store = buildPlainBlobStore(params);
        }
      }
      FileStream file(tmpStoreFile.fpath.c_str(), "ab+");
      store->save_mmap([&](const void* d, size_t s) {
        file.ensureWrite(d, s);
      });
    }
    else {
      if (dictRefCount == 0) {
        dictWaitHandle = WaitForMemory("dictZip", dictWorkingMemory);
        valvec<byte_t> sample;
        t1 = g_pf.now();
        NativeDataInput<InputBuffer> sampleInput(&tmpSampleFile_.fp);
        size_t realsampleLenSum = 0;
        if (sampleLenSum_ < INT32_MAX) {
          for (size_t len = 0; len < sampleLenSum_; ) {
            sampleInput >> sample;
            zbuilder->addSample(sample);
            len += sample.size();
          }
          realsampleLenSum = sampleLenSum_;
        }
        else {
          uint64_t upperBound2 = uint64_t(
            randomGenerator_.max() * double(INT32_MAX) / sampleLenSum_);
          for (size_t len = 0; len < sampleLenSum_; ) {
            sampleInput >> sample;
            if (randomGenerator_() < upperBound2) {
              zbuilder->addSample(sample);
              realsampleLenSum += sample.size();
            }
            len += sample.size();
          }
        }
        tmpSampleFile_.close();
        if (0 == realsampleLenSum) { // prevent from empty
          zbuilder->addSample("Hello World!");
        }
        zbuilder->finishSample();
        t2 = g_pf.now();
      }
      zbuilder->prepare(kvs.stat.numKeys, tmpStoreFile, fileOffset);
      BuilderWriteValues(input, kvs, [&](fstring value) {zbuilder->addRecord(value); });
      auto store = zbuilder->finish(DictZipBlobStore::ZipBuilder::FinishWithoutReload);
      assert(store == nullptr);
      (void)store; // shut up !
      if (dictRefCount == 0) {
        dzstat = zbuilder->getZipStat();
      }
      ++dictRefCount;
    }
    fileOffset = FileStream(tmpStoreFile.fpath.c_str(), "rb+").fsize();
    kvs.valueFileEnd = fileOffset;
    assert((kvs.valueFileEnd - kvs.valueFileBegin) % 8 == 0);
  }
  if (dictRefCount > 0) {
    zbuilder->freeDict();
    t4 = g_pf.now();
    t3 += t2 - t1;
    auto dict = zbuilder->getDictionary().memory;
    FileStream(tmpDictFile, "wb+").ensureWrite(dict.data(), dict.size());
    zbuilder.reset();
    dictWaitHandle.Release();
  }
  else {
    tmpDictFile.fpath.clear();
    t4 = g_pf.now();
  }

  dzstat.dictZipTime = g_pf.sf(t3, t4);

  DebugCleanup();
  // wait for indexing complete, if indexing is slower than value compressing
  waitIndex();
  return WriteSSTFileMulti(t3, t4, tmpIndexFile, tmpStoreFile, tmpDictFile, dzstat);
}

#endif // TerocksPrivateCode

void TerarkZipTableBuilder::DebugPrepare() {
}

void TerarkZipTableBuilder::DebugCleanup() {
  if (tmpDumpFile_.isOpen()) {
    tmpDumpFile_.close();
  }
  tmpValueFile_.close();
}

void
TerarkZipTableBuilder::BuilderWriteValues(NativeDataInput<InputBuffer>& input,
  KeyValueStatus& kvs, std::function<void(fstring)> write) {
  auto& bzvType = kvs.type;
  bzvType.resize(kvs.stat.numKeys);
  if (nullptr == second_pass_iter_)
  {
    valvec<byte_t> value;
    size_t entryId = 0;
    size_t bitPos = 0;
    for (size_t recId = 0; recId < kvs.stat.numKeys; recId++) {
      uint64_t seqType = input.load_as<uint64_t>();
      uint64_t seqNum;
      ValueType vType;
      UnPackSequenceAndType(seqType, &seqNum, &vType);
      size_t oneSeqLen = valueBits_.one_seq_len(bitPos);
      assert(oneSeqLen >= 1);
      if (1 == oneSeqLen && (kTypeDeletion == vType || kTypeValue == vType)) {
        if (0 == seqNum && kTypeValue == vType) {
          bzvType.set0(recId, size_t(ZipValueType::kZeroSeq));
          input >> value;
        }
        else {
          if (kTypeValue == vType) {
            bzvType.set0(recId, size_t(ZipValueType::kValue));
          }
          else {
            bzvType.set0(recId, size_t(ZipValueType::kDelete));
          }
          value.erase_all();
          value.append((byte_t*)&seqNum, 7);
          input.load_add(value);
        }
      }
      else {
        bzvType.set0(recId, size_t(ZipValueType::kMulti));
        size_t headerSize = ZipValueMultiValue::calcHeaderSize(oneSeqLen);
        value.resize(headerSize);
        ((ZipValueMultiValue*)value.data())->offsets[0] = uint32_t(oneSeqLen);
        for (size_t j = 0; j < oneSeqLen; j++) {
          if (j > 0) {
            seqType = input.load_as<uint64_t>();
          }
          value.append((byte_t*)&seqType, 8);
          input.load_add(value);
          if (j + 1 < oneSeqLen) {
            ((ZipValueMultiValue*)value.data())->offsets[j + 1] = value.size() - headerSize;
          }
        }
      }
      write(value);
      bitPos += oneSeqLen + 1;
      entryId += oneSeqLen;
    }
    // tmpValueFile_ ignore kTypeRangeDeletion keys
    // so entryId may less than properties_.num_entries
    assert(entryId <= properties_.num_entries);
  }
  else
  {
    valvec<byte_t> value;
    size_t entryId = 0;
    size_t bitPos = 0;
    bool veriftKey = table_options_.debugLevel == 2 || table_options_.debugLevel == 3;
    bool veriftValue = table_options_.debugLevel == 3;
    bool dumpKeyValue = table_options_.debugLevel == 4;
    auto dumpKeyValueFunc = [&](const ParsedInternalKey& ikey, const Slice& value) {
      fprintf(tmpDumpFile_.fp(), "DEBUG: 2nd pass => %s / %s \n", ikey.DebugString(true).c_str(), value.ToString(true).c_str());
    };
    if (veriftKey) {
      //TODO
    }
    if (veriftValue) {
      //TODO
    }

    for (size_t recId = 0; recId < kvs.stat.numKeys; recId++) {
      value.erase_all();
      assert(second_pass_iter_->Valid());
      ParsedInternalKey pikey;
      Slice curKey = second_pass_iter_->key();
      DEBUG_PRINT_2ND_PASS_KEY(curKey);
      ParseInternalKey(curKey, &pikey);
      if (dumpKeyValue) {
        dumpKeyValueFunc(pikey, second_pass_iter_->value());
      }
      while (kTypeRangeDeletion == pikey.type) {
        second_pass_iter_->Next();
        assert(second_pass_iter_->Valid());
        curKey = second_pass_iter_->key();
        DEBUG_PRINT_2ND_PASS_KEY(curKey);
        ParseInternalKey(curKey, &pikey);
        if (dumpKeyValue) {
          dumpKeyValueFunc(pikey, second_pass_iter_->value());
        }
        entryId += 1;
      }
      if (veriftKey) {
        pikey.user_key.remove_prefix(key_prefixLen_);
        //TODO
      }
      Slice curVal = second_pass_iter_->value();
      size_t oneSeqLen = valueBits_.one_seq_len(bitPos);
      assert(oneSeqLen >= 1);
      if (1 == oneSeqLen && (kTypeDeletion == pikey.type || kTypeValue == pikey.type)) {
        //assert(fstringOf(pikey.user_key) == backupKeys[recId]);
        if (veriftValue) {
          //TODO
        }
        if (0 == pikey.sequence && kTypeValue == pikey.type) {
          bzvType.set0(recId, size_t(ZipValueType::kZeroSeq));
          write(fstringOf(curVal));
        }
        else {
          if (kTypeValue == pikey.type) {
            bzvType.set0(recId, size_t(ZipValueType::kValue));
          }
          else {
            bzvType.set0(recId, size_t(ZipValueType::kDelete));
          }
          value.append((byte_t*)&pikey.sequence, 7);
          value.append(fstringOf(curVal));
          write(value);
        }
        second_pass_iter_->Next();
      }
      else {
        bzvType.set0(recId, size_t(ZipValueType::kMulti));
        size_t headerSize = ZipValueMultiValue::calcHeaderSize(oneSeqLen);
        value.resize(headerSize);
        ((ZipValueMultiValue*)value.data())->offsets[0] = uint32_t(oneSeqLen);
        for (size_t j = 0; j < oneSeqLen; j++) {
          if (j > 0) {
            assert(second_pass_iter_->Valid());
            curKey = second_pass_iter_->key();
            DEBUG_PRINT_2ND_PASS_KEY(curKey);
            ParseInternalKey(curKey, &pikey);
            if (dumpKeyValue) {
              dumpKeyValueFunc(pikey, second_pass_iter_->value());
            }
            while (kTypeRangeDeletion == pikey.type) {
              second_pass_iter_->Next();
              assert(second_pass_iter_->Valid());
              curKey = second_pass_iter_->key();
              DEBUG_PRINT_2ND_PASS_KEY(curKey);
              ParseInternalKey(curKey, &pikey);
              if (dumpKeyValue) {
                dumpKeyValueFunc(pikey, second_pass_iter_->value());
              }
              entryId += 1;
            }
            curVal = second_pass_iter_->value();
          }
          else {
            assert(kTypeRangeDeletion != pikey.type);
          }
          if (veriftValue) {
            //TODO
          }
          //assert(fstringOf(pikey.user_key) == backupKeys[recId]);
          uint64_t seqType = PackSequenceAndType(pikey.sequence, pikey.type);
          value.append((byte_t*)&seqType, 8);
          value.append(fstringOf(curVal));
          if (j + 1 < oneSeqLen) {
            ((ZipValueMultiValue*)value.data())->offsets[j + 1] = value.size() - headerSize;
          }
          second_pass_iter_->Next();
        }
        write(value);
      }
      bitPos += oneSeqLen + 1;
      entryId += oneSeqLen;
    }
    // second pass no range deletion ...
    //assert(entryId <= properties_.num_entries);
  }
}

Status TerarkZipTableBuilder::WriteStore(TerarkIndex* index, terark::BlobStore* store
  , KeyValueStatus& kvs
  , BlockHandle& dataBlock
  , long long& t5, long long& t6, long long& t7) {
  auto& keyStat = kvs.stat;
  auto& bzvType = kvs.type;
  INFO(ioptions_.info_log
      , "TerarkZipTableBuilder::Finish():this=%012p:  index type = %-32s, store type = %-20s\n"
      , this, index->Name(), store->name()
  );
  using namespace std::placeholders;
  auto writeAppend = std::bind(&TerarkZipTableBuilder::DoWriteAppend, this, _1, _2);
  size_t maxUintVecVal = keyStat.numKeys - 1;
  if (index->NeedsReorder()) {
    bitfield_array<2> zvType2(keyStat.numKeys);
    size_t workingMemory = UintVecMin0::compute_mem_size_by_max_val(keyStat.numKeys, maxUintVecVal);
    auto waitHandle = WaitForMemory("reorder", workingMemory);
    UintVecMin0 newToOld(keyStat.numKeys, maxUintVecVal);
    index->GetOrderMap(newToOld);
    t6 = g_pf.now();
    if (fstring(ioptions_.user_comparator->Name()).startsWith("rev:")) {
      // Damn reverse bytewise order
      for (size_t newId = 0; newId < keyStat.numKeys; ++newId) {
        size_t dictOrderOldId = newToOld[newId];
        size_t reverseOrderId = keyStat.numKeys - dictOrderOldId - 1;
        newToOld.set_wire(newId, reverseOrderId);
        zvType2.set0(newId, bzvType[reverseOrderId]);
      }
    }
    else {
      for (size_t newId = 0; newId < keyStat.numKeys; ++newId) {
        size_t dictOrderOldId = newToOld[newId];
        zvType2.set0(newId, bzvType[dictOrderOldId]);
      }
    }
    t7 = g_pf.now();
    try {
      dataBlock.set_offset(offset_);
      store->reorder_zip_data(newToOld, std::ref(writeAppend));
      dataBlock.set_size(offset_ - dataBlock.offset());
    }
    catch (const Status& s) {
      return s;
    }
    bzvType.clear();
    bzvType.swap(zvType2);
  }
  else {
    if (fstring(ioptions_.user_comparator->Name()).startsWith("rev:")) {
      bitfield_array<2> zvType2(keyStat.numKeys);
      size_t workingMemory = UintVecMin0::compute_mem_size_by_max_val(keyStat.numKeys, maxUintVecVal);
      auto waitHandle = WaitForMemory("reorder", workingMemory);
      UintVecMin0 newToOld(keyStat.numKeys, maxUintVecVal);
      t6 = g_pf.now();
      for (size_t newId = 0, oldId = keyStat.numKeys - 1; newId < keyStat.numKeys;
        ++newId, --oldId) {
        newToOld.set_wire(newId, oldId);
        zvType2.set0(newId, bzvType[oldId]);
      }
      t7 = g_pf.now();
      try {
        dataBlock.set_offset(offset_);
        store->reorder_zip_data(newToOld, std::ref(writeAppend));
        dataBlock.set_size(offset_ - dataBlock.offset());
      }
      catch (const Status& s) {
        return s;
      }
      bzvType.clear();
      bzvType.swap(zvType2);
    }
    else {
      t7 = t6 = t5;
      try {
        dataBlock.set_offset(offset_);
        store->save_mmap(std::ref(writeAppend));
        dataBlock.set_size(offset_ - dataBlock.offset());
      }
      catch (const Status& s) {
        return s;
      }
    }
  }
  return Status::OK();
}

void TerarkZipTableBuilder::DoWriteAppend(const void* data, size_t size) {
  Status s = file_->Append(Slice((const char*)data, size));
  if (!s.ok()) {
    throw s;
  }
  offset_ += size;
}

Status TerarkZipTableBuilder::WriteSSTFile(long long t3, long long t4
  , fstring tmpIndexFile
  , fstring tmpStoreFile
  , fstring tmpDictFile
  , const DictZipBlobStore::ZipStat& dzstat)
{
  assert(histogram_.size() == 1);
  terark::MmapWholeFile dictMmap;
  BlobStore::Dictionary dict;
  if (!tmpDictFile.empty()) {
    terark::MmapWholeFile(tmpDictFile.c_str()).swap(dictMmap);
    dict = BlobStore::Dictionary(fstring{(const char*)dictMmap.base,
        (ptrdiff_t)dictMmap.size});
  }
  terark::MmapWholeFile storeMmap(tmpStoreFile.c_str());
  auto store = UniquePtrOf(BlobStore::load_from_user_memory(
      fstring{(const char*)storeMmap.base, (ptrdiff_t)storeMmap.size}, dict));
  auto& kvs = histogram_.front();
  auto& keyStat = kvs.stat;
  auto& bzvType = kvs.type;
  const size_t realsampleLenSum = dict.memory.size();
  long long rawBytes = properties_.raw_key_size + properties_.raw_value_size;
  long long t5 = g_pf.now();
  unique_ptr<TerarkIndex> index(TerarkIndex::LoadFile(tmpIndexFile));
  assert(index->NumKeys() == keyStat.numKeys);
  Status s;
  BlockHandle dataBlock, dictBlock, indexBlock, zvTypeBlock(0, 0), tombstoneBlock(0, 0);
  BlockHandle commonPrefixBlock;
  {
    size_t real_size = index->Memory().size() + store->mem_size() + bzvType.mem_size();
    size_t block_size, last_allocated_block;
    file_->writable_file()->GetPreallocationStatus(&block_size, &last_allocated_block);
    INFO(ioptions_.info_log
      , "TerarkZipTableBuilder::Finish():this=%012p: old prealloc_size = %zd, real_size = %zd\n"
      , this, block_size, real_size
    );
    file_->writable_file()->SetPreallocationBlockSize(1 * 1024 * 1024 + real_size);
  }
  long long t6, t7;
  offset_ = 0;
  s = WriteStore(index.get(), store.get(), kvs, dataBlock, t5, t6, t7);
  if (!s.ok()) {
    return s;
  }
  std::string commonPrefix;
  commonPrefix.reserve(key_prefixLen_ + keyStat.commonPrefixLen);
  commonPrefix.append(kvs.prefix.data(), kvs.prefix.size());
  commonPrefix.append((const char*)prevUserKey_.data(), keyStat.commonPrefixLen);
  WriteBlock(commonPrefix, file_, &offset_, &commonPrefixBlock);
  properties_.data_size = dataBlock.size();
  s = WriteBlock(dict.memory, file_, &offset_, &dictBlock);
  if (!s.ok()) {
    return s;
  }
  s = WriteBlock(index->Memory(), file_, &offset_, &indexBlock);
  if (!s.ok()) {
    return s;
  }
  if (zeroSeqCount_ != bzvType.size()) {
    assert(zeroSeqCount_ < bzvType.size());
    fstring zvTypeMem(bzvType.data(), bzvType.mem_size());
    s = WriteBlock(zvTypeMem, file_, &offset_, &zvTypeBlock);
    if (!s.ok()) {
      return s;
    }
  }
  index.reset();
#if defined(TerocksPrivateCode)
  auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions_.table_factory);
  assert(table_factory);
  auto& license = table_factory->GetLicense();
  BlockHandle licenseHandle;
  s = WriteBlock(SliceOf(license.dump()), file_, &offset_, &licenseHandle);
  if (!s.ok()) {
    return s;
  }
#endif // TerocksPrivateCode
  if (!range_del_block_.empty()) {
    s = WriteBlock(range_del_block_.Finish(), file_, &offset_, &tombstoneBlock);
    if (!s.ok()) {
      return s;
    }
  }
  range_del_block_.Reset();
  properties_.index_size = indexBlock.size();
  WriteMetaData({
#if defined(TerocksPrivateCode)
    { &kTerarkZipTableExtendedBlock                                , licenseHandle     },
#endif // TerocksPrivateCode
    { dict.memory.size() ? &kTerarkZipTableValueDictBlock : NULL   , dictBlock         },
    { &kTerarkZipTableIndexBlock                                   , indexBlock        },
    { !zvTypeBlock.IsNull() ? &kTerarkZipTableValueTypeBlock : NULL, zvTypeBlock       },
    { &kTerarkZipTableCommonPrefixBlock                            , commonPrefixBlock },
    { !tombstoneBlock.IsNull() ? &kRangeDelBlock : NULL            , tombstoneBlock    },
  });
  long long t8 = g_pf.now();
  {
    std::unique_lock<std::mutex> lock(g_sumMutex);
    g_sumKeyLen += properties_.raw_key_size;
    g_sumValueLen += properties_.raw_value_size;
    g_sumUserKeyLen += keyStat.sumKeyLen;
    g_sumUserKeyNum += keyStat.numKeys;
    g_sumEntryNum += properties_.num_entries;
  }
  INFO(ioptions_.info_log,
    "TerarkZipTableBuilder::Finish():this=%012p: second pass time =%8.2f's,%8.3f'MB/sec, value only(%4.1f%% of KV)\n"
    "   wait indexing time = %7.2f's,\n"
    "  remap KeyValue time = %7.2f's, %8.3f'MB/sec (all stages of remap)\n"
    "    Get OrderMap time = %7.2f's, %8.3f'MB/sec (index lex order gen)\n"
    "  rebuild zvType time = %7.2f's, %8.3f'MB/sec\n"
    "  write SST data time = %7.2f's, %8.3f'MB/sec\n"
    "    z-dict build time = %7.2f's, sample length = %7.3f'MB, throughput = %6.3f'MB/sec\n"
    "    zip my value time = %7.2f's, unzip  length = %7.3f'GB\n"
    "    zip my value throughput = %7.3f'MB/sec\n"
    "    zip pipeline throughput = %7.3f'MB/sec\n"
    "    entries = %zd  avg-key = %.2f  avg-zkey = %.2f  avg-val = %.2f  avg-zval = %.2f\n"
    "    usrkeys = %zd  avg-key = %.2f  avg-zkey = %.2f  avg-val = %.2f  avg-zval = %.2f\n"
    "    UnZipSize{ index =%9.4f GB  value =%9.4f GB   all =%9.4f GB }\n"
    "    __ZipSize{ index =%9.4f GB  value =%9.4f GB   all =%9.4f GB }\n"
    "    UnZip/Zip{ index =%9.4f     value =%9.4f      all =%9.4f    }\n"
    "    Zip/UnZip{ index =%9.4f     value =%9.4f      all =%9.4f    }\n"
    "----------------------------\n"
    "    total value len =%14.6f GB     avg =%8.3f KB (by entry num)\n"
    "    total  key  len =%14.6f GB     avg =%8.3f KB\n"
    "    total ukey  len =%14.6f GB     avg =%8.3f KB\n"
    "    total ukey  num =%17.9f Billion\n"
    "    total entry num =%17.9f Billion\n"
    "    write speed all =%17.9f MB/sec (with    version num)\n"
    "    write speed all =%17.9f MB/sec (without version num)"
, this, g_pf.sf(t3, t4)
, properties_.raw_value_size*1.0 / g_pf.uf(t3, t4)
, properties_.raw_value_size*100.0 / rawBytes

, g_pf.sf(t4, t5) // wait indexing time
, g_pf.sf(t5, t8), double(offset_) / g_pf.uf(t5, t8)

, g_pf.sf(t5, t6), properties_.index_size / g_pf.uf(t5, t6) // index lex walk

, g_pf.sf(t6, t7), keyStat.numKeys * 2 / 8 / (g_pf.uf(t6, t7) + 1.0) // rebuild zvType

, g_pf.sf(t7, t8), double(offset_) / g_pf.uf(t7, t8) // write SST data

, dzstat.dictBuildTime, realsampleLenSum / 1e6
, realsampleLenSum / dzstat.dictBuildTime / 1e6

, dzstat.dictZipTime, properties_.raw_value_size / 1e9
, properties_.raw_value_size / dzstat.dictZipTime / 1e6
, dzstat.pipelineThroughBytes / dzstat.dictZipTime / 1e6

, size_t(properties_.num_entries)
, double(properties_.raw_key_size)   / properties_.num_entries
, double(properties_.index_size)     / properties_.num_entries
, double(properties_.raw_value_size) / properties_.num_entries
, double(properties_.data_size)      / properties_.num_entries

, keyStat.numKeys
, double(keyStat.sumKeyLen)          / keyStat.numKeys
, double(properties_.index_size)     / keyStat.numKeys
, double(properties_.raw_value_size) / keyStat.numKeys
, double(properties_.data_size)      / keyStat.numKeys

, keyStat.sumKeyLen / 1e9, properties_.raw_value_size / 1e9, rawBytes / 1e9

, properties_.index_size / 1e9, properties_.data_size / 1e9, offset_ / 1e9

, double(keyStat.sumKeyLen) / properties_.index_size
, double(properties_.raw_value_size) / properties_.data_size
, double(rawBytes) / offset_

, properties_.index_size / double(keyStat.sumKeyLen)
, properties_.data_size / double(properties_.raw_value_size)
, offset_ / double(rawBytes)

, g_sumValueLen / 1e9, g_sumValueLen / 1e3 / g_sumEntryNum
, g_sumKeyLen / 1e9, g_sumKeyLen / 1e3 / g_sumEntryNum
, g_sumUserKeyLen / 1e9, g_sumUserKeyLen / 1e3 / g_sumUserKeyNum
, g_sumUserKeyNum / 1e9
, g_sumEntryNum / 1e9
, (g_sumKeyLen + g_sumValueLen) / g_pf.uf(g_lastTime, t8)
, (g_sumKeyLen + g_sumValueLen - g_sumEntryNum * 8) / g_pf.uf(g_lastTime, t8)
);
  return s;
}

#if defined(TerocksPrivateCode)

Status TerarkZipTableBuilder::WriteSSTFileMulti(long long t3, long long t4
  , fstring tmpIndexFile
  , fstring tmpStoreFile
  , fstring tmpDictFile
  , const DictZipBlobStore::ZipStat& dzstat) {
  assert(histogram_.size() > 1);
  terark::MmapWholeFile dictMmap;
  BlobStore::Dictionary dict;
  if (!tmpDictFile.empty()) {
    terark::MmapWholeFile(tmpDictFile.c_str()).swap(dictMmap);
    dict = BlobStore::Dictionary(fstring{(const char*)dictMmap.base,
        (ptrdiff_t)dictMmap.size});
  }
  const size_t realsampleLenSum = dict.memory.size();
  long long rawBytes = properties_.raw_key_size + properties_.raw_value_size;
  long long t5 = g_pf.now();
  terark::MmapWholeFile mmapIndexFile(tmpIndexFile.c_str());
  terark::MmapWholeFile mmapStoreFile(tmpStoreFile.c_str());
  assert(mmapIndexFile.base != nullptr);
  assert(mmapStoreFile.base != nullptr);
  Status s;
  BlockHandle dataBlock, dictBlock, indexBlock, zvTypeBlock, offsetBlock, tombstoneBlock(0, 0);
  BlockHandle commonPrefixBlock;
  TerarkZipMultiOffsetInfo offsetInfo;
  offsetInfo.Init(key_prefixLen_, histogram_.size());
  size_t typeSize = 0;
  size_t commonPrefixLenSize = 0;
  for (size_t i = 0; i < histogram_.size(); ++i) {
    auto& kvs = histogram_[i];
    typeSize += kvs.type.mem_size();
    commonPrefixLenSize += kvs.stat.commonPrefixLen;
  }
  {
    size_t real_size = TerarkZipMultiOffsetInfo::calc_size(key_prefixLen_, histogram_.size())
      + mmapIndexFile.size
      + mmapStoreFile.size
      + typeSize
      + terark::align_up(commonPrefixLenSize, 16);
    size_t block_size, last_allocated_block;
    file_->writable_file()->GetPreallocationStatus(&block_size, &last_allocated_block);
    INFO(ioptions_.info_log
      , "TerarkZipTableBuilder::Finish():this=%012p: old prealloc_size = %zd, real_size = %zd\n"
      , this, block_size, real_size
    );
    file_->writable_file()->SetPreallocationBlockSize((1ull << 20) + real_size);
  }
  long long t6 = t5, t7 = t5;
  offset_ = 0;
  auto getMmapPart = [](terark::MmapWholeFile& mmap, size_t beg, size_t end) {
    assert(beg <= end);
    assert(end <= mmap.size);
    return fstring((const char*)mmap.base + beg, (const char*)mmap.base + end);
  };
  valvec<byte_t> commonPrefix;
  size_t sumKeyLen = 0;
  size_t numKeys = 0;
  size_t keyOffset = 0;
  size_t valueOffset = 0;
  typeSize = 0;
  commonPrefixLenSize = 0;
  commonPrefix.reserve(terark::align_up(commonPrefixLenSize, 16));
  for (size_t i = 0; i < histogram_.size(); ++i) {
    auto& kvs = histogram_[isReverseBytewiseOrder_ ? histogram_.size() - 1 - i : i];
    sumKeyLen += kvs.stat.sumKeyLen;
    numKeys += kvs.stat.numKeys;
    commonPrefix.append(fstring(kvs.stat.minKey).substr(0, kvs.stat.commonPrefixLen));
    unique_ptr<TerarkIndex> index(TerarkIndex::LoadMemory(getMmapPart(mmapIndexFile,
      kvs.keyFileBegin, kvs.keyFileEnd)));
    unique_ptr<BlobStore> store(BlobStore::load_from_user_memory(getMmapPart(mmapStoreFile,
      kvs.valueFileBegin, kvs.valueFileEnd), dict));
    assert(index->NumKeys() == kvs.stat.numKeys);
    long long t6p, t7p;
    s = WriteStore(index.get(), store.get(), kvs, dataBlock, t7, t6p, t7p);
    if (!s.ok()) {
      return s;
    }
    t6 += t6p - t7;
    t7 = t7p;
    keyOffset += kvs.keyFileEnd - kvs.keyFileBegin;
    valueOffset = offset_;
    typeSize += kvs.type.mem_size();
    commonPrefixLenSize += kvs.stat.commonPrefixLen;
    offsetInfo.set(i, kvs.prefix, keyOffset, valueOffset, typeSize, commonPrefixLenSize);
  }
  commonPrefix.resize(terark::align_up(commonPrefixLenSize, 16), 0);
  WriteBlock(commonPrefix, file_, &offset_, &commonPrefixBlock);
  properties_.data_size = dataBlock.size();
  if (!dict.memory.empty()) {
    s = WriteBlock(dict.memory, file_, &offset_, &dictBlock);
    if (!s.ok()) {
      return s;
    }
  }
  try {
    indexBlock.set_offset(offset_);
    indexBlock.set_size(mmapIndexFile.size);
    if (isReverseBytewiseOrder_) {
      for (size_t i = histogram_.size(); i > 0; ) {
        auto& kvs = histogram_[--i];
        DoWriteAppend((const char*)mmapIndexFile.base + kvs.keyFileBegin,
          kvs.keyFileEnd - kvs.keyFileBegin);
      }
      assert(offset_ == indexBlock.offset() + indexBlock.size());
    }
    else {
      DoWriteAppend(mmapIndexFile.base, mmapIndexFile.size);
    }
    zvTypeBlock.set_offset(offset_);
    zvTypeBlock.set_size(typeSize);
    if (isReverseBytewiseOrder_) {
      for (size_t i = histogram_.size(); i > 0; ) {
        auto& kvs = histogram_[--i];
        DoWriteAppend(kvs.type.data(), kvs.type.mem_size());
      }
    }
    else {
      for (size_t i = 0; i < histogram_.size(); ++i) {
        auto& kvs = histogram_[i];
        DoWriteAppend(kvs.type.data(), kvs.type.mem_size());
      }
    }
    assert(offset_ == zvTypeBlock.offset() + zvTypeBlock.size());
  }
  catch (const Status& s) {
    return s;
  }
  s = WriteBlock(offsetInfo.dump(), file_, &offset_, &offsetBlock);
  if (!s.ok()) {
    return s;
  }
  auto table_factory = dynamic_cast<TerarkZipTableFactory*>(ioptions_.table_factory);
  assert(table_factory);
  auto& license = table_factory->GetLicense();
  BlockHandle licenseHandle;
  s = WriteBlock(SliceOf(license.dump()), file_, &offset_, &licenseHandle);
  if (!s.ok()) {
    return s;
  }
  if (!range_del_block_.empty()) {
    s = WriteBlock(range_del_block_.Finish(), file_, &offset_, &tombstoneBlock);
    if (!s.ok()) {
      return s;
    }
  }
  range_del_block_.Reset();
  properties_.index_size = indexBlock.size();
  WriteMetaData({
    {&kTerarkZipTableExtendedBlock                                , licenseHandle     },
    {!dict.memory.empty() ? &kTerarkZipTableValueDictBlock : NULL , dictBlock         },
    {&kTerarkZipTableIndexBlock                                   , indexBlock        },
    {!zvTypeBlock.IsNull() ? &kTerarkZipTableValueTypeBlock : NULL, zvTypeBlock       },
    {&kTerarkZipTableOffsetBlock                                  , offsetBlock       },                     
    {&kTerarkZipTableCommonPrefixBlock                            , commonPrefixBlock },
    {!tombstoneBlock.IsNull() ? &kRangeDelBlock : NULL            , tombstoneBlock    },
  });
  long long t8 = g_pf.now();
  {
    std::unique_lock<std::mutex> lock(g_sumMutex);
    g_sumKeyLen += properties_.raw_key_size;
    g_sumValueLen += properties_.raw_value_size;
    g_sumUserKeyLen += sumKeyLen;
    g_sumUserKeyNum += numKeys;
    g_sumEntryNum += properties_.num_entries;
  }
  INFO(ioptions_.info_log,
    "TerarkZipTableBuilder::FinishMulti():this=%012p: second pass time =%8.2f's,%8.3f'MB/sec, value only(%4.1f%% of KV)\n"
    "   wait indexing time = %7.2f's,\n"
    "  remap KeyValue time = %7.2f's, %8.3f'MB/sec (all stages of remap)\n"
    "    Get OrderMap time = %7.2f's, %8.3f'MB/sec (index lex order gen)\n"
    "  rebuild zvType time = %7.2f's, %8.3f'MB/sec\n"
    "  write SST data time = %7.2f's, %8.3f'MB/sec\n"
    "    z-dict build time = %7.2f's, sample length = %7.3f'MB, throughput = %6.3f'MB/sec\n"
    "    zip my value time = %7.2f's, unzip  length = %7.3f'GB\n"
    "    zip my value throughput = %7.3f'MB/sec\n"
    "    zip pipeline throughput = %7.3f'MB/sec\n"
    "    entries = %zd  avg-key = %.2f  avg-zkey = %.2f  avg-val = %.2f  avg-zval = %.2f\n"
    "    usrkeys = %zd  avg-key = %.2f  avg-zkey = %.2f  avg-val = %.2f  avg-zval = %.2f\n"
    "    UnZipSize{ index =%9.4f GB  value =%9.4f GB   all =%9.4f GB }\n"
    "    __ZipSize{ index =%9.4f GB  value =%9.4f GB   all =%9.4f GB }\n"
    "    UnZip/Zip{ index =%9.4f     value =%9.4f      all =%9.4f    }\n"
    "    Zip/UnZip{ index =%9.4f     value =%9.4f      all =%9.4f    }\n"
    "----------------------------\n"
    "    total value len =%14.6f GB     avg =%8.3f KB (by entry num)\n"
    "    total  key  len =%14.6f GB     avg =%8.3f KB\n"
    "    total ukey  len =%14.6f GB     avg =%8.3f KB\n"
    "    total ukey  num =%17.9f Billion\n"
    "    total entry num =%17.9f Billion\n"
    "    write speed all =%17.9f MB/sec (with    version num)\n"
    "    write speed all =%17.9f MB/sec (without version num)"
, this, g_pf.sf(t3, t4)
, properties_.raw_value_size*1.0 / g_pf.uf(t3, t4)
, properties_.raw_value_size*100.0 / rawBytes

, g_pf.sf(t4, t5) // wait indexing time
, g_pf.sf(t5, t8), double(offset_) / g_pf.uf(t5, t8)

, g_pf.sf(t5, t6), properties_.index_size / g_pf.uf(t5, t6) // index lex walk

, g_pf.sf(t6, t7), numKeys * 2 / 8 / (g_pf.uf(t6, t7) + 1.0) // rebuild zvType

, g_pf.sf(t7, t8), double(offset_) / g_pf.uf(t7, t8) // write SST data

, dzstat.dictBuildTime, realsampleLenSum / 1e6
, realsampleLenSum / dzstat.dictBuildTime / 1e6

, dzstat.dictZipTime, properties_.raw_value_size / 1e9
, properties_.raw_value_size / dzstat.dictZipTime / 1e6
, dzstat.pipelineThroughBytes / dzstat.dictZipTime / 1e6

, size_t(properties_.num_entries)
, double(properties_.raw_key_size)   / properties_.num_entries
, double(properties_.index_size)     / properties_.num_entries
, double(properties_.raw_value_size) / properties_.num_entries
, double(properties_.data_size)      / properties_.num_entries

, numKeys
, double(sumKeyLen)                  / numKeys
, double(properties_.index_size)     / numKeys
, double(properties_.raw_value_size) / numKeys
, double(properties_.data_size)      / numKeys

, sumKeyLen / 1e9, properties_.raw_value_size / 1e9, rawBytes / 1e9

, properties_.index_size / 1e9, properties_.data_size / 1e9, offset_ / 1e9

, double(sumKeyLen) / properties_.index_size
, double(properties_.raw_value_size) / properties_.data_size
, double(rawBytes) / offset_

, properties_.index_size / double(sumKeyLen)
, properties_.data_size / double(properties_.raw_value_size)
, offset_ / double(rawBytes)

, g_sumValueLen / 1e9, g_sumValueLen / 1e3 / g_sumEntryNum
, g_sumKeyLen / 1e9, g_sumKeyLen / 1e3 / g_sumEntryNum
, g_sumUserKeyLen / 1e9, g_sumUserKeyLen / 1e3 / g_sumUserKeyNum
, g_sumUserKeyNum / 1e9
, g_sumEntryNum / 1e9
, (g_sumKeyLen + g_sumValueLen) / g_pf.uf(g_lastTime, t8)
, (g_sumKeyLen + g_sumValueLen - g_sumEntryNum * 8) / g_pf.uf(g_lastTime, t8)
);
  return s;
}

#endif // TerocksPrivateCode

Status TerarkZipTableBuilder::WriteMetaData(std::initializer_list<std::pair<const std::string*, BlockHandle> > blocks) {
  MetaIndexBuilder metaindexBuiler;
  for (const auto& block : blocks) {
    if (block.first) {
      metaindexBuiler.Add(*block.first, block.second);
    }
  }
  PropertyBlockBuilder propBlockBuilder;
  propBlockBuilder.AddTableProperty(properties_);
  NotifyCollectTableCollectorsOnFinish(collectors_,
    ioptions_.info_log,
    &propBlockBuilder);
  BlockHandle propBlock, metaindexBlock;
  Status s = WriteBlock(propBlockBuilder.Finish(), file_, &offset_, &propBlock);
  if (!s.ok()) {
    return s;
  }
  metaindexBuiler.Add(kPropertiesBlock, propBlock);
  s = WriteBlock(metaindexBuiler.Finish(), file_, &offset_, &metaindexBlock);
  if (!s.ok()) {
    return s;
  }
  Footer footer(kTerarkZipTableMagicNumber, 0);
  footer.set_metaindex_handle(metaindexBlock);
  footer.set_index_handle(BlockHandle::NullBlockHandle());
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  s = file_->Append(footer_encoding);
  if (s.ok()) {
    offset_ += footer_encoding.size();
  }
  return s;
}

Status TerarkZipTableBuilder::OfflineFinish() {
  std::unique_ptr<DictZipBlobStore> zstore(zbuilder_->finish(
    DictZipBlobStore::ZipBuilder::FinishFreeDict));
  fstring fpath = zstore->get_fpath();
  std::string tmpStoreFile(fpath.data(), fpath.size());
  auto& kvs = histogram_[0];
  auto dzstat = zbuilder_->getZipStat();
  valvec<byte_t> commonPrefix(prevUserKey_.data(), kvs.stat.commonPrefixLen);
  AutoDeleteFile tmpIndexFile{tmpValueFile_.path + ".index"};
  AutoDeleteFile tmpDictFile{tmpValueFile_.path + ".dict"};
  fstring dict = zstore->get_dict().memory;
  FileStream(tmpDictFile, "wb+").ensureWrite(dict.data(), dict.size());
  zstore.reset();
  zbuilder_.reset();
  long long t1 = g_pf.now();
  {
    auto factory = TerarkIndex::GetFactory(table_options_.indexType);
    if (!factory) {
      THROW_STD(invalid_argument,
        "invalid indexType: %s", table_options_.indexType.c_str());
    }
    NativeDataInput<InputBuffer> tempKeyFileReader(&tmpKeyFile_.fp);
    FileStream writer(tmpIndexFile, "wb+");
    factory->Build(tempKeyFileReader, table_options_,
      [&writer](const void* data, size_t size) {
        writer.ensureWrite(data, size);
      },
      kvs.stat);
  }
  long long tt = g_pf.now();
  INFO(ioptions_.info_log
    , "TerarkZipTableBuilder::Finish():this=%012p:  index pass time =%8.2f's,%8.3f'MB/sec\n"
    , this, g_pf.sf(t1, tt), properties_.raw_key_size*1.0 / g_pf.uf(t1, tt)
  );
  return WriteSSTFile(t1, tt, tmpIndexFile, tmpStoreFile, tmpDictFile, dzstat);
}

void TerarkZipTableBuilder::Abandon() {
  closed_ = true;
  tmpKeyFile_.complete_write();
  tmpValueFile_.complete_write();
  tmpSampleFile_.complete_write();
  zbuilder_.reset();
  tmpZipDictFile_.Delete();
  tmpZipValueFile_.Delete();
}

void TerarkZipTableBuilder::AddPrevUserKey(bool finish) {
  UpdateValueLenHistogram(); // will use valueBuf_
  if (zbuilder_) {
    OfflineZipValueData(); // will change valueBuf_
  }
  valueBuf_.erase_all();
  auto& currentHistogram = histogram_.back();
  currentHistogram.key[prevUserKey_.size()]++;
  tmpKeyFile_.writer << prevUserKey_;
  valueBits_.push_back(false);
  currentHistogram.stat.sumKeyLen += prevUserKey_.size();
  currentHistogram.stat.numKeys++;
  if (finish) {
    currentHistogram.stat.maxKey.assign(prevUserKey_);
  }
}

void TerarkZipTableBuilder::OfflineZipValueData() {
  uint64_t seq, seqType = *(uint64_t*)valueBuf_.strpool.data();
  auto& bzvType = histogram_[0].type;
  ValueType type;
  UnPackSequenceAndType(seqType, &seq, &type);
  const size_t vNum = valueBuf_.size();
  if (vNum == 1 && (kTypeDeletion == type || kTypeValue == type)) {
    if (0 == seq && kTypeValue == type) {
      bzvType.push_back(byte_t(ZipValueType::kZeroSeq));
      zbuilder_->addRecord(fstring(valueBuf_.strpool).substr(8));
    }
    else {
      if (kTypeValue == type) {
        bzvType.push_back(byte_t(ZipValueType::kValue));
      }
      else {
        bzvType.push_back(byte_t(ZipValueType::kDelete));
      }
      // use Little Endian upper 7 bytes
      *(uint64_t*)valueBuf_.strpool.data() <<= 8;
      zbuilder_->addRecord(fstring(valueBuf_.strpool).substr(1));
    }
  }
  else {
    bzvType.push_back(byte_t(ZipValueType::kMulti));
    size_t valueBytes = valueBuf_.strpool.size();
    size_t headerSize = ZipValueMultiValue::calcHeaderSize(vNum);
    valueBuf_.strpool.grow_no_init(headerSize);
    char* pData = valueBuf_.strpool.data();
    memmove(pData + headerSize, pData, valueBytes);
    auto multiVals = (ZipValueMultiValue*)pData;
    multiVals->offsets[0] = uint32_t(vNum);
    for (size_t i = 1; i < vNum; ++i) {
      multiVals->offsets[i] = uint32_t(valueBuf_.offsets[i]);
    }
    zbuilder_->addRecord(valueBuf_.strpool);
  }
}

void TerarkZipTableBuilder::UpdateValueLenHistogram() {
  uint64_t seq, seqType = *(uint64_t*)valueBuf_.strpool.data();
  ValueType type;
  UnPackSequenceAndType(seqType, &seq, &type);
  const size_t vNum = valueBuf_.size();
  size_t valueLen = 0;
  if (vNum == 1 && (kTypeDeletion == type || kTypeValue == type)) {
    if (0 == seq && kTypeValue == type) {
      valueLen = valueBuf_.strpool.size() - 8;
      ++zeroSeqCount_;
    }
    else {
      valueLen = valueBuf_.strpool.size() - 1;
    }
  }
  else {
    valueLen = valueBuf_.strpool.size() + sizeof(uint32_t)*vNum;
  }
  histogram_.back().value[valueLen]++;
}


TableBuilder*
createTerarkZipTableBuilder(const TerarkZipTableOptions& tzo,
                            const TableBuilderOptions&   tbo,
                            uint32_t                     column_family_id,
                            WritableFileWriter*          file,
                            size_t                       key_prefixLen)
{
    return new TerarkZipTableBuilder(tzo, tbo, column_family_id, file, key_prefixLen);
}

} // namespace
