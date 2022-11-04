/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2022 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "platform.h"
#include "jstring.hpp"
#include "jlog.hpp"
#include <cstdint>
#include <memory>
#include <queue>
#include <vector>

/* 
 * A node in a code tree. This class has exactly two subclasses: InternalNode, Leaf.
 */
class HuffCodeTreeNode : public CInterface
{
};

/* 
 * A leaf node in a code tree. It has a symbol value.
 */
class HuffCodeTreeLeaf final : public HuffCodeTreeNode
{
friend class CanonicalCode;
friend class HuffCodeTree;
private:
    unsigned symbol;	 // index into the input names table
public:
    explicit HuffCodeTreeLeaf(unsigned _symbol) : symbol(_symbol) {} 
};

/* 
 * An internal node in a code tree. It has two nodes as children.
 */
class HuffCodeTreeBranch final : public HuffCodeTreeNode 
{
    friend class HuffCodeTree;
    friend class CanonicalCode;
private:
    Owned<HuffCodeTreeNode> left;
    Owned<HuffCodeTreeNode> right;
	
public: 
    explicit HuffCodeTreeBranch(HuffCodeTreeNode *_left, HuffCodeTreeNode *_right) : left(_left), right(_right) {}
};


/* 
 * A binary tree that represents a mapping between symbols and binary strings.
 * The data structure is immutable. There are two main uses of a code tree:
 * - Read the root field and walk through the tree to extract the desired information.
 * - Call getCode() to get the binary code for a particular encodable symbol.
 * The path to a leaf node determines the leaf's symbol's code. Starting from the root, going
 * to the left child represents a 0, and going to the right child represents a 1. Constraints:
 * - The root must be an internal node, and the tree is finite.
 * - No symbol value is found in more than one leaf.
 * - Not every possible symbol value needs to be in the tree.
 * Illustrated example:
 *   Huffman codes:
 *     0: Symbol A
 *     10: Symbol B
 *     110: Symbol C
 *     111: Symbol D
 *   
 *   Code tree:
 *       .
 *      / \
 *     A   .
 *        / \
 *       B   .
 *          / \
 *         C   D
 */

class HuffCodeTree : public CInterface
{
	friend class CanonicalCode;
public:
	explicit HuffCodeTree(const HuffCodeTreeNode *_root, std::uint32_t symbolLimit);
	// Returns the Huffman code for the given symbol, which is a list of 0s and 1s.
	const std::vector<char> &getCode(std::uint32_t symbol) const;
	unsigned getBinaryCode(uint32_t symbol) const;

private:	
	// Recursive helper function for the constructor
	void buildCodeList(const HuffCodeTreeNode *node, std::vector<char> &prefix);
	
private:
    Owned<const HuffCodeTreeNode> root;
	// Stores the code for each symbol, or null if the symbol has no code.
	// For example, if symbol 5 has code 10011, then codes.get(5) is the list [1,0,0,1,1].
	std::vector<std::vector<char> > codes;

};

HuffCodeTree::HuffCodeTree(const HuffCodeTreeNode *_root, uint32_t symbolLimit) : root(_root) 
{
	codes = std::vector<std::vector<char> >(symbolLimit, std::vector<char>());  // Initially all empty
	std::vector<char> prefix;
	buildCodeList(root, prefix);  // Fill 'codes' with appropriate data
}


void HuffCodeTree::buildCodeList(const HuffCodeTreeNode *node, std::vector<char> &prefix) {
	if (dynamic_cast<const HuffCodeTreeBranch*>(node) != nullptr) {
		const HuffCodeTreeBranch *internalNode = dynamic_cast<const HuffCodeTreeBranch*>(node);
		
		prefix.push_back(0);
		buildCodeList(internalNode->left, prefix);
		prefix.pop_back();
		
		prefix.push_back(1);
		buildCodeList(internalNode->right, prefix);
		prefix.pop_back();
		
	} else if (dynamic_cast<const HuffCodeTreeLeaf*>(node) != nullptr) {
		const HuffCodeTreeLeaf *leaf = dynamic_cast<const HuffCodeTreeLeaf*>(node);
		if (leaf->symbol >= codes.size())
			throw std::invalid_argument("Symbol exceeds symbol limit");
		if (!codes.at(leaf->symbol).empty())
			throw std::invalid_argument("Symbol has more than one code");
		codes.at(leaf->symbol) = prefix;
		
	} else {
		throw std::logic_error("Assertion error: Illegal node type");
	}
}


const std::vector<char> &HuffCodeTree::getCode(uint32_t symbol) const {
	if (codes.at(symbol).empty())
		throw std::domain_error("No code for given symbol");
	else
		return codes.at(symbol);
}

unsigned HuffCodeTree::getBinaryCode(uint32_t symbol) const 
{
	unsigned val = 0;
	auto &code = getCode(symbol);
	for (auto b = code.rbegin(); b != code.rend(); ++b) 
	{
		val <<= 1;
		val = val | *b;
	}
	return val;
}

/* 
 * A table of symbol frequencies. Symbols values are numbered from 0 to symbolLimit-1.
 * A frequency table is mainly used like this:
 * 0. Collect the frequencies of symbols in the stream that we want to compress.
 * 1. Build a code tree that is statically optimal for the current frequencies.
 * This implementation is designed to avoid arithmetic overflow - it correctly
 * builds an optimal code tree for any legal number of symbols (2 to UINT32_MAX),
 * with each symbol having a legal frequency (0 to UINT32_MAX).
 */
class FrequencyTable final {
	
	/*---- Field and constructor ----*/
	
	// Length at least 2.
	private: std::vector<std::uint32_t> frequencies;
	
	
	// Constructs a frequency table from the given array of frequencies.
	// The array length must be at least 2, and each value must be non-negative.
	public: explicit FrequencyTable(const std::vector<std::uint32_t> &freqs);
	
	
	
	/*---- Basic methods ----*/
	
	// Returns the number of symbols in this frequency table. The result is always at least 2.
	public: std::uint32_t getSymbolLimit() const;
	
	
	// Returns the frequency of the given symbol in this frequency table.
	public: std::uint32_t get(std::uint32_t symbol) const;
	
	
	// Sets the frequency of the given symbol in this frequency table to the given value.
	public: void set(std::uint32_t symbol, std::uint32_t freq);
	
	
	// Increments the frequency of the given symbol in this frequency table.
	public: void increment(std::uint32_t symbol);
	
	
	
	/*---- Advanced methods ----*/
	
	// Returns a code tree that is optimal for the symbol frequencies in this table.
	// The tree always contains at least 2 leaves (even if they come from symbols with
	// 0 frequency), to avoid degenerate trees. Note that optimal trees are not unique.
	public: HuffCodeTree *buildCodeTree() const;
	
	
	// Helper structure for buildCodeTree()
	private: class NodeWithFrequency {
		
		public: HuffCodeTreeNode *node;
		public: std::uint32_t lowestSymbol;
		public: std::uint64_t frequency;  // Using wider type prevents overflow
		
		
		public: explicit NodeWithFrequency(HuffCodeTreeNode *nd, std::uint32_t lowSym, std::uint64_t freq);
		
		
		// Sort by ascending frequency, breaking ties by ascending symbol value.
		public: bool operator<(const NodeWithFrequency &other) const;
		
	};
	
	
	private: static NodeWithFrequency popQueue(std::priority_queue<NodeWithFrequency> &pqueue);
	
};

FrequencyTable::FrequencyTable(const std::vector<uint32_t> &freqs) :
		frequencies(freqs) {
	if (freqs.size() < 2)
		throw std::invalid_argument("At least 2 symbols needed");
	if (freqs.size() > UINT32_MAX)
		throw std::length_error("Too many symbols");
}


uint32_t FrequencyTable::getSymbolLimit() const {
	return static_cast<uint32_t>(frequencies.size());
}


uint32_t FrequencyTable::get(uint32_t symbol) const {
	return frequencies.at(symbol);
}


void FrequencyTable::set(uint32_t symbol, uint32_t freq) {
	frequencies.at(symbol) = freq;
}


void FrequencyTable::increment(uint32_t symbol) {
	if (frequencies.at(symbol) == UINT32_MAX)
		throw std::overflow_error("Maximum frequency reached");
	frequencies.at(symbol)++;
}


HuffCodeTree *FrequencyTable::buildCodeTree() const {
	// Note that if two nodes have the same frequency, then the tie is broken
	// by which tree contains the lowest symbol. Thus the algorithm has a
	// deterministic output and does not rely on the queue to break ties.
	std::priority_queue<NodeWithFrequency> pqueue;
	
	// Add leaves for symbols with non-zero frequency
	{
		uint32_t i = 0;
		for (uint32_t freq : frequencies) {
			if (freq > 0)
				pqueue.push(NodeWithFrequency(new HuffCodeTreeLeaf(i), i, freq));
			i++;
		}
	}
	
	// Pad with zero-frequency symbols until queue has at least 2 items
	{
		uint32_t i = 0;
		for (uint32_t freq : frequencies) {
			if (pqueue.size() >= 2)
				break;
			if (freq == 0)
				pqueue.push(NodeWithFrequency(new HuffCodeTreeLeaf(i), i, freq));
			i++;
		}
	}
	assert(pqueue.size() >= 2);
	
	// Repeatedly tie together two nodes with the lowest frequency
	while (pqueue.size() > 1) {
		NodeWithFrequency x = popQueue(pqueue);
		NodeWithFrequency y = popQueue(pqueue);
		pqueue.push(NodeWithFrequency(
			new HuffCodeTreeBranch(x.node, y.node),
			std::min(x.lowestSymbol, y.lowestSymbol),
			x.frequency + y.frequency));
	}
	
	// Return the remaining node
	NodeWithFrequency temp = popQueue(pqueue);
	HuffCodeTreeBranch *root = dynamic_cast<HuffCodeTreeBranch *>(temp.node);
	return new HuffCodeTree(root, getSymbolLimit());
}


FrequencyTable::NodeWithFrequency::NodeWithFrequency(HuffCodeTreeNode *_node, uint32_t lowSym, uint64_t freq) :
	node(_node),
	lowestSymbol(lowSym),
	frequency(freq) {}


bool FrequencyTable::NodeWithFrequency::operator<(const NodeWithFrequency &other) const {
	if (frequency > other.frequency)
		return true;
	else if (frequency < other.frequency)
		return false;
	else if (lowestSymbol > other.lowestSymbol)
		return true;
	else if (lowestSymbol < other.lowestSymbol)
		return false;
	else
		return false;
}


FrequencyTable::NodeWithFrequency FrequencyTable::popQueue(std::priority_queue<NodeWithFrequency> &pqueue) {
	FrequencyTable::NodeWithFrequency result = std::move(const_cast<NodeWithFrequency&&>(pqueue.top()));
	pqueue.pop();
	return result;
}

/* 
 * A canonical Huffman code, which only describes the code length of
 * each symbol. Immutable. Code length 0 means no code for the symbol.
 * The binary codes for each symbol can be reconstructed from the length information.
 * In this implementation, lexicographically lower binary codes are assigned to symbols
 * with lower code lengths, breaking ties by lower symbol values. For example:
 *   Code lengths (canonical code):
 *     Symbol A: 1
 *     Symbol B: 3
 *     Symbol C: 0 (no code)
 *     Symbol D: 2
 *     Symbol E: 3
 *   
 *   Sorted lengths and symbols:
 *     Symbol A: 1
 *     Symbol D: 2
 *     Symbol B: 3
 *     Symbol E: 3
 *     Symbol C: 0 (no code)
 *   
 *   Generated Huffman codes:
 *     Symbol A: 0
 *     Symbol D: 10
 *     Symbol B: 110
 *     Symbol E: 111
 *     Symbol C: None
 *   
 *   Huffman codes sorted by symbol:
 *     Symbol A: 0
 *     Symbol B: 110
 *     Symbol C: None
 *     Symbol D: 10
 *     Symbol E: 111
 */
class CanonicalCode final : public CInterface {
	
	/*---- Field ----*/
	
	private: std::vector<std::uint32_t> codeLengths;
	
	
	
	/*---- Constructors ----*/
	
	// Constructs a canonical Huffman code from the given array of symbol code lengths.
	// Each code length must be non-negative. Code length 0 means no code for the symbol.
	// The collection of code lengths must represent a proper full Huffman code tree.
	// Examples of code lengths that result in under-full Huffman code trees:
	// - [1]
	// - [3, 0, 3]
	// - [1, 2, 3]
	// Examples of code lengths that result in correct full Huffman code trees:
	// - [1, 1]
	// - [2, 2, 1, 0, 0, 0]
	// - [3, 3, 3, 3, 3, 3, 3, 3]
	// Examples of code lengths that result in over-full Huffman code trees:
	// - [1, 1, 1]
	// - [1, 1, 2, 2, 3, 3, 3, 3]
	public: explicit CanonicalCode(const std::vector<std::uint32_t> &codeLens);
	
	
	// Builds a canonical Huffman code from the given code tree.
	public: explicit CanonicalCode(const HuffCodeTree *tree, std::uint32_t symbolLimit);
	
	
	// Recursive helper method for the above constructor.
	private: void buildCodeLengths(const HuffCodeTreeNode *node, std::uint32_t depth);
	
	
	
	/*---- Various methods ----*/
	
	// Returns the symbol limit for this canonical Huffman code.
	// Thus this code covers symbol values from 0 to symbolLimit&minus;1.
	public: std::uint32_t getSymbolLimit() const;
	
	
	// Returns the code length of the given symbol value. The result is 0
	// if the symbol has node code; otherwise the result is a positive number.
	public: std::uint32_t getCodeLength(std::uint32_t symbol) const;
	
	
	// Returns the canonical code tree for this canonical Huffman code.
	public: HuffCodeTree *toCodeTree() const;
	
};

// CanonicalCode implementation

CanonicalCode::CanonicalCode(const std::vector<uint32_t> &codeLens) {
	// Check basic validity
	if (codeLens.size() < 2)
		throw std::invalid_argument("At least 2 symbols needed");
	if (codeLens.size() > UINT32_MAX)
		throw std::length_error("Too many symbols");
	
	// Copy once and check for tree validity
	codeLengths = codeLens;
	std::sort(codeLengths.begin(), codeLengths.end(), std::greater<uint32_t>());
	uint32_t currentLevel = codeLengths.front();
	uint32_t numNodesAtLevel = 0;
	for (uint32_t cl : codeLengths) {
		if (cl == 0)
			break;
		while (cl < currentLevel) {
			if (numNodesAtLevel % 2 != 0)
				throw std::invalid_argument("Under-full Huffman code tree");
			numNodesAtLevel /= 2;
			currentLevel--;
		}
		numNodesAtLevel++;
	}
	while (currentLevel > 0) {
		if (numNodesAtLevel % 2 != 0)
			throw std::invalid_argument("Under-full Huffman code tree");
		numNodesAtLevel /= 2;
		currentLevel--;
	}
	if (numNodesAtLevel < 1)
		throw std::invalid_argument("Under-full Huffman code tree");
	if (numNodesAtLevel > 1)
		throw std::invalid_argument("Over-full Huffman code tree");
	
	// Copy again
	codeLengths = codeLens;
}


CanonicalCode::CanonicalCode(const HuffCodeTree *tree, uint32_t symbolLimit) {
	if (symbolLimit < 2)
		throw std::invalid_argument("At least 2 symbols needed");
	codeLengths = std::vector<uint32_t>(symbolLimit, 0);
	buildCodeLengths(tree->root, 0);
}


void CanonicalCode::buildCodeLengths(const HuffCodeTreeNode *node, uint32_t depth) {
	if (dynamic_cast<const HuffCodeTreeBranch *>(node) != nullptr) {
		const HuffCodeTreeBranch *internalNode = dynamic_cast<const HuffCodeTreeBranch *>(node);
		buildCodeLengths(internalNode->left, depth + 1);
		buildCodeLengths(internalNode->right, depth + 1);
	} else if (dynamic_cast<const HuffCodeTreeLeaf*>(node) != nullptr) {
		uint32_t symbol = dynamic_cast<const HuffCodeTreeLeaf*>(node)->symbol;
		if (symbol >= codeLengths.size())
			throw std::invalid_argument("Symbol exceeds symbol limit");
		// Note: CodeTree already has a checked constraint that disallows a symbol in multiple leaves
		if (codeLengths.at(symbol) != 0)
			throw std::logic_error("Assertion error: Symbol has more than one code");
		codeLengths.at(symbol) = depth;
	} else {
		throw std::logic_error("Assertion error: Illegal node type");
	}
}


uint32_t CanonicalCode::getSymbolLimit() const {
	return static_cast<uint32_t>(codeLengths.size());
}


uint32_t CanonicalCode::getCodeLength(uint32_t symbol) const {
	if (symbol >= codeLengths.size())
		throw std::domain_error("Symbol out of range");
	return codeLengths.at(symbol);
}


HuffCodeTree *CanonicalCode::toCodeTree() const {
	std::vector<HuffCodeTreeNode *> nodes;
	for (uint32_t i = *std::max_element(codeLengths.cbegin(), codeLengths.cend()); ; i--) {  // Descend through code lengths
		if (nodes.size() % 2 != 0)
			throw std::logic_error("Assertion error: Violation of canonical code invariants");
		std::vector<HuffCodeTreeNode * > newNodes;
		
		// Add leaves for symbols with positive code length i
		if (i > 0) {
			uint32_t j = 0;
			for (uint32_t cl : codeLengths) {
				if (cl == i)
					newNodes.push_back(new HuffCodeTreeLeaf(j));
				j++;
			}
		}
		
		// Merge pairs of nodes from the previous deeper layer
		for (std::size_t j = 0; j < nodes.size(); j += 2) 
        {
			newNodes.push_back(new HuffCodeTreeBranch(nodes.at(j), nodes.at(j + 1)));
		}
		nodes = std::move(newNodes);
		
		if (i == 0)
			break;
	}
	
	if (nodes.size() != 1)
		throw std::logic_error("Assertion error: Violation of canonical code invariants");
	
	HuffCodeTreeNode *temp = nodes.front();
	HuffCodeTreeBranch *root = dynamic_cast<HuffCodeTreeBranch*>(temp);
	return new HuffCodeTree(root, static_cast<uint32_t>(codeLengths.size()));
}


//======================================================================================================

interface HuffSymbolTable : public IInterface
{
	virtual unsigned numSymbols() const = 0;
	virtual void setCode(unsigned symidx, unsigned codeLen, unsigned code) = 0;
};

class StringSymbolTable : public CInterfaceOf<HuffSymbolTable>
{
	struct StringEntry
	{
		StringAttr symbol;
		unsigned code = 0;
		byte codelength = 0;     // in bits
		inline void setCode(unsigned _codelength, unsigned _code)
		{
			code = _code;
			codelength = _codelength;
		}
		void dump()
		{
			StringBuffer bincode;
			unsigned v = code;
			for (unsigned i = 0; i < codelength; i++)
			{
				bincode.appendf("%c", '0'+(v & 1));
				v >>= 1;
			}
			DBGLOG("%s codelength %u code %s", symbol.str(), codelength, bincode.str());
		}
		StringEntry(const char *sym) : symbol(sym) {}
	};
	std::vector<StringEntry> symbols;
public:
	StringSymbolTable() = default;

	virtual unsigned numSymbols() const override { return symbols.size(); };
	virtual void setCode(unsigned symidx, unsigned codeLen, unsigned code) override { symbols.at(symidx).setCode(codeLen, code); };

	void addSymbol(const char *sym) { symbols.emplace_back(StringEntry(sym)); }
	void dump()	{ for (auto &s: symbols) { s.dump(); } }
};

class HuffBuilder
{
private:

public:
	HuffBuilder() = default;
	void assignCodes(HuffSymbolTable *symbols, uint64_t *counts);
};

void HuffBuilder::assignCodes(HuffSymbolTable *symbols, uint64_t *frequencies)
{
	struct NodeBuilder
	{
		HuffCodeTreeNode *node;
		unsigned lowSym;
		uint64_t count;
		
		explicit NodeBuilder(HuffCodeTreeNode *_node, unsigned _lowSym, uint64_t _count) : node(_node), lowSym(_lowSym), count(_count) {}
		bool operator<(const NodeBuilder &other) const
		{
			if (count > other.count)
				return true;
			else if (count == other.count && lowSym > other.lowSym)
				return true;
			else
				return false;
		}
		static NodeBuilder popQueue(std::priority_queue<NodeBuilder> &pqueue) 
		{
			NodeBuilder result = std::move(const_cast<NodeBuilder&&>(pqueue.top()));
			pqueue.pop();
			return result;
		}
	};

	std::priority_queue<NodeBuilder> pqueue;
	unsigned symCount = symbols->numSymbols();
	for (unsigned i = 0; i < symCount; i++) 
	{
		uint64_t freq = frequencies[i];
		assertex(freq);
		pqueue.push(NodeBuilder(new HuffCodeTreeLeaf(i), i, freq));
	}
	
	// Repeatedly tie together two nodes with the lowest frequency
	while (pqueue.size() > 1) 
	{
		NodeBuilder x = NodeBuilder::popQueue(pqueue);
		NodeBuilder y = NodeBuilder::popQueue(pqueue);
		pqueue.emplace(NodeBuilder(new HuffCodeTreeBranch(x.node, y.node), std::min(x.lowSym, y.lowSym), x.count + y.count));
	}
	
	// Return the remaining node
	NodeBuilder head = NodeBuilder::popQueue(pqueue);
	Owned<HuffCodeTree> code = new HuffCodeTree(head.node, symCount);
	Owned<CanonicalCode> canonCode = new CanonicalCode(code, symCount);
	// Replace code tree with canonical one. For each symbol,
	// the code value may change but the code length stays the same.
	code.setown(canonCode->toCodeTree());
    for (uint32_t i = 0; i < canonCode->getSymbolLimit(); i++) 
    {
        uint32_t val = canonCode->getCodeLength(i);
		symbols->setCode(i, val, code->getBinaryCode(i));
	}
}

#ifdef _USE_CPPUNIT
#include "unittests.hpp"

struct NameInfo
{
    const char *name;
    double frequency;
    unsigned count;
};

constexpr NameInfo usNames[] = {
    { "OTHER", 0.00, 10000000 },
    #include "names"
};

class JHuffTest : public CppUnit::TestFixture  
{
    CPPUNIT_TEST_SUITE( JHuffTest  );
//        CPPUNIT_TEST(huffTest);
        CPPUNIT_TEST(newHuffTest);
    CPPUNIT_TEST_SUITE_END();

    void huffTest()
    {
        FrequencyTable freqs(std::vector<uint32_t>(5200, 0));
        unsigned idx = 0;
        for (auto &n: usNames)
        {
            freqs.set(idx++, n.count);
        }
        Owned<HuffCodeTree> code = freqs.buildCodeTree();
        Owned<CanonicalCode> canonCode = new CanonicalCode(code, freqs.getSymbolLimit());
        // Replace code tree with canonical one. For each symbol,
        // the code value may change but the code length stays the same.
        code.setown(canonCode->toCodeTree());

        unsigned long totbits = 0;
        unsigned long totbytes = 0;
        unsigned long totcount = 0;
        for (uint32_t i = 0; i < canonCode->getSymbolLimit(); i++) 
        {
            uint32_t val = canonCode->getCodeLength(i);
            totcount += usNames[i].count;
            totbits += usNames[i].count*val;
            totbytes += usNames[i].count*strlen(usNames[i].name);
            printf("%s code length %u ", usNames[i].name, val);
            for (char b : code->getCode(i))
                printf("%u", b);
            printf("\n");
        }
        unsigned long compressed = totbits/8;
        DBGLOG("For total population, count %lu, string length %lu, padded length=%lu, compressed = %lu", totcount, totbytes, totcount*20, compressed);
        DBGLOG("Compression ratio: %lu%% (%lu%% if space padded to 20 bytes)",(compressed*100)/totbytes, (compressed*100)/(totcount*20));
    }

    void newHuffTest()
    {
		StringSymbolTable symbols;
		uint64_t frequencies[std::extent<decltype(usNames)>::value];
		unsigned idx = 0;
        for (auto &n: usNames)
        {
            symbols.addSymbol(n.name);
			frequencies[idx++] = n.count;
        }
		HuffBuilder builder;
		builder.assignCodes(&symbols, frequencies);
		symbols.dump();
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( JHuffTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( JHuffTest, "JHuffTest" );

#endif
