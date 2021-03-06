//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// btree_index.cpp
//
// Identification: src/backend/index/btree_index.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "backend/common/logger.h"
#include "backend/index/bwtree_index.h"
#include "backend/index/index_key.h"
#include "backend/storage/tuple.h"

// add lock to debug
// #define LOCK_DEBUG
// #define MY_DEBUG

namespace peloton {
namespace index {

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
BWTreeIndex<KeyType, ValueType, KeyComparator, KeyEqualityChecker>::BWTreeIndex(
    IndexMetadata *metadata)
    : Index(metadata),
      container(KeyComparator(metadata), KeyEqualityChecker(metadata),
                metadata),
      equals(metadata),
      comparator(metadata) {}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
BWTreeIndex<KeyType, ValueType, KeyComparator,
            KeyEqualityChecker>::~BWTreeIndex() {}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
bool BWTreeIndex<KeyType, ValueType, KeyComparator,
                 KeyEqualityChecker>::InsertEntry(__attribute__((unused))
                                                  const storage::Tuple *key,
                                                  __attribute__((unused))
                                                  const ItemPointer location) {
  LOG_INFO("Entering InsertEntry");

#ifdef MY_DEBUG
//  printf("I-start\n");
#endif

#ifdef LOCK_DEBUG
  index_lock.WriteLock();
#endif

  KeyType index_key;
  index_key.SetFromKey(key);

  auto key_pair = std::pair<KeyType, ValueType>(index_key, location);

  bool ret = container.insert_entry(key_pair.first, key_pair.second);

#ifdef LOCK_DEBUG
  index_lock.Unlock();
#endif
  LOG_INFO("Leaving InsertEntry");

#ifdef MY_DEBUG
  printf("I-end\n");
#endif
  return ret;
}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
bool BWTreeIndex<KeyType, ValueType, KeyComparator,
                 KeyEqualityChecker>::DeleteEntry(__attribute__((unused))
                                                  const storage::Tuple *key,
                                                  __attribute__((unused))
                                                  const ItemPointer location) {
  LOG_INFO("Entering DeleteEntry");
#ifdef MY_DEBUG
  printf("D-start\n");
#endif

#ifdef LOCK_DEBUG
  index_lock.WriteLock();
#endif

  KeyType index_key;
  index_key.SetFromKey(key);

  bool ret = container.delete_entry(index_key, location);

#ifdef MY_DEBUG
  container.print_info(0);
#endif

#ifdef LOCK_DEBUG
  index_lock.Unlock();
#endif

#ifdef MY_DEBUG
  printf("D-end\n");
#endif
  LOG_INFO("Leaving DeleteEntry");
  return ret;
}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
std::vector<ItemPointer>
BWTreeIndex<KeyType, ValueType, KeyComparator, KeyEqualityChecker>::Scan(
    __attribute__((unused)) const std::vector<Value> &values,
    __attribute__((unused)) const std::vector<oid_t> &key_column_ids,
    __attribute__((unused)) const std::vector<ExpressionType> &expr_types,
    __attribute__((unused)) const ScanDirectionType &scan_direction) {
#ifdef MY_DEBUG
  printf("Scan-start\n");
#endif

  std::vector<KeyType> keys_result;
  std::vector<std::vector<ItemPointer>> values_result;
  std::vector<ItemPointer> result;

#ifdef LOCK_DEBUG
  index_lock.ReadLock();
#endif
  {
    switch (scan_direction) {
      case SCAN_DIRECTION_TYPE_FORWARD:
      case SCAN_DIRECTION_TYPE_BACKWARD: {
        container.scan(keys_result, values_result);

        unsigned long vector_size = values_result.size();
        for (int i = 0; i < vector_size; i++) {
          auto tuple =
              keys_result[i].GetTupleForComparison(metadata->GetKeySchema());

          // Compare the current key in the scan with "values" based on
          // "expression types"
          // For instance, "5" EXPR_GREATER_THAN "2" is true
          if (Compare(tuple, key_column_ids, expr_types, values) == true) {
            result.insert(result.end(), values_result[i].begin(),
                          values_result[i].end());
          }
        }
      } break;
      case SCAN_DIRECTION_TYPE_INVALID:
      default:
        throw Exception("Invalid scan direction \n");
        break;
    }
  }
#ifdef LOCK_DEBUG
  index_lock.Unlock();
#endif

#ifdef MY_DEBUG
  printf("Scan-end\n");
#endif
  return result;
}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
std::vector<ItemPointer> BWTreeIndex<KeyType, ValueType, KeyComparator,
                                     KeyEqualityChecker>::ScanAllKeys() {
  LOG_INFO("Entering ScanAllKeys");
#ifdef MY_DEBUG
  printf("ScanAll-start\n");
#endif
  std::vector<ItemPointer> result;

#ifdef LOCK_DEBUG
  index_lock.ReadLock();
#endif

  container.scan_all(result);

#ifdef LOCK_DEBUG
  index_lock.Unlock();
#endif

#ifdef MY_DEBUG
  printf("ScanAll-end\n");
#endif
  return result;
}

/**
 * @brief Return all locations related to this key.
 */
template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
std::vector<ItemPointer>
BWTreeIndex<KeyType, ValueType, KeyComparator, KeyEqualityChecker>::ScanKey(
    __attribute__((unused)) const storage::Tuple *key) {
  LOG_INFO("Entering ScanKey");

#ifdef MY_DEBUG
  printf("ScanKey-start\n");
#endif

#ifdef LOCK_DEBUG
  index_lock.ReadLock();
#endif
  std::vector<ItemPointer> result;
  KeyType index_key;
  index_key.SetFromKey(key);

  container.get_value(index_key, result);
#ifdef LOCK_DEBUG
  index_lock.Unlock();
#endif

#ifdef MY_DEBUG
  printf("ScanKey-end\n");
#endif
  return result;
}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
std::string BWTreeIndex<KeyType, ValueType, KeyComparator,
                        KeyEqualityChecker>::GetTypeName() const {
  return "BWTree";
}

template <typename KeyType, typename ValueType, class KeyComparator,
          class KeyEqualityChecker>
size_t BWTreeIndex<KeyType, ValueType, KeyComparator,
                   KeyEqualityChecker>::GetMemoryFootprint() {
  return 0;
}

// Explicit template instantiation
template class BWTreeIndex<IntsKey<1>, ItemPointer, IntsComparator<1>,
                           IntsEqualityChecker<1>>;
template class BWTreeIndex<IntsKey<2>, ItemPointer, IntsComparator<2>,
                           IntsEqualityChecker<2>>;
template class BWTreeIndex<IntsKey<3>, ItemPointer, IntsComparator<3>,
                           IntsEqualityChecker<3>>;
template class BWTreeIndex<IntsKey<4>, ItemPointer, IntsComparator<4>,
                           IntsEqualityChecker<4>>;

template class BWTreeIndex<GenericKey<4>, ItemPointer, GenericComparator<4>,
                           GenericEqualityChecker<4>>;
template class BWTreeIndex<GenericKey<8>, ItemPointer, GenericComparator<8>,
                           GenericEqualityChecker<8>>;
template class BWTreeIndex<GenericKey<12>, ItemPointer, GenericComparator<12>,
                           GenericEqualityChecker<12>>;
template class BWTreeIndex<GenericKey<16>, ItemPointer, GenericComparator<16>,
                           GenericEqualityChecker<16>>;
template class BWTreeIndex<GenericKey<24>, ItemPointer, GenericComparator<24>,
                           GenericEqualityChecker<24>>;
template class BWTreeIndex<GenericKey<32>, ItemPointer, GenericComparator<32>,
                           GenericEqualityChecker<32>>;
template class BWTreeIndex<GenericKey<48>, ItemPointer, GenericComparator<48>,
                           GenericEqualityChecker<48>>;
template class BWTreeIndex<GenericKey<64>, ItemPointer, GenericComparator<64>,
                           GenericEqualityChecker<64>>;
template class BWTreeIndex<GenericKey<96>, ItemPointer, GenericComparator<96>,
                           GenericEqualityChecker<96>>;
template class BWTreeIndex<GenericKey<128>, ItemPointer, GenericComparator<128>,
                           GenericEqualityChecker<128>>;
template class BWTreeIndex<GenericKey<256>, ItemPointer, GenericComparator<256>,
                           GenericEqualityChecker<256>>;
template class BWTreeIndex<GenericKey<512>, ItemPointer, GenericComparator<512>,
                           GenericEqualityChecker<512>>;

template class BWTreeIndex<TupleKey, ItemPointer, TupleKeyComparator,
                           TupleKeyEqualityChecker>;

}  // End index namespace
}  // End peloton namespace
