//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// BWTree.h
//
// Identification: src/backend/index/BWTree.h
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>
#include <assert.h>
#include <atomic>
#include <unordered_map>
#include <stack>
#include <vector>
#include <unordered_set>
#include "../common/types.h"
#include "../storage/tuple.h"
#include "index.h"

// macro for debug
//#define MY_PRINT_DEBUG
//#define LEI_PRINT_DEBUG
#define TURN_ON_CONSOLIDATE
//#define BINARY_SEARCH

// in bytes
#define BWTREE_NODE_SIZE 256

#define BWTREE_MAX(a, b) ((a) < (b) ? (b) : (a))

#define MAX_DELTA_CHAIN_LEN 8

// in bits
#define MAPPING_TABLE_SIZE_BITNUM 10
#define MAPPING_TABLE_SIZE (1 << (MAPPING_TABLE_SIZE_BITNUM))

#define GET_TIER1_INDEX(pid) ((pid) >> 10)
#define GET_TIER2_INDEX(pid) ((pid)&0x3ff)

#define NULL_PID -1

namespace std {
struct HashPair {
  size_t operator()(peloton::ItemPointer const& ptr) const {
    using std::hash;
    return hash<peloton::oid_t>()(ptr.block) ^
           hash<peloton::oid_t>()(ptr.offset);
  }
};
}

namespace peloton {
namespace index {

typedef long long PidType;

enum NodeType {
  LEAF = 0,
  INNER = 1,
  RECORD_DELTA = 2,
  INDEX_ENTRY_DELTA = 3,
  REMOVE_NODE_DELTA = 4,
  MERGE_DELTA = 5,
  DELETE_INDEX_TERM_DELTA = 6,
  SPLIT_DELTA = 7
};

class ItemPointerEqualityChecker {
 public:
  inline bool operator()(const ItemPointer& lhs, const ItemPointer& rhs) const {
    return (lhs.block == rhs.block) && (lhs.offset == rhs.offset);
  }
};

class ItemPointerComparator {
 public:
  inline bool operator()(const ItemPointer& lhs, const ItemPointer& rhs) const {
    return (lhs.block < rhs.block) ||
           ((lhs.block == rhs.block) && (lhs.offset < rhs.offset));
  }
};

// Look up the stx btree interface for background.
// peloton/third_party/stx/btree.h
template <typename KeyType, typename ValueType, class KeyComparator,
          typename KeyEqualityChecker>
class BWTree {
 public:
  // *** Constructed Types

  // Typedef of our own type
  typedef BWTree<KeyType, ValueType, KeyComparator, KeyEqualityChecker>
      BwTreeSelf;

  // The unordered_set that will be used for recording
  // duplicated keys
  typedef std::unordered_set<ValueType, std::HashPair,
                             ItemPointerEqualityChecker> DelSet;

 public:
  // *** Static Constant Options and Values of the Bw Tree
  // Base B2 tree parameter: The number of key/data slots in each leaf
  static const unsigned short leafslotmax =
      BWTREE_MAX(8, BWTREE_NODE_SIZE / (sizeof(KeyType) + sizeof(ValueType)));

  // Base B+ tree parameter: The number of key slots in each inner node,
  // this can differ from slots in each leaf.
  static const unsigned short innerslotmax =
      BWTREE_MAX(8, BWTREE_NODE_SIZE / (sizeof(KeyType) + sizeof(PidType)));

  // Computed B+ tree parameter: The minimum number of key/data slots used
  // in a leaf. If fewer slots are used, the leaf will be merged or slots
  // shifted from it's siblings.
  static const unsigned short minleafslots = (leafslotmax / 2);

  // Computed B+ tree parameter: The minimum number of key slots used
  // in an inner node. If fewer slots are used, the inner node will be
  // merged or slots shifted from it's siblings.
  static const unsigned short mininnerslots = (innerslotmax / 2);

  struct Node;
  struct MappingTable {
   private:
    // The first level mapping table
    Node** mappingtable_1[MAPPING_TABLE_SIZE];
    std::atomic<unsigned long> nextPid;

   public:
    MappingTable() {
      for (int i = 0; i < MAPPING_TABLE_SIZE; i++) {
        mappingtable_1[i] = nullptr;
      }
      nextPid = 0;
    }

    bool delete_chain(Node* node) {
      Node* next = node;
      while (next) {
        node = next;
        if (node->node_type == MERGE_DELTA) {
          PidType temp = ((MergeDelta*)node)->orignal_node->pid;
          delete_chain(this->get(temp));
        }
        next = node->next;
        delete node;
      }
      return true;
    }

    ~MappingTable() {
      for (int i = 0; i < MAPPING_TABLE_SIZE; i++) {
        if (mappingtable_1[i] != nullptr) {
          for (int j = 0; j < MAPPING_TABLE_SIZE; j++) {
            if (mappingtable_1[i][j] != nullptr)
              delete_chain(mappingtable_1[i][j]);
          }
          delete[] mappingtable_1[i];
          mappingtable_1[i] = nullptr;
        }
      }
    }

    Node* get(PidType pid) {
      if (pid == NULL_PID) return nullptr;

      long tier1_idx = GET_TIER1_INDEX(pid);
      long tier2_idx = GET_TIER2_INDEX(pid);

      if (mappingtable_1[tier1_idx] == nullptr) return nullptr;

      return mappingtable_1[tier1_idx][tier2_idx];
    }

    bool set(PidType pid, Node* expected, Node* addr) {
      if (addr != nullptr) addr->pid = pid;

      long tier1_idx = GET_TIER1_INDEX(pid);
      long tier2_idx = GET_TIER2_INDEX(pid);

      if (mappingtable_1[tier1_idx] == nullptr) {
        LOG_ERROR("meet a null level1 idx");
        return false;
      }

      // using cas to set the value in mapping table
      return std::atomic_compare_exchange_strong(
          (std::atomic<Node*>*)&mappingtable_1[tier1_idx][tier2_idx], &expected,
          addr);
    }

    // if correctly add this new addr, return the new pid,
    // else return NULL_PID
    long add(Node* addr) {
      unsigned long new_pid = nextPid++;

      if (addr != nullptr) addr->pid = new_pid;

      long tier1_idx = GET_TIER1_INDEX(new_pid);
      long tier2_idx = GET_TIER2_INDEX(new_pid);

      Node** expectded = mappingtable_1[tier1_idx];

      // atomically add new secondary index
      if (mappingtable_1[tier1_idx] == nullptr) {
        Node** desired = new Node* [MAPPING_TABLE_SIZE];
        for (int i = 0; i < MAPPING_TABLE_SIZE; i++) {
          desired[i] = nullptr;
        }
        if (!std::atomic_compare_exchange_strong(
                (std::atomic<Node**>*)&mappingtable_1[tier1_idx], &expectded,
                desired)) {
          delete[] desired;
        }
      }

      Node* expectded2 = mappingtable_1[tier1_idx][tier2_idx];
      // atomically add addr to desired secondary index
      if (std::atomic_compare_exchange_strong(
              (std::atomic<Node*>*)&mappingtable_1[tier1_idx][tier2_idx],
              &expectded2, addr)) {
        return new_pid;
      } else {
        LOG_ERROR("add pid fail!!!!\n\n\n");
        return NULL_PID;
      }
    }

    void remove(PidType pid) {
      long tier1_idx = GET_TIER1_INDEX(pid);
      long tier2_idx = GET_TIER2_INDEX(pid);

      mappingtable_1[tier1_idx][tier2_idx] = nullptr;
    }
  };

  /**
   * The Node inheritance hierarchy
   * **/
  struct Node {
    // Delta chain next pointer
    Node* next;

    // type of this node
    NodeType node_type;

    // Length of current delta chain
    size_t delta_list_len;

    // reference to outer mapping table
    MappingTable& mapping_table;

    // linked list pointers to traverse the leaves
    PidType next_leafnode;

    // flag to indicate whether this chain belongs to a leaf node
    bool is_leaf;

    // minimal and maximal key in this node
    KeyType low_key, high_key;

    // check if lowkey == -inf, highkey == +inf
    bool inf_lowkey, inf_highkey;

    // Number of key slotuse use, so number of valid children or data
    // pointers
    unsigned short slotuse;

    // Node pid
    PidType pid;

    // constructor
    Node(Node* ne, NodeType ntype, size_t delta_l, MappingTable& mt,
         PidType next_leaf, bool is_lf, KeyType lowkey, KeyType highkey,
         bool inf_low, bool inf_high)
        : next(ne),
          node_type(ntype),
          delta_list_len(delta_l),
          mapping_table(mt),
          next_leafnode(next_leaf),
          is_leaf(is_lf),
          low_key(lowkey),
          high_key(highkey),
          inf_lowkey(inf_low),
          inf_highkey(inf_high) {
      slotuse = 0;
    }

    virtual ~Node() {}

    // True if this is a leaf node
    inline bool is_leaf_node() const { return (node_type == NodeType::LEAF); }

    inline bool need_split() const {
      if (is_leaf) {
        return slotuse >= leafslotmax;
      } else {
        return slotuse >= innerslotmax;
      }
    }

    inline bool need_merge() const {
      if (is_leaf) {
        return slotuse <= minleafslots;
      } else {
        return slotuse <= mininnerslots;
      }
    }
  };

 private:
  // We need a root node
  PidType root;

  // the mapping table used in our BWTree
  MappingTable mapping_table;

  // another mapping table used for garbage collection
  MappingTable garbage_table;

  // Pointer to first leaf in the double linked leaf chain
  PidType headleaf;

 private:
  // Extended structure of a inner node in-memory. Contains only keys and no
  // data items.
  struct InnerNode : public Node {
    // Keys of children or data pointers,
    //  we plus one so as to avoid overflow when consolidation
    KeyType slotkey[innerslotmax + 1];

    // Pointers(PIDs) to children,
    //  we plus one so as to avoid overflow when consolidation
    PidType childid[innerslotmax + 1 + 1];

    // Constructor
    InnerNode(MappingTable& mapping_table, KeyType lowkey, KeyType highkey,
              bool inf_low, bool inf_high)
        : Node(nullptr, NodeType::INNER, 0, mapping_table, NULL_PID, false,
               lowkey, highkey, inf_low, inf_high) {}
  };

  // Extended structure of a leaf node in memory. Contains pairs of keys and
  // data items. Key and data slots are kept in separate arrays, because the
  // key array is traversed very often compared to accessing the data items.
  struct LeafNode : public Node {
    // Keys of children or data pointers
    //  we plus one so as to avoid overflow when consolidation
    KeyType slotkey[leafslotmax + 1];

    // Array of data, each bucket points to an vector of actual data
    //  we plus one so as to avoid overflow when consolidation
    std::vector<ValueType>* slotdata[leafslotmax + 1];

    // Constructor
    LeafNode(MappingTable& mapping_table, PidType next_leafnode, KeyType lowkey,
             KeyType highkey, bool inf_low, bool inf_high)
        : Node(nullptr, NodeType::LEAF, 0, mapping_table, next_leafnode, true,
               lowkey, highkey, inf_low, inf_high) {
      // initialize the value bucket
      for (int i = 0; i < leafslotmax + 1; i++) {
        slotdata[i] = nullptr;
      }
    }

    // Destructor
    ~LeafNode() {
      for (int i = 0; i < leafslotmax + 1; i++) {
        if (slotdata[i] != nullptr) delete slotdata[i];
      }
    }
  };

  // Delta Node for record update operation
  struct RecordDelta : public Node {
    enum RecordType { INSERT = 0, DELETE = 1, UPDATE = 2 };

    // the enum variable indicating what kind of record delta it is
    RecordType op_type;

    // the key and value associated with is this delta
    KeyType key;
    ValueType value;

    // Constructor
    RecordDelta(Node* next, RecordType op, KeyType k, ValueType v,
                MappingTable& mapping_table, PidType next_leafnode,
                KeyType lowkey, KeyType highkey, bool inf_low, bool inf_high)
        : Node(nullptr, NodeType::RECORD_DELTA, 0, mapping_table, next_leafnode,
               next->is_leaf, lowkey, highkey, inf_low, inf_high),
          op_type(op),
          key(k),
          value(v) {
      // prepend this node to current head in the delta chain
      prepend(this, next);
      // temporarily update the slotuse of the new delta node
      this->slotuse = next->slotuse;
    }
  };

  // Delta Node for splitting operation step 1
  struct SplitDelta : public Node {
    // the separator key in the original node
    KeyType Kp;

    // pointer to the new created node
    PidType pQ;

    // Constructor
    SplitDelta(Node* next, KeyType Kp, PidType pQ, MappingTable& mapping_table,
               PidType next_leafnode, KeyType lowkey, KeyType highkey,
               bool inf_low, bool inf_high)
        : Node(next, NodeType::SPLIT_DELTA, 0, mapping_table, next_leafnode,
               next->is_leaf, lowkey, highkey, inf_low, inf_high),
          Kp(Kp),
          pQ(pQ) {
      // prepend this node to current head in the delta chain
      prepend(this, next);

      // temporarily update the slotuse of the new delta node
      this->slotuse = next->slotuse / 2;
    }
  };

  // Delta Node for splitting operation step 2
  struct IndexEntryDelta : public Node {
    // the separator key.
    // the new creeated node wil be bounded by [Kp, Kq)
    KeyType Kp, Kq;
    // indicating whether Kq is +infinity
    bool inf_Kq;

    // pointer to the new created node
    PidType pQ;

    // Constructor
    IndexEntryDelta(Node* next, KeyType Kp, KeyType Kq, bool Kq_is_inf,
                    PidType pQ, MappingTable& mapping_table,
                    PidType next_leafnode, KeyType lowkey, KeyType highkey,
                    bool inf_low, bool inf_high)
        : Node(next, NodeType::INDEX_ENTRY_DELTA, 0, mapping_table,
               next_leafnode, next->is_leaf, lowkey, highkey, inf_low,
               inf_high),
          Kp(Kp),
          Kq(Kq),
          inf_Kq(Kq_is_inf),
          pQ(pQ) {
      // prepend this node to current head in the delta chain
      prepend(this, next);
      // update the slotuse of the new delta node
      this->slotuse = next->slotuse + 1;
    }
  };

  // Delta Node for merging operation step 1
  struct RemoveDelta : public Node {
    // Constructor
    RemoveDelta(Node* next, MappingTable& mapping_table, PidType next_leafnode,
                KeyType lowkey, KeyType highkey, bool inf_low, bool inf_high)
        : Node(next, NodeType::REMOVE_NODE_DELTA, 0, mapping_table,
               next_leafnode, next->is_leaf, lowkey, highkey, inf_low,
               inf_high) {}
  };

  // Delta Node for merging operation step 2
  struct MergeDelta : public Node {
    KeyType Kp;
    Node* orignal_node;
    // Constructor
    MergeDelta(Node* next, KeyType Kp, Node* orignal_node,
               MappingTable& mapping_table, PidType next_leafnode,
               KeyType lowkey, KeyType highkey, bool inf_low, bool inf_high)
        : Node(next, NodeType::MERGE_DELTA, 0, mapping_table, next_leafnode,
               next->is_leaf, lowkey, highkey, inf_low, inf_high),
          Kp(Kp),
          orignal_node(orignal_node) {}
  };

  // Delta Node for merging operation step 3
  struct DeleteIndexDelta : public Node {
    KeyType Kp, Kq;
    PidType pQ;
    bool inf_Kq;

    // Constructor
    DeleteIndexDelta(Node* next, KeyType Kp, KeyType Kq, bool Kq_is_inf,
                     PidType pQ, MappingTable& mapping_table,
                     PidType next_leafnode, KeyType lowkey, KeyType highkey,
                     bool inf_low, bool inf_high)
        : Node(next, NodeType::INDEX_ENTRY_DELTA, 0, mapping_table,
               next_leafnode, next->is_leaf, lowkey, highkey, inf_low,
               inf_high),
          Kp(Kp),
          Kq(Kq),
          inf_Kq(Kq_is_inf),
          pQ(pQ) {}
  };

 public:
  // constructor
  BWTree(const KeyComparator& kc, const KeyEqualityChecker& ke,
         peloton::index::IndexMetadata* metadata)
      : m_key_less(kc),
        m_key_equal(ke),
        m_value_equal(ItemPointerEqualityChecker()),
        m_metadata(metadata) {
    KeyType waste;
    LeafNode* addr =
        new LeafNode(mapping_table, NULL_PID, waste, waste, true, true);

    long newpid = mapping_table.add(addr);
    if (newpid >= 0) {
      // initialize the root and head pid.
      root = newpid;
      headleaf = newpid;
    } else {
      delete addr;
      LOG_ERROR("Can't create the initial leafNode!");
      assert(0);
    }

    LOG_INFO("leaf_max = %d, inner_max = %d", leafslotmax, innerslotmax);
  }

  // destructor
  ~BWTree(){};

 public:
  // True if a < b ? "constructed" from m_key_less()
  inline bool key_less(const KeyType& a, const KeyType b,
                       bool b_max_inf) const {
    if (b_max_inf) return true;
    return m_key_less(a, b);
  }

  // True if a <= b ? constructed from m_key_less()
  inline bool key_lessequal(const KeyType& a, const KeyType b,
                            bool b_max_inf) const {
    if (b_max_inf) return true;
    return !m_key_less(b, a);
  }

  // True if a > b ? constructed from m_key_less()
  inline bool key_greater(const KeyType& a, const KeyType& b,
                          bool b_min_inf) const {
    if (b_min_inf) return true;
    return m_key_less(b, a);
  }

  // True if a >= b ? constructed from m_key_less()
  inline bool key_greaterequal(const KeyType& a, const KeyType b,
                               bool b_min_inf) const {
    if (b_min_inf) return true;
    return !m_key_less(a, b);
  }

  // True if a == b ? constructed from m_key_less()
  inline bool key_equal(const KeyType& a, const KeyType& b) const {
    return m_key_equal(a, b);
  }

  // True if a == b constructed from m_value_equal()
  inline bool value_equal(const ValueType& a, const ValueType& b) const {
    return m_value_equal(a, b);
  }

  inline bool key_in_node(KeyType key, const Node& node){
    return (
        key_greaterequal(key, node.low_key, node.inf_lowkey)
        && key_less(key, node.high_key, node.inf_highkey)
    );
  }


 private:
  KeyComparator m_key_less;
  KeyEqualityChecker m_key_equal;
  ItemPointerEqualityChecker m_value_equal;
  peloton::index::IndexMetadata* m_metadata;

  // get a path of PIDs for searhing for
  // searching for a key from the tree rooted at a given pid
  std::stack<PidType> search(PidType pid, KeyType key) {
    auto node = mapping_table.get(pid);
    if (node == nullptr) {
      std::stack<PidType> empty_res;
      return empty_res;
    }

    // get a path of PIDs for searhing for
    // searching for a key from the tree rooted at a given pid
    std::stack<PidType> path;
    path.push(pid);

    // call the helping function to fill up stack path
    PidType res = search(node, key, path);
    if (res == -1) {
      std::stack<PidType> empty_res;
      return empty_res;
    }

    if (path.empty()) {
      LOG_ERROR("Search get empty tree");
    }
    return path;
  }

  // The helping funtion for the above search function
  PidType search(Node* node, KeyType key, std::stack<PidType>& path) {
    // should always keep track of right key range even in delta node
    PidType pid;

    switch (node->node_type) {
      case LEAF:
      case RECORD_DELTA: {
        return node->pid;
      }
      case INDEX_ENTRY_DELTA:
      case DELETE_INDEX_TERM_DELTA: {
        IndexEntryDelta* indexEntryDelta = static_cast<IndexEntryDelta*>(node);
        if (key_greaterequal(key, indexEntryDelta->Kp, false) &&
            key_less(key, indexEntryDelta->Kq, indexEntryDelta->inf_Kq)) {
          pid = ((IndexEntryDelta*)node)->pQ;
          node = mapping_table.get(pid);
          if (node == nullptr) {
            LOG_ERROR("pid in split/merge delta not exist");
            return -1;
          }
          path.push(pid);
          return search(node, key, path);
        }

        return search(node->next, key, path);
      }
      case REMOVE_NODE_DELTA: {
        path.pop();
        if (path.empty()) {
          LOG_INFO("Search Path empty");
          return -1;
        }
        pid = path.top();

        node = mapping_table.get(pid);
        if (node == nullptr) {
          LOG_ERROR("Pid in split/merge delta not exist");
          return -1;
        } else {
          return search(node, key, path);
        }
      }
      case MERGE_DELTA: {
        if (key_greaterequal(key, ((MergeDelta*)node)->Kp, false)) {
          node = ((MergeDelta*)node)->orignal_node;
          if (node == nullptr) {
            LOG_ERROR("pid in split delta not exist");
            return -1;
          }

          return search(node, key, path);
        }
        return search(node->next, key, path);
      }
      case SPLIT_DELTA: {
        pid = ((SplitDelta*)node)->pQ;
        if (key_greaterequal(key, ((SplitDelta*)node)->Kp, false)) {
          node = mapping_table.get(pid);
          if (node == nullptr) {
            LOG_ERROR("pid in split/merge delta not exist");
            return -1;
          }
          // replace the top with our split node
          path.pop();
          path.push(pid);
          return search(node, key, path);
        }
        return search(node->next, key, path);
      }
      case INNER: {
        if (node->slotuse == 0) {
          pid = ((InnerNode*)node)->childid[0];
          if (pid == NULL_PID) {
            LOG_ERROR("error leftmost pid -- NULL_PID");
            return -1;
          }

          path.push(pid);
          node = mapping_table.get(pid);
          if (node == nullptr) {
            LOG_ERROR("pid in inner node not exist");
            return -1;
          }

          return search(node, key, path);
        } else {
          int i = 0;
          for (i = 0; i < node->slotuse; i++) {
            if (key_less(key, ((InnerNode*)node)->slotkey[i], false))
              break;  // 0, 1 ,2
          }           // 0  1  2  3
          pid = ((InnerNode*)node)->childid[i];
          node = mapping_table.get(pid);
          if (node == nullptr) {
            LOG_ERROR("pid in inner node not exist");
            return -1;
          }
          path.push(pid);
          return search(node, key, path);
        }
      }
      default:
        return -1;
    }
    return -1;
  }

  // check whether a key is in a give delta chain
  bool key_is_in(KeyType key, Node* listhead, DelSet& deleted) {
    if (listhead == nullptr) return false;

    Node* node = listhead;
    switch (node->node_type) {
      case RECORD_DELTA: {
        RecordDelta* rcd_node = (RecordDelta*)node;
        if (rcd_node->op_type == RecordDelta::INSERT &&
            key_equal(rcd_node->key, key) &&
            (!deleted.count(rcd_node->value))) {
          return true;
        } else if (rcd_node->op_type == RecordDelta::DELETE &&
                   key_equal(rcd_node->key, key)) {
          deleted.insert(rcd_node->value);
        }
        return key_is_in(key, node->next, deleted);
      }
      case LEAF: {
        LeafNode* lf_node = (LeafNode*)node;
        for (int i = 0; i < (lf_node->slotuse); i++) {
          if (key_equal(lf_node->slotkey[i], key)) {
            for (auto val : (*lf_node->slotdata[i])) {
              if (!deleted.count(val)) {
                return true;
              }
            }
            return false;
          }
        }
        return false;
      }
      case MERGE_DELTA: {
        if (key_greaterequal(key, ((MergeDelta*)node)->Kp, false)) {
          node = ((MergeDelta*)node)->orignal_node;
          return key_is_in(key, node, deleted);
        }
        return key_is_in(key, node->next, deleted);
      }
      case SPLIT_DELTA: {
        if (key_greaterequal(key, ((SplitDelta*)node)->Kp, false)) {
          LOG_ERROR("key is in should never reach here, split wrong branch");
          assert(0);
        }
        return key_is_in(key, node->next, deleted);
      }
      default:
        return false;
    }
  }

  // the helping function for above function
  inline bool key_is_in(KeyType key, Node* listhead) {
    DelSet deleted_set;
    return key_is_in(key, listhead, deleted_set);
  };

  // return pair a pair of integer, the 1st number will be
  // the number of values for a coressponding key in the delta chian;
  // the 2nd will be the number of (key, value) pair in the delta chain
  std::pair<int, int> count_pair(KeyType key, ValueType value, Node* listhead) {
    int total_count = 0;
    int pair_count = 0;
    DelSet deleted;
    Node* node = listhead;

    int len = 0;
    while (node != nullptr) {
      switch (node->node_type) {
        case RECORD_DELTA: {
          len++;
          RecordDelta* rcd_node = static_cast<RecordDelta*>(node);
          if (rcd_node->op_type == RecordDelta::INSERT) {
            if (key_equal(rcd_node->key, key) &&
                (!deleted.count(rcd_node->value))) {
              // update the total count and pair count
              total_count++;
              if (value_equal(rcd_node->value, value)) {
                pair_count++;
              }
            }
          } else if (rcd_node->op_type == RecordDelta::DELETE &&
                     key_equal(rcd_node->key, key)) {
            // add the deleted value to the delSet
            deleted.insert(rcd_node->value);
          }
          node = node->next;
          break;
        }
        case LEAF: {
          LeafNode* lf_node = static_cast<LeafNode*>(node);
          for (int i = 0; i < (lf_node->slotuse); i++) {
            if (key_equal(lf_node->slotkey[i], key)) {
              for (ValueType v : *(lf_node->slotdata[i])) {
                if (deleted.count(v)) continue;
                total_count++;
                if (value_equal(v, value)) pair_count++;
              }
              break;
            }
          }
          if (node->next != nullptr) {
            LOG_ERROR("leaf.next != null");
            assert(node->next == nullptr);
          }
          node = nullptr;
          break;
        }
        case MERGE_DELTA: {
          if (key_greaterequal(key, ((MergeDelta*)node)->Kp, false)) {
            node = ((MergeDelta*)node)->orignal_node;
          } else {
            node = node->next;
          }
          break;
        }
        case SPLIT_DELTA: {
          if (key_greaterequal(key, ((SplitDelta*)node)->Kp, false)) {
            LOG_ERROR("count pair should never reach here, split wrong branch");
            assert(0);
          } else {
            node = node->next;
          }
          break;
        }
        default:
          LOG_ERROR("count pair should not be here");
          break;
      }
    }

    return std::pair<int, int>(total_count, pair_count);
  }

  // append a delete detla for deleting (key, value) from a delta chain
  // whose head is basic_node.
  // deletekey indicates whether we need to delete the key from this node
  // (when all its value have been deleted)
  bool append_delete(Node* basic_node, KeyType key, ValueType value,
                     bool deletekey) {
    RecordDelta* new_delta = new RecordDelta(
        basic_node, RecordDelta::DELETE, key, value, mapping_table,
        basic_node->next_leafnode, basic_node->low_key, basic_node->high_key,
        basic_node->inf_lowkey, basic_node->inf_highkey);

    if (deletekey) {
      new_delta->slotuse -= 1;
    }

    if (mapping_table.set(basic_node->pid, basic_node, new_delta)) {
      return true;
    } else {
      LOG_INFO("CAS FAIL: redo delete recordDelta");
      delete new_delta;
      return false;
    };
  }

  bool apend_merge() {
    // TODO: A lot
    return false;
  }

  // prepend a delta_node to orig_node and increment the delta_list_len
  inline static bool prepend(Node* delta_node, Node* orig_node) {
    // update the delta_list_len of the delta node
    delta_node->delta_list_len = orig_node->delta_list_len + 1;

    // maintain next, prev pointer
    delta_node->next = orig_node;

    return true;
  }

 public:
  // save all values corressponding to a given key in the result vector
  void get_value(KeyType key, std::vector<ValueType>& result) {
    std::stack<PidType> path = search(root, key);

    PidType target_node = path.top();
    Node* next = mapping_table.get(target_node);

    LOG_INFO("Search result: pid - %lld, slotuse: %d", next->pid,
             next->slotuse);

    if (!next->is_leaf) {
      LOG_ERROR("get_value's search result is not a leaf");
      return;
    }

    DelSet delset;

    // traverse the delta chain from top down
    // to construct the correct result
    while (next) {
      switch (next->node_type) {
        case RECORD_DELTA: {
          RecordDelta* node = static_cast<RecordDelta*>(next);
          if (key_equal(node->key, key)) {
            if (node->op_type == RecordDelta::RecordType::INSERT) {
              if (delset.find(node->value) == delset.end())
                result.push_back(node->value);
            } else if (node->op_type == RecordDelta::RecordType::DELETE) {
              // if we meet a "delete" all later value associated with this key
              // is invalid.
              delset.insert(node->value);
            }
          }
          next = node->next;
        } break;
        case LEAF: {
          LeafNode* leaf = static_cast<LeafNode*>(next);
          for (int i = 0; i < leaf->slotuse; i++) {
            if (key_equal(leaf->slotkey[i], key)) {
              unsigned long vsize = leaf->slotdata[i]->size();
              for (int j = 0; j < vsize; j++) {
                if (delset.find(leaf->slotdata[i]->at(j)) == delset.end())
                  result.push_back(leaf->slotdata[i]->at(j));
              }
            }
          }
          next = nullptr;
        } break;
        case SPLIT_DELTA: {
          SplitDelta* split_delta = static_cast<SplitDelta*>(next);
          // if key >= Kp, we go to the new split node
          if (key_greaterequal(key, split_delta->Kp, false)) {
            LOG_ERROR("search result direct to another node");
            next = mapping_table.get(split_delta->pQ);
          }
          // else we go alone this delta chain
          else {
            next = split_delta->next;
          }
        } break;
        case MERGE_DELTA: {
          MergeDelta* merge_delta = static_cast<MergeDelta*>(next);
          // if key >= Kp, we go to the original node
          if (key_greaterequal(key, merge_delta->Kp, false)) {
            next = merge_delta->orignal_node;
          }
          // else we go alone this delta chain
          else {
            next = merge_delta->next;
          }
        } break;
        case REMOVE_NODE_DELTA:
          // if we meet remove node delta, we can search from the root again.
          result.clear();
          get_value(key, result);
          break;
        default:
          LOG_ERROR("meet wrong delta: %d during get_value", next->node_type);
      }
    }
  }

  // creaet a new root when splitting the current root
  void create_root(PidType cur_root, PidType new_node_pid,
                   KeyType pivotal){
    KeyType waste;
    InnerNode* new_root_node =
        new InnerNode(mapping_table, waste, waste, true, true);

    new_root_node->slotuse = 1;
    new_root_node->slotkey[0] = pivotal;
    new_root_node->childid[0] = cur_root;
    new_root_node->childid[1] = new_node_pid;
    PidType new_root_pid = mapping_table.add(new_root_node);

    if(new_root_pid == NULL_PID){
      LOG_ERROR("can't add root pid in split");
      assert(new_root_pid != NULL_PID);
    }
    root = new_root_pid;

    int i=0;
    while(!std::atomic_compare_exchange_strong(
        (std::atomic<PidType>*)&root,
        &cur_root,
        new_root_pid)
        ){
      //LOG_ERROR("wanner add root node, but root node changed before");
      assert(i++ < 5);
    }
    LOG_INFO("new root = %llu, created", new_root_pid);
    return;
  }

  // find the parent of node that contains key
  Node* find_parent(KeyType key, std::stack<PidType>& path,
                    const std::vector<PidType>& visited_nodes){
    PidType parent_pid = path.top();
    Node* parent_node =  mapping_table.get(parent_pid);
    LOG_INFO("parent = %llu, got from path", parent_pid);

    while(!key_in_node(key, *parent_node)){
      LOG_INFO("in split insert Entry, parent changed!");
      path = search(BWTree::root, key);
      for(int i = visited_nodes.size()-1; i>=0; i--){
        if(visited_nodes[i] != path.top()){
          LOG_ERROR("In re-find parent, children changed");
          assert(visited_nodes[i] == path.top());
        }
        path.pop();
      }
      parent_pid = path.top();
      parent_node =  mapping_table.get(parent_pid);
    }
    return parent_node;
  }

  // perform split for the leaf node containing key
  // It will iterativelyy split parent node if necessary
  void split(KeyType key) {
    std::stack<PidType> path = search(BWTree::root, key);
    std::vector<PidType> visited_nodes;

    PidType check_split_pid = path.top();
    path.pop();
    Node* check_split_node = mapping_table.get(check_split_pid);

    while (check_split_node->need_split()) {
      LOG_INFO("pid = %llu, begin Split", check_split_pid);

      // Step 1, add splitDelta to current check_split_node
      SplitDelta* new_split;
      KeyType pivotal;

      // create a new right sibling node
      PidType new_node_pid;

      if (check_split_node->is_leaf)
        new_node_pid = create_leaf(check_split_node, &pivotal);
      else
        new_node_pid = create_inner(check_split_node, &pivotal);

      Node* new_node = mapping_table.get(new_node_pid);

      // create and prepend a split node
      new_split = new SplitDelta(check_split_node, pivotal, new_node_pid,
                                 mapping_table, new_node_pid,
                                 check_split_node->low_key, new_node->low_key,
                                 check_split_node->inf_lowkey, false);

      if (!mapping_table.set(check_split_pid, check_split_node, new_split)) {
        LOG_INFO("CAS FAIL: Split CAS fails");
        // clean created waste
        Node* old_ptr = mapping_table.get(new_node_pid);
        delete old_ptr;
        if (!mapping_table.set(new_node_pid, old_ptr, nullptr)) {
          LOG_ERROR("In split, delete node before retry fail!");
          assert(0);
        }
        delete new_split;

        check_split_node = mapping_table.get(check_split_pid);
        continue;
      }

      if(key_greaterequal(key,pivotal,false)) {
        visited_nodes.push_back(new_node_pid);
      } else {
        visited_nodes.push_back(check_split_pid);
      }


      if (check_split_node->is_leaf) {
        LOG_INFO("pid = %llu, Split finished, new leaf node %llu created",
                 check_split_pid, new_node_pid);

#ifdef MY_PRINT_DEBUG
        print_node_info(check_split_pid);
        print_node_info(new_node_pid);
#endif
      } else {
        LOG_INFO("pid = %llu, Split finished, new inner node %llu created",
                 check_split_pid, new_node_pid);
#ifdef MY_PRINT_DEBUG
        print_node_info(check_split_pid);
        print_node_info(new_node_pid);
#endif
      }

#ifdef TURN_ON_CONSOLIDATE
      // check if we need to consolidate
      consolidate(check_split_pid);
#endif

      // Step 2, create new root if necessary
      if (path.empty()) {
        // create new root
        create_root(check_split_pid, new_node_pid, pivotal);
        return;
      }

      // Step 3 add indexEntryDelta to current check_split_node
      bool redo = true;
      while (redo) {
        // get the parent node
        check_split_node = find_parent(key,path,visited_nodes);
        check_split_pid = check_split_node->pid;
        LOG_INFO("finding parent = %llu", check_split_pid);


        IndexEntryDelta* new_indexEntryDelta = new IndexEntryDelta(
            check_split_node, new_split->Kp, new_node->high_key,
            new_node->inf_highkey, new_split->pQ, mapping_table,
            check_split_node->next_leafnode, check_split_node->low_key,
            check_split_node->high_key, check_split_node->inf_lowkey,
            check_split_node->inf_highkey);

        if (mapping_table.set(check_split_pid, check_split_node,
                              new_indexEntryDelta)) {
          LOG_INFO("new indexEntryDelta added to pid = %llu", check_split_pid);
          redo = false;
          path.pop();
        } else {
          LOG_INFO("CAS FAIL: redo add indexEntryDelta");
          delete new_indexEntryDelta;
          check_split_node = mapping_table.get(check_split_pid);
        }
      }

      check_split_node = mapping_table.get(check_split_pid);
#ifdef TURN_ON_CONSOLIDATE
      // check if we need to consolidate
      if (!check_split_node->need_split()) {
        consolidate(check_split_pid);
        break;
      }
#endif
    }
  }

  // insert a (key, value) pair into our bwtree
  bool insert_entry(KeyType key, ValueType value) {
#ifdef LEI_PRINT_DEBUG
    LOG_TRACE("-----Entering insert_entry, key:");
    print_key_info(key);
#endif

    bool redo = true;
    while (redo) {
      // Step1: perform split if necessary
      split(key);

      // Step2: Add the insert record delta to current delta chain
      // update the basic_node before adding record delta
      std::stack<PidType> path = search(BWTree::root, key);

      PidType basic_pid = path.top();

#ifdef TURN_ON_CONSOLIDATE
      // Check whether we need to consolidate, also check split correctness
      Node* basic_node = consolidate(basic_pid);
      if(basic_node == nullptr) continue;
#else
      Node* basic_node = mapping_table.get(basic_pid);
#endif

      if(!key_in_node(key, *basic_node)){
        LOG_INFO("Insert meet structure change!");
        continue;
      }

      bool key_dup = key_is_in(key, basic_node);

      // check whether we can insert duplicate key
      if (m_metadata->HasUniqueKeys()) {
        LOG_INFO("unique key required!");
        if (key_dup) return false;
      }

      RecordDelta* new_delta = new RecordDelta(
          basic_node, RecordDelta::INSERT, key, value, mapping_table,
          basic_node->next_leafnode, basic_node->low_key, basic_node->high_key,
          basic_node->inf_lowkey, basic_node->inf_highkey);

      new_delta->pid = basic_pid;
      if (!key_dup) new_delta->slotuse = new_delta->next->slotuse + 1;

      new_delta->high_key = basic_node->high_key;
      new_delta->low_key = basic_node->low_key;

      redo = !mapping_table.set(basic_pid, basic_node, new_delta);
      if (redo) {
        LOG_INFO("CAS FAIL: redo add insert record");
        delete new_delta;
      }else{
#ifdef LEI_PRINT_DEBUG
        LOG_TRACE("-----Success add a new insert record delta,%lld-----",basic_pid);
        print_node_info(new_delta);
#endif
      }


    }
    //    print_node_info(basic_pid);

    return true;
  }

  // delete a (key, value) pair from our bwtree
  bool delete_entry(KeyType key, ValueType value) {


    // update the basic_node before adding record delta
    bool redo = true;
    // check and insert delete delta
    while (redo) {
      // Step1: perform split if necessary
      split(key);

      // search again to make sure we are deleting from the correct node
      std::stack<PidType> path = search(root, key);

      PidType basic_pid = path.top();
      path.pop();

#ifdef TURN_ON_CONSOLIDATE
      // Check whether we need to consolidate, also check the correctness of
      // previous split
      Node* basic_node = consolidate(basic_pid);

#else
      Node* basic_node = mapping_table.get(basic_pid);
#endif

      if(!key_in_node(key, *basic_node)){
        LOG_INFO("Delete meet structure change!");
        continue;
      }

      auto tv_count_pair = count_pair(key, value, basic_node);

      if (!tv_count_pair.second) {
        LOG_INFO("DeleteEntry Not Exist");
        return false;
      }

      if (tv_count_pair.second > tv_count_pair.first) {
        LOG_ERROR("error!! count pair second > first");
        assert(0);
      }

      bool deletekey = (tv_count_pair.second == tv_count_pair.first);

      // Step3: Add the delete record delta to current delta chain
      //      printf("before append_delete\n");
      redo = !append_delete(basic_node, key, value, deletekey);
    }

    return true;
  };

  // perform consolidate for a certain node identified by its pid
  Node* consolidate(PidType pid) {
    Node* orinode = mapping_table.get(pid);

    // check the correctness of previous split
    if (orinode->need_split()) {
      LOG_ERROR("From consolidate: invalid prevous split");
      assert(0);
    }

    while (orinode->delta_list_len > MAX_DELTA_CHAIN_LEN) {
      if (orinode->is_leaf) {
        LOG_INFO("begin leaf consolidation!");

        // We need to consolidate a leaf node
        LeafNode* new_leaf = new LeafNode(
            mapping_table, orinode->next_leafnode, orinode->low_key,
            orinode->high_key, orinode->inf_lowkey, orinode->inf_highkey);

        auto res = leaf_fake_consolidate(orinode);
        auto keys = res.first;
        auto vals = res.second;

        if (keys.size() != vals.size() || keys.size() > leafslotmax) {
          LOG_ERROR("wrong consolidated leaf key size!");
          assert(keys.size() == vals.size() && keys.size() <= leafslotmax);
        }

        for (int i = 0; i < keys.size(); i++) {
          new_leaf->slotkey[i] = keys[i];
          new_leaf->slotdata[i] = new std::vector<ValueType>(vals[i]);
        }
        new_leaf->slotuse = keys.size();

        // CAS replace the item with new leaf and throw old delta chain away
        if (mapping_table.set(pid, orinode, new_leaf)) {
          while (garbage_table.add(orinode) == NULL_PID)
            ;
#ifdef LEI_PRINT_DEBUG
          LOG_TRACE("-----leaf consolidation finished! %lld, old------",pid);
          print_node_info(orinode);
          LOG_TRACE("-----leaf consolidation finished! %lld,------",pid);
          print_node_info(new_leaf);
#endif
          return new_leaf;
        } else {
//          print_node_info(orinode);
//          print_node_info(new_leaf);
          delete new_leaf;
          LOG_INFO(
              "CAS FAIL: unnecessary consolidate, remove just created "
              "consolidated node");
        }
      } else {
        LOG_INFO("begin inner consolidation!");

        // We need to consolidate an inner node
        InnerNode* new_inner =
            new InnerNode(mapping_table, orinode->low_key, orinode->high_key,
                          orinode->inf_lowkey, orinode->inf_highkey);

        auto res = inner_fake_consolidate(orinode);
        auto keys = res.first;
        auto childs = res.second;

        if (keys.size() != childs.size() - 1 || keys.size() > innerslotmax) {
          LOG_ERROR("wrong consolidated inner key size!");
          assert(0);
        }

        int i;
        for (i = 0; i < keys.size(); i++) {
          new_inner->slotkey[i] = keys[i];
          new_inner->childid[i] = childs[i];
        }
        new_inner->childid[i] = childs[i];
        new_inner->slotuse = keys.size();

        // CAS replace the item with new leaf and throw old delta chain away
        if (mapping_table.set(pid, orinode, new_inner)) {
          while (garbage_table.add(orinode) == NULL_PID)
            ;
          LOG_INFO("-----inner consolidation finished! %lld------",pid);
          print_node_info(new_inner);
          return new_inner;
        } else {
          delete new_inner;
          LOG_INFO(
              "CAS FAIL: unnecessary consolidate, remove just created "
              "consolidated node");
        }
      }
      LOG_INFO("Fail consolidation");
      orinode = mapping_table.get(pid);
      if (orinode->need_split()) {
        LOG_ERROR("From consolidate: invalid split check");
        return nullptr;
      }
    }

    return orinode;
  }

  // helper function to consolidate a leaf node and return the
  // resulting vector of keys and values
  std::pair<std::vector<KeyType>, std::vector<std::vector<ValueType>>>
  leaf_fake_consolidate(Node* new_delta) {
    std::stack<Node*> delta_chain;
    Node* tmp_cur_node = new_delta;
    while (tmp_cur_node) {
      delta_chain.push(tmp_cur_node);
      tmp_cur_node = tmp_cur_node->next;
    }

    // prepare two array to store what logical k-v pairs we have
    std::vector<KeyType> tmpkeys;

    std::vector<std::vector<ValueType>> tmpvals;

    // the first node must be the original leaf node itself
    if (delta_chain.top()->node_type != LEAF) {
      LOG_ERROR("delta chain top not LEAF");
      assert(delta_chain.top()->node_type == LEAF);
    }

    LeafNode* orig_leaf_node = static_cast<LeafNode*>(delta_chain.top());
    delta_chain.pop();

    // copy the data in the base node
    for (int i = 0; i < orig_leaf_node->slotuse; i++) {
      tmpkeys.push_back(orig_leaf_node->slotkey[i]);
      tmpvals.push_back(std::vector<ValueType>(*(orig_leaf_node->slotdata[i])));
    }

    //LOG_INFO("Leaf Fake Consolidate: delta_chain.len = %lu", delta_chain.size());

    // traverse the delta chain
    while (!delta_chain.empty()) {
      // get top delta node
      Node* cur_delta = delta_chain.top();
      delta_chain.pop();
      switch (cur_delta->node_type) {
        case RECORD_DELTA: {
          // first see the key has already existed
          bool no_key = true;
          RecordDelta* recordDelta = static_cast<RecordDelta*>(cur_delta);
          //printf("\t pid:%llu, Consolidation meet key: ", recordDelta->pid);

          if (recordDelta->op_type == RecordDelta::INSERT) {
            for (int x = 0; x < tmpkeys.size(); x++) {
              if (key_equal(tmpkeys[x], recordDelta->key)) {
#ifdef LEI_PRINT_DEBUG
                print_key_info(recordDelta->key);
                printf("\n");
#endif
                tmpvals[x].push_back(recordDelta->value);
                no_key = false;
                break;
              }
            }

            // key not exists, need to insert somewhere
            if (no_key) {
              if (recordDelta->slotuse == 0) {
#ifdef LEI_PRINT_DEBUG
                printf("\t pid:%llu, Consolidation add new key: ", recordDelta->pid);
                print_key_info(recordDelta->key);
                printf("\n");
#endif
                tmpkeys.push_back(recordDelta->key);
                tmpvals.push_back(
                    std::vector<ValueType>(1, recordDelta->value));
              } else {
                int target_pos = 0;
                for (int x = ((int)tmpkeys.size()) - 1; x >= 0; x--) {
                  if (key_greaterequal(recordDelta->key, tmpkeys[x], false)) {
                    target_pos = x + 1;
                    break;
                  }
                }

                tmpkeys.insert(tmpkeys.begin() + target_pos, recordDelta->key);
                tmpvals.insert(tmpvals.begin() + target_pos,
                               std::vector<ValueType>(1, recordDelta->value));
              }

              if (tmpvals.size() != recordDelta->slotuse) {
                LOG_ERROR("tmpvals.size() != recordDelta->slotuse");
                assert(tmpvals.size() == recordDelta->slotuse);
              }
            }  // end of RecordDelta::INSERT

          } else if (recordDelta->op_type == RecordDelta::DELETE) {
            for (int x = 0; x < tmpkeys.size(); x++) {
              if (key_equal(tmpkeys[x], recordDelta->key)) {
                // remove value in the vector
                for (int j = ((int)tmpvals[x].size() - 1); j >= 0; j--) {
                  if (m_value_equal(tmpvals[x][j], recordDelta->value)) {
                    tmpvals[x].erase(tmpvals[x].begin() + j);
                  }
                }

                // if vector is empty, needed to be removed
                if (tmpvals[x].size() == 0) {
                  tmpkeys.erase(tmpkeys.begin() + x);
                  tmpvals.erase(tmpvals.begin() + x);
                }
                break;
              }
            }
            if (tmpvals.size() != recordDelta->slotuse) {
//              printf("tmpvals.size() = %lu\n", tmpvals.size());
//              print_node_info(recordDelta);
              LOG_ERROR("tmpvals.size() != recordDelta->slotuse");
              assert(tmpvals.size() == recordDelta->slotuse);
            }
          }
          break;
        }
        case SPLIT_DELTA: {
          SplitDelta* splitDelta = static_cast<SplitDelta*>(cur_delta);
          // truncate all the values whose key is >= Kp
          for (int i = 0; i < tmpkeys.size(); i++) {
            if (key_greaterequal(tmpkeys[i], splitDelta->Kp, false)) {
              tmpkeys.resize(i);
              tmpvals.resize(i);
              break;
            }
          }
        } break;
        case MERGE_DELTA:
          break;
        case REMOVE_NODE_DELTA:
          break;
        case INNER:
          LOG_ERROR("Wrong inner delta!");
          break;
        case LEAF:
          LOG_ERROR("Wrong leaf delta!");
          break;
        default:
          break;
      }
    }

    LOG_INFO("Leaf fake Consolidation finish: %lu keys, %lu vals", tmpkeys.size(),
             tmpvals.size());
    return std::make_pair(tmpkeys, tmpvals);
  }

  // helper function to consolidate an inner node and return the
  // resulting vector of keys and childpids
  std::pair<std::vector<KeyType>, std::vector<PidType>> inner_fake_consolidate(
      Node* new_delta) {
    std::stack<Node*> delta_chain;
    Node* tmp_cur_node = new_delta;
    while (tmp_cur_node) {
      delta_chain.push(tmp_cur_node);
      tmp_cur_node = tmp_cur_node->next;
    }

    // prepare two array to store what logical k-v pairs we have
    std::vector<KeyType> tmpkeys;

    std::vector<PidType> tmpchilds;

    // the first node must be the original leaf node itself
    assert(delta_chain.top()->node_type == INNER);

    InnerNode* orig_inner_node = static_cast<InnerNode*>(delta_chain.top());
    delta_chain.pop();

    // copy the data in the base node
    for (int i = 0; i < orig_inner_node->slotuse; i++) {
      tmpkeys.push_back(orig_inner_node->slotkey[i]);
      tmpchilds.push_back(orig_inner_node->childid[i]);
    }
    tmpchilds.push_back(orig_inner_node->childid[orig_inner_node->slotuse]);

    LOG_INFO("Inner Consolidate: delta_chain.len = %lu", delta_chain.size());

    // traverse the delta chain
    while (!delta_chain.empty()) {
      // get top delta node
      Node* cur_delta = delta_chain.top();
      delta_chain.pop();

      switch (cur_delta->node_type) {
        case INDEX_ENTRY_DELTA: {
          IndexEntryDelta* indexEntryDelta =
              static_cast<IndexEntryDelta*>(cur_delta);
          int pos = 0;
          for (pos = 0; pos < tmpkeys.size(); pos++) {
            if (key_less(indexEntryDelta->Kp, tmpkeys[pos], false)) {
              break;
            }
          }

          tmpkeys.insert(tmpkeys.begin() + pos, indexEntryDelta->Kp);
          tmpchilds.insert(tmpchilds.begin() + pos + 1, indexEntryDelta->pQ);
        } break;

        case SPLIT_DELTA: {
          SplitDelta* splitDelta = static_cast<SplitDelta*>(cur_delta);
          // truncate all the values whose key is >= Kp
          for (int i = 0; i < tmpkeys.size(); i++) {
            if (key_greaterequal(tmpkeys[i], splitDelta->Kp, false)) {
              tmpkeys.resize(i);
              tmpchilds.resize(i + 1);
              break;
            }
          }
        } break;
        case INNER:
          LOG_ERROR("Wrong inner delta!");
          break;
        case LEAF:
          LOG_ERROR("Wrong leaf delta!");
          break;
        default:
          LOG_ERROR("impossible type on inner node");
          break;
      }
    }

    return std::make_pair(tmpkeys, tmpchilds);
  }

  // create a new leaf node that split from original "check_split_node"
  // and setting the pivotal as the first key in the new leaf node
  PidType create_leaf(Node* check_split_node, KeyType* pivotal) {
    KeyType waste;
    LeafNode* new_leaf = new LeafNode(
        mapping_table, check_split_node->next_leafnode, waste,
        check_split_node->high_key, false, check_split_node->inf_highkey);

    auto res = leaf_fake_consolidate(check_split_node);
    int orisize = check_split_node->slotuse;

    for (int i = orisize / 2; i < orisize; i++) {
      new_leaf->slotkey[i - orisize / 2] = res.first[i];
      new_leaf->slotdata[i - orisize / 2] =
          new std::vector<ValueType>(res.second[i]);
    }
    new_leaf->low_key = new_leaf->slotkey[0];
    new_leaf->slotuse = (unsigned short)((orisize + 1) / 2);

    *pivotal = new_leaf->slotkey[0];
    PidType new_leaf_pid = mapping_table.add(new_leaf);

    if(new_leaf_pid == NULL_PID) {
      LOG_ERROR("can't add new_leaf_pid");
      assert(new_leaf_pid != NULL_PID);
    }

    return new_leaf_pid;
  }

  // create a new inner node that split from original "check_split_node"
  // and setting the pivotal as the first key in the new inner node
  PidType create_inner(Node* check_split_node, KeyType* pivotal) {
    KeyType waste;
    InnerNode* new_inner =
        new InnerNode(mapping_table, waste, check_split_node->high_key, false,
                      check_split_node->inf_highkey);

    auto res = inner_fake_consolidate(check_split_node);
    int orisize = check_split_node->slotuse;

    new_inner->childid[0] = NULL_PID;
    for (int i = orisize / 2; i < orisize; i++) {
      new_inner->slotkey[i - orisize / 2] = res.first[i];
      new_inner->childid[i - orisize / 2 + 1] = res.second[i + 1];
    }
    new_inner->low_key = new_inner->slotkey[0];
    new_inner->slotuse = (unsigned short)((orisize + 1) / 2);

    *pivotal = new_inner->slotkey[0];
    PidType new_inner_pid = mapping_table.add(new_inner);

    if(new_inner_pid == NULL_PID) {
      LOG_ERROR("can't add new_inner_pid");
      assert(new_inner_pid != NULL_PID);
    }

    return new_inner_pid;
  }

  // interface for scanning all values in the bwtree and put them into
  // a result vector
  void scan_all(std::vector<ValueType>& v) {
#ifdef LEI_PRINT_DEBUG
    LOG_TRACE("-----------IN SCAN ALL:----------");
#endif
    Node* node = mapping_table.get(headleaf);

    // scan the leaf nodes list from begin to the end
    while (node != nullptr) {
      auto all_key_value_pair = leaf_fake_consolidate(node);

      LOG_INFO("fake_consolidate size: %lu", all_key_value_pair.second.size());

#ifdef LEI_PRINT_DEBUG
      for(int i =0; i< (all_key_value_pair.first).size();i++){
        print_key_info(all_key_value_pair.first[i]);
        printf(" %lu\n",all_key_value_pair.second[i].size());
      }
#endif
      for (auto const& value : all_key_value_pair.second) {
        v.insert(v.end(), value.begin(), value.end());
      }

      node = mapping_table.get(node->next_leafnode);
    }
  }

  // interface for scanning all values in the bwtree under some constraints
  // and put them into a result vector
  void scan(std::vector<KeyType>& keys_result,
            std::vector<std::vector<ItemPointer>>& values_result) {
    Node* node = mapping_table.get(headleaf);

    // scan the leaf nodes list from begin to the end
    while (node != nullptr) {
      auto all_key_value_pair = leaf_fake_consolidate(node);
      std::vector<KeyType>& keys = all_key_value_pair.first;
      std::vector<std::vector<ValueType>>& values = all_key_value_pair.second;

      keys_result.insert(keys_result.end(), keys.begin(), keys.end());
      values_result.insert(values_result.end(), values.begin(), values.end());

      node = mapping_table.get(node->next_leafnode);
    }
  }

  /*
   * Followings are helper funtion for debuging
   * */
  // print the content of a key in readable manner
  void print_key_info(KeyType& key) {
    std::cout << key.GetTupleForComparison(m_metadata->GetKeySchema())
                     .GetValue(0) << ","
              << key.GetTupleForComparison(m_metadata->GetKeySchema())
                     .GetValue(1) << " ";
  }

  // print out a delta chain in readable manner
  void print_node_delta_chain(Node* node, size_t total_len) {
    // print out deltas added to this node in order
    for (int i = 0; i <= total_len; i++) {
      if (node->delta_list_len != total_len - i) {
        LOG_ERROR("Wrong delta chain length!");
      }
      if (node->node_type == RECORD_DELTA) {
        if (((RecordDelta*)node)->op_type == RecordDelta::INSERT) {
          printf("insert(%d) ", ((RecordDelta*)node)->slotuse);
          print_key_info(((RecordDelta*)node)->key);
          printf("->");
        } else if (((RecordDelta*)node)->op_type == RecordDelta::DELETE)
          printf("delete->");
      } else if (node->node_type == SPLIT_DELTA) {
        printf("split(pQ=%llu)->", ((SplitDelta*)node)->pQ);
      } else if (node->node_type == LEAF) {
        printf("leaf");
        for (int i = 0; i < node->slotuse; i++) {
          print_key_info(((LeafNode*)node)->slotkey[i]);
          printf(" %lu",(((LeafNode*)node)->slotdata[i])->size());
          printf("\n");
        }

      } else if (node->node_type == INNER) {
        printf("inner");
      }
      node = node->next;
    }
  }

  // print out necessary info for a node in readable manner (given Node*)
  void print_node_info(Node* node) {
    size_t total_len = node->delta_list_len;
    printf("pid - %lld, delta_chain_len: %ld, slotuse: %d\n", node->pid,
           total_len, node->slotuse);

    // print out deltas added to this node in order
    print_node_delta_chain(node, total_len);

    printf("\n");
  }

  // print out necessary info for a node in readable manner (given PID)
  void print_node_info(PidType pid) {
    Node* node = mapping_table.get(pid);
    size_t total_len = node->delta_list_len;
    printf("pid - %lld, delta_chain_len: %ld, slotuse: %d\n", pid, total_len,
           node->slotuse);

    // print out deltas added to this node in order
    print_node_delta_chain(node, total_len);

    printf("\n");
  }
};

}  // End index namespace
}  // End peloton namespace
