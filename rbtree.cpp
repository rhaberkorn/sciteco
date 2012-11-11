#include <bsd/sys/tree.h>

#include "rbtree.h"

RB_GENERATE(RBTree::Tree, RBTree::RBEntry, nodes, RBTree::compare_entries);
