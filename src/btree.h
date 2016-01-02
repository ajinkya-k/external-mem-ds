#include "buffer.h"
#include <stdexcept>
#include <stddef.h>
#include <list>
#include <cstring>

using namespace	std;

//Type Definitions
typedef long blocknum_t;
typedef long offset_t;
typedef long long int size_type;

//CONSTANTS
const blocknum_t NULL_BLOCK = -1;
const offset_t NULL_OFFSET = -1;
/*
	Actually a B+Tree but using BTree in class names for readability


        Class BTreeNode (Abstract)
              /           \
             /             \
        LeafNode      InternalNode

	Class BTree HAS A BTreeNode root
	BTreeLeaf contains pointers(block nos and offsets) to item records
	The leaves will form a doubly linked list

*/

/*

	The B-Tree on the disk will be stored as one file with
	all the internal nodes and leaf ndoes
	and another one with the data which is pointed to by the leaves

	The structure of the B-Tree key file:


	Part of the file                Size            Comments
	==========================================================================
                                    HEADER
                                    ------
	“RMAD”                          4 bytes         Just for fun! :P
	“BTREE”                         8 bytes         Identifies the data structure
	Element Key size                4 bytes         generally from sizeof(K)
	Element Value size              4 bytes         generally from sizeof(V)
	Root Node block number          sizeof(long)    block no. of the root of Btree
	Total blocks allocated          <temp>          <temp>
	Head block no.                  sizeof(long)    traversing DLL in leaves
	Tail block no.                  sizeof(long)    traversing DLL in leaves
	data_file’s name                32 bytes

                                    Root Node
                                    ---------
	No of empty slots               sizeof(long)
	M keys                          M * sizeof(K)
	M + 1 pointers                  (M+1)*sizeof(long)  (i.e. block numbers)

                                    Block (internal node)
                                    ---------------------
	Node-type identifier            1 byte          0: Internal, 1: Leaf
	Parent node block no.           sizeof(long)
	No of empty slots               sizeof(int)
	M keys                          M * sizeof(K)
	M + 1 pointers                  (M+1)*sizeof(long) block numbers

                                    Block (leaf node)
                                    -----------------
	Node-type identifier            1 byte          0: Internal, 1: Leaf
	Prev - block no                 sizeof(long)
	Next - block no                 sizeof(long)
	Parent node block no.           sizeof(long)
	No. of empty slots              sizeof(long)
	M keys                          M * sizeof(K)
	M + 1 pointers                  M+1 * (sizeof(long) + sizeof(offset)
                                    (i.e. block nos and offsets but in a different file)

*/

struct blockOffsetPair{
	blocknum_t block_number;
	offset_t offset;
};

template <typename K, typename V, typename CompareFn>
class BTreeNode {

protected:
	long M;
	blocknum_t block_number;
	bool isRoot;
	std::list<K> keys;
	CompareFn cmpl;

	virtual BTreeNode<K, V, CompareFn>* splitChild(const K&);

public:
	BTreeNode(blocknum_t block_number, long M): block_number(block_number), M(M) {
	};

	//virtual V& findInNode(const K&);
	~BTreeNode();

	bool isSplitNeededForAdd() {
		return (curr_keys == M - 1);
	};

	virtual bool isLeaf();
	virtual void addToNode(const K&);
	virtual void deleteFromNode(const K&);

	// methods for comparing keys
	bool eq(const K& k1, const K& k2) {
		return !(cmpl(k1, k2) && cmpl(k2, k1));
	}

	bool neq(const K& k1, const K& k2) {
		return !(eq(k1, k2));
	}



	// this method checks if the child where the key K goes
	// after insert requires a split or not
	bool isSplitNeededForAdd(const K&);

	// Will use the BufferedFrameReader's static method to read the disk_block and
	// set approriate values
	void readNodeFromDisk(BufferFrame*);

	// Will use the BufferedFrameWriter's static method to change the disk_block
	void writeNodetoDisk(BufferFrame*);
	
	// Returns true if no. of keys present == M
	bool isFull(){ return (keys.size() == M) }

	// Finds Median of existing Key and new_key
	K& findMedian();
};

template <typename K, typename V, typename CompareFn>
class InternalNode : BTreeNode<K, V, CompareFn> {
private:
	long M;
	std::vector<blocknum_t> child_block_numbers;

public:
	InternalNode(blocknum_t block_number, long M, bool _isRoot = false) {
		BTreeNode<K, V, CompareFn>(block_number, M);
		this->isRoot = _isRoot;
	};

	blocknum_t findInNode(const K&);	//returns block number of appropriate child node
	void addToNode(const K&);
	void addToNode(const K&, blocknum_t);
	void deleteFromNode(const K&);
	bool isLeaf(){ return false; }
};

template <typename K, typename V, typename CompareFn>
class TreeLeafNode : BTreeNode<K, V, CompareFn> {
private:
	//array of block, offset pairs
	std::list<blockOffsetPair> value_node_address;
public:
	TreeLeafNode(blocknum_t block_number, long M, bool _isRoot = false) {
		BTreeNode<K, V, CompareFn>(block_number, M);
		this->isRoot = _isRoot;
	};

	blockOffsetPair findInNode(const K&);
	void addToNode(const K&, const V&); //TODO: two arguments or one argument as item <K, V, CompareFn>
	void addToNode(const K&, blockOffsetPair);
 	void deleteFromNode(const K&);

    bool isLeaf(){ return true; }
};

/**
	Some of the fields available in header of BufferedFile

    const size_type key_size;
	const size_type value_size;
	const string Identifier = "BTREE";

	These can be accessed as :
	$buffered_file_object->header->key_size;
*/

template <typename K, typename V, typename CompareFn>
class BTree {

private:
	const size_t Node_type_identifier_size = 1; // in bytes

	BufferedFile* buffered_file_internal;
	BufferedFile* buffered_file_data;

	//bool isRootLeaf;
	BTreeNode<K, V, CompareFn>* root;
	size_type sz;
	size_type blocksize;
	long M; // Maximum M keys in the internal nodes

	CompareFn cmpl;

	int calculateM(const size_t blocksize, const size_t key_size);

	BTreeNode<K, V, CompareFn> * getNodeFromBlockNum(blocknum_t);
	/* makes an InternalNode : if first byte of block is 0
	   makes a 	LeafNode     : if fisrt byte of block is 1
	*/

public:
	BTree(const char* pathname, size_type _blocksize) : blocksize(_blocksize), sz(0) {
		buffered_file_internal = new BufferedFile(pathname, blocksize);

		// get data file
		buffered_file_data = new BufferedFile(strcat(pathname, "_data"), sizeof(V));

		// will write it to the appropriate position in the header
		buffered_file_internal->header->setDataFileName(
			strcat(pathname, "_data")
		);

		root = nullptr;
		M = calculateM(blocksize, sizeof(K));

		// this would have read the header from the file and set all the fields accordingly
		// in the header (might have to make header a friend class of BTree)
		// Two possibilities:
		// 1. if the file was made previously, the header is read and values set in mem
		// 2. if file was not previously used, header class reads required values from BTree class (friend)
		// and then writes it to the header block on the file
		buffered_file_internal->header->init();

		// now ready for operation
	}

	~BTree() {
		delete buffered_file_internal;
		delete buffered_file_data;
	}

	V& searchElem(const K& key);
	void insertElem(const K& key, const V& value);
	void deleteElem(const K& key);
	void clearTree();

	size_type size() {
		return sz;
	}
};

template <typename K, typename V, typename CompareFn>
int BTree<K, V, CompareFn>::calculateM(const size_t blocksize, const size_t key_size) {

	/*
		sizeof(Node-type identifier) = 1 byte (A)
		sizeof(block_number) = sizeof(long)
		sizeof(no_of_empty_slots) = sizeof(long)
		sizeof(keys) = from user’s input or Sizeof(K)
		sizeof(block_number_data_file) = sizeof(long)
		sizeof(offset) = size we might restrict the block size to a fixed no. BL_MAX,
		we can always get the offset size to be (Log2(BL_MAX)/ 8) OR
		for now sizeof(int)

		Block_size = we know already as supplied by the user.

		So, M can be found out as:
		Block_size = sizeof(Node-type identifier)
		+ sizeof(block_number) + sizeof(No of empty slots)
		+ M*sizeof(keys) + (M+1)*(sizeof(block_number) + sizeof(offset))

		find the greatest integer M that satisfies the above relation.
	*/

	int long_size = sizeof(long);
	int int_size = sizeof(int);

	int M = (blocksize - Node_type_identifier_size - 3 * long_size - int_size)
		/ (int_size + long_size + key_size);

	return M;
};

template <typename K, typename V, typename CompareFn>
V& BTree<K, V, CompareFn>::searchElem(const K& search_key) {
	blockOffsetPair valueAddr;
	blocknum_t next_block_num;

	// Node for traversing the tree
	BTreeNode<K, V, CompareFn>* head = root;

	while(! head.isLeaf()){

		// always called on an internal node
		next_block_num = head->findInNode(search_key);
		if(next_block_num == NULL_BLOCK) return nullptr;

		BTreeNode<K, V, CompareFn>* next_node = getNodeFromBlockNum(next_block_num);
		head = next_node;
	}

	// always called on a leaf node - returns a block-offset pair
	valueAddr = head->findInNode(search_key);

	// i.e. the key is not found
	if (valueAddr.block_number == NULL_BLOCK
		|| valueAddr.offset == NULL_OFFSET
	) {
		return nullptr;
	}

	// otherwise read the value from the block in data file and return
	BufferFrame* buff = buffered_file_data->readBlock(valueAddr.block_number);
	return BufferedFrameReader::readPtr<V>(buff, valueAddr.offset);
};


template <typename K, typename V, typename CompareFn>
BTreeNode<K, V, CompareFn>* BTree<K, V, CompareFn>::getNodeFromBlockNum(blocknum_t block_number) {

	BufferFrame* buff = buffered_file_internal->readBlock(block_number);
	BTreeNode<K, V, CompareFn>* new_node = nullptr;

	// read 1st byte from buff
	bool isLeaf = BufferedFrameReader::read<bool>(buff, 0);

	if (isLeaf) {
		new_node = new TreeLeafNode<K, V, CompareFn>(block_number, M);
		// read other properties from block and set them
	} else {
		new_node = new InternalNode<K, V, CompareFn>(block_number, M);
		// read other properties from block and set them
	}

	return new_node;
};

template <typename K, typename V, typename CompareFn>
blocknum_t InternalNode<K, V, CompareFn>::findInNode(const K& find_key) {
	typename std::list<K>::const_iterator key_iter;
	typename std::list<blocknum_t>::const_iterator block_iter;

	for (
		key_iter = this->keys.begin(), block_iter = this->child_block_number.begin();
		key_iter != (this->keys).end();
		key_iter++, block_iter++
	){
		if (cmpl(*key_iter, find_key)) {
			return *block_iter;
		}
	}

	return *block_iter;
};

template <typename K, typename V, typename CompareFn>
blockOffsetPair TreeLeafNode<K, V, CompareFn>::findInNode(const K& find_key) {
	typename std::list<K>::const_iterator key_iter;
	typename std::list<blockOffsetPair>::const_iterator block_iter;
	blockOffsetPair reqd_block;

	for (
		key_iter = this->keys.begin(), block_iter = this->value_node_address.begin();
		key_iter != (this->keys).end();
		key_iter++, block_iter++
	){
		if (eq(*key_iter, find_key)) {
			reqd_block.block_number = (*block_iter).block_number;
			reqd_block.offset = (*block_iter).offset;
			return reqd_block;
		}
	}

	reqd_block.block_number = NULL_BLOCK;
	reqd_block.offset = NULL_OFFSET;

	return reqd_block;
};


template <typename K, typename V, typename CompareFn>
void BTree<K, V, CompareFn>::insertElem(const K& new_key, const V& new_value) {
	BufferFrame* disk_block;
	BTreeNode<K, V> * next_node;

	if (this->root == nullptr) {
		// i.e. 'first' insert in the tree
		blocknum_t root_block_num = buffered_file_internal->header->root_block_num;
		disk_block = buffered_file_internal->readBlock(root_block_num);
		this->root = new TreeLeafNode<K, V, CompareFn>(root_block_num, this->M, true);
		this->root->addToNode(new_key, new_value);

		this->root->writeNodetoDisk(disk_block);
	} else {

		blockOffsetPair valueAddr;
		blocknum_t next_block_num;

		// Node for traversing the tree
		BTreeNode<K, V, CompareFn>* trav = this->root;

		// split the root if needed and make a new root

		if (this->root->isFull()) {
			blocknum_t new_root_bnum = buffered_file_internal->allotBlock();
			BTreeNode<K, V>* t = new TreeLeafNode(new_root_bnum, this->M, true);
			//trav = t->splitChild();
			this->root = t;
			//TODO: Need to make changes. Need to think
		}

		BTreeNode<K, V, CompareFn>* next_node;

		while(trav && ! trav->isLeaf()) {

			next_block_num = trav->findInNode(K);
			next_node = getNodeFromBlockNum(next_block_num);

			// pro-active splitting
			if (next_node->isFull()) {
				// returns the proper next node
			} else {
				next_block_num = trav->findInNode(new_key);
				next_node = getNodeFromBlockNum(next_block_num);
				delete trav;
				trav = trav->splitChild(*next_node, new_key);
			}
		}

		// always called on a leaf node - returns a block-offset pair
		valueAddr = trav->findInNode(new_key);

		// i.e. the key is not found
		if (valueAddr.block_number == NULL_BLOCK
			|| valueAddr.offset == NULL_OFFSET
		) {
			// key not present, so add the key and the value
			trav->addToNode(new_key, new_value);
		} else {
			// key already exists, just update the value
			trav->updateValue(new_key, new_value);
		}
	}
};

<<<<<<< HEAD
template <typename K, typename V, typename CompareFn>
BTreeNode<K, V, CompareFn>* BTreeNode<K, V, CompareFn>::splitChild(const K& new_key) {
	typename std::list<K>::const_iterator key_iter;
	typename std::list<blocknum_t>::const_iterator block_iter;
	typename std::list<blockOffsetPair>::const_iterator blockOffset_iter;

	BTreeNode<K, V, CompareFn>* new_node;
	BufferFrame* disk_block;
	blocknum_t new_block_num;
	K temp_key;
	blocknum_t temp_block;
	blockOffsetPair temp_block_offset;

	new_block_num = buffered_file_internal->allotBlock();
	disk_block_new = buffered_file_internal->readBlock(new_block_num);
	disk_block_old = buffered_file_internal->readBlock(this->block_number);

	if (this->isLeaf()) {
		new_node = new TreeLeafNode<K, V, CompareFn>(new_block_num, this->M, this->isRoot);

		this->addToNode(new_key);
		for(
			int i = 0, key_iter = this->keys.begin(), blockOffset_iter = this->value_node_address.begin();
			i <= (this->keys).size() / 2, key_iter != (this->keys).end();
			key_iter++, blockOffset_iter++
		) {
			temp_key = *key_iter; 
			temp_block_offset.block_number = *blockOffset_iter.block_number;
			temp_block_offset.offset = *blockOffset_iter.offset;
			new_node->addToNode(temp_key, temp_block);
			this->deleteFromNode(temp_key);
			this->writeNodePropToFrame(disk_block_old);
			new_node->writeNodePropToFrame(disk_block_new);
=======
template <typename K, typename V>
K& BTreeNode<K, V>::findMedian(){
	//TODO: write median definition
}

template <typename K, typename V>
BTreeNode<K, V>* splitChild(const BTreeNode<K,V>& child_to_split) {

	//In this method this refers to the node that initiated the split on its child child_to_split

	new_block_num = buffered_file_internal->allotBlock();
	disk_block_new = buffered_file_internal->readBlock(new_block_num);
	K median_key = findMedian();

	


	//disk_block_old = buffered_file_internal->readBlock(this->block_number);

	// if (this->isLeaf()) {
	// 	new_node = new TreeLeafNode<K, V>(new_block_num, this->M, this->isRoot);

	// 	this->addToNode(new_key);
	// 	for(
	// 		int i = 0, key_iter = this->keys.begin(), block_iter = this->value_node_address.begin();
	// 		i <= (this->keys).size() / 2, key_iter != (this->keys).end();
	// 		key_iter++, block_iter++
	// 	) {
	// 		new_node->addToNode(*key_iter, *block_iter);
	// 		this->deleteFromNode(*key_iter, *block_iter);
	// 		this->writeNodetoDisk(disk_block_old);
	// 		new_node->writeNodetoDisk(disk_block_new);
	// 	}
	// } else {
	// 	new_node = new InternalNode<K, V>(new_block_num, this->M, this->isRoot);

	// 	for(
	// 		int i = 0, key_iter = this->keys.begin(), block_iter = this->child_block_numbers.begin();
	// 		i <= (this->keys).size() / 2, key_iter != (this->keys).end();
	// 		key_iter++, block_iter++
	// 	) {
	// 		new_node->addToNode(*key_iter, *block_iter);
	// 		this->deleteFromNode(*key_iter, *block_iter);
	// 		this->writeNodetoDisk(disk_block_old);
	// 		new_node->writeNodetoDisk(disk_block_new);
	// 	}
	// }



	if(child_to_split.isLeaf()){
		//Removed this->isRoot as root is never child of anyone
		new_node = new TreeLeafNode<K, V>(new_block_num, this->M);

		//typename std::list<K>::const_iterator new_key_iter;
		//typename std::list<blocknum_t>::const_iterator new_block_iter;

		typename std::list<K>::const_iterator key_iter;
		typename std::list<blockOffsetPair>::const_iterator block_iter;

		//distribute keys
		for (
			old_key_iter = child_to_split->keys.begin(), old_block_iter = child_to_split->value_node_address.begin();
			old_key_iter != (child_block_number->keys).end();
			old_key_iter++, old_block_iter++
			){
			if(*old_key_iter == median_key){
				//CRITICAL: MEDIAN KEY MUST BE ADDED TO NEW NODE IN CASE OF LEAF NODES
				new_node->keys.push_back(*old_key_iter);
				new_node->value_node_address.push_back(*old_block_iter);
				//remove median from old node
				//need to think about remove as it affects the iterator
				//which can cause problems

			} else if(*old_key_iter > median_key){
				new_node->keys.push_back(*old_key_iter);
				new_node->value_node_address.push_back(*old_block_iter);
				//remove the key and block from oldnode
				//need to think about remove as it affects the iterator
				//which can cause problems
			}

>>>>>>> 124b4cf... Fixed datatypes. Changed insertElem method. added redistribution code to splitChild
		}


		//TODO: adjust pointers and keys in "this" node
		this->addToNode(median_key);

	} else {
<<<<<<< HEAD
		new_node = new InternalNode<K, V, CompareFn>(new_block_num, this->M, this->isRoot);

		for(
			int i = 0, key_iter = this->keys.begin(), block_iter = this->child_block_numbers.begin();
			i <= (this->keys).size() / 2, key_iter != (this->keys).end();
			key_iter++, block_iter++
		) {
			temp_key = *key_iter; temp_block = *block_iter;
			new_node->addToNode(temp_key, temp_block);
			this->deleteFromNode(temp_key);
			this->writeNodePropToFrame(disk_block_old);
			new_node->writeNodePropToFrame(disk_block_new);
=======
		//Removed this->isRoot as root is never child of anyone
		new_node = new InternalNode<K, V>(new_block_num, this->M);


		typename std::list<K>::const_iterator key_iter;
		typename std::list<blocknum_t>::const_iterator block_iter;

		//distribute keys
		for (
			old_key_iter = child_to_split->keys.begin(), old_block_iter = child_to_split->child_block_numbers.begin();
			old_key_iter != (child_to_split->keys).end();
			old_key_iter++, old_block_iter++
			){
			if(*old_key_iter == median_key){
				//remove median from old node
				//need to think about remove as it affects the iterator
				//which can cause problems

			} else if(*old_key_iter > median_key){
				new_node->keys.push_back(*old_key_iter);
				new_node->child_block_numbers.push_back(*old_block_iter);
				//remove the key and block from oldnode
				//need to think about remove as it affects the iterator
				//which can cause problems
			}

>>>>>>> 124b4cf... Fixed datatypes. Changed insertElem method. added redistribution code to splitChild
		}
		//add the rightmost block number to new node
		new_node->child_block_numbers.push_back(*old_block_iter)

		//TODO: remove last remaining block number from old node


		//TODO: adjust pointers and keys in "this" node
	}

	//adjust keys and pointers in node
	return new_node;
};
