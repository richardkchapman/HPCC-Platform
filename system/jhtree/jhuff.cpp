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
#include "jhuff.hpp"
#include <cstdint>
#include <memory>
#include <queue>
#include <vector>

// Some code below originally inspired by code at https://github.com/nayuki/Reference-Huffman-coding

class HuffCodeTreeNode
{
public:
    bool isBranch = false;
    byte depth = 0;
};

class HuffCodeTreeLeaf final : public HuffCodeTreeNode
{
friend class HuffBuilder;
private:
    unsigned symbol = 0;	 // index into the provided SymbolTable object
public:
    HuffCodeTreeLeaf() = default;
};

class HuffCodeTreeBranch final : public HuffCodeTreeNode 
{
    friend class HuffBuilder;
private:
    HuffCodeTreeNode *left = nullptr;
    HuffCodeTreeNode *right = nullptr;
    
public: 
    HuffCodeTreeBranch() = default;
    void init(HuffCodeTreeNode *_left, HuffCodeTreeNode *_right, unsigned _depth = 0)
    {
        isBranch = true;
        left = _left;
        right = _right;
        depth = _depth;
    }
};

class StringSymbolTable : public CInterfaceOf<IHuffSymbolTable>
{
    struct StringEntry
    {
        StringAttr symbol;
        unsigned code = 0;
        byte codeLength = 0;     // in bits
        inline void setCode(unsigned _codeLength, unsigned _code)
        {
            codeLength = _codeLength;
            code = _code;
        }
        void dump() const
        {
            StringBuffer bincode;
            unsigned v = code;
            for (unsigned i = 0; i < codeLength; i++)
            {
                bincode.insert(0, (char) ('0' + (v & 1)));
                v >>= 1;
            }
            DBGLOG("%s codelength %u code %s", symbol.str(), codeLength, bincode.str());
        }
        StringEntry(const char *sym) : symbol(sym) {}
    };
    std::vector<StringEntry> symbols;
public:
    StringSymbolTable() = default;

    virtual unsigned numSymbols() const override { return symbols.size(); };
    virtual void setCode(unsigned symidx, unsigned codeLen, unsigned code) override { symbols.at(symidx).setCode(codeLen, code); };
    virtual void dump(unsigned idx) const override { symbols.at(idx).dump(); }
    virtual void dumpAll() const override { for (auto &s: symbols) { s.dump(); } }

    void addSymbol(const char *sym) { symbols.emplace_back(StringEntry(sym)); }
};

class HuffBuilder : public CInterfaceOf<IHuffBuilder>
{
private:
    void setDepths(HuffCodeTreeNode *node, unsigned depth);
    void setCodes(HuffCodeTreeNode *node, unsigned depth, unsigned code);
    void assignCodes(uint64_t *counts);
    HuffCodeTreeLeaf *leaves = nullptr;
    HuffCodeTreeBranch *branches = nullptr;
    HuffCodeTreeNode *root = nullptr;
    Linked<IHuffSymbolTable> symbols;
public:
    HuffBuilder(IHuffSymbolTable *_symbols, uint64_t *counts);
    ~HuffBuilder();
};

HuffBuilder::HuffBuilder(IHuffSymbolTable *_symbols, uint64_t *frequencies)
: symbols(_symbols)
{
    assignCodes(frequencies);
}

HuffBuilder::~HuffBuilder()
{
    delete [] leaves;
    delete [] branches;
}

void HuffBuilder::setDepths(HuffCodeTreeNode *node, unsigned depth)
{
    node->depth = depth;
    if (node->isBranch)
    {
        const HuffCodeTreeBranch *branch = static_cast<const HuffCodeTreeBranch *>(node);
        setDepths(branch->left, depth+1);
        setDepths(branch->right, depth+1);
    }
}

void HuffBuilder::setCodes(HuffCodeTreeNode *node, unsigned depth, unsigned code)
{
    assert(node->depth == depth);
    if (node->isBranch)
    {
        const HuffCodeTreeBranch *branch = static_cast<const HuffCodeTreeBranch *>(node);
        setCodes(branch->left, depth+1, code << 1 | 0);
        setCodes(branch->right, depth+1, code << 1 | 1);
    }
    else
    {
        const HuffCodeTreeLeaf *leaf = static_cast<const HuffCodeTreeLeaf *>(node);
        symbols->setCode(leaf->symbol, depth, code);
    }
}

void HuffBuilder::assignCodes(uint64_t *frequencies)
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
    leaves = new HuffCodeTreeLeaf[symCount];
    for (unsigned i = 0; i < symCount; i++) 
    {
        leaves[i].symbol = i;
        uint64_t freq = frequencies[i];
        assertex(freq);
        pqueue.push(NodeBuilder(&leaves[i], i, freq));
    }

    unsigned branchIdx = 0;
    branches = new HuffCodeTreeBranch[symCount-1];

    // Repeatedly tie together two nodes with the lowest frequency
    while (pqueue.size() > 1) 
    {
        NodeBuilder x = NodeBuilder::popQueue(pqueue);
        NodeBuilder y = NodeBuilder::popQueue(pqueue);
        branches[branchIdx].init(x.node, y.node);
        pqueue.emplace(NodeBuilder(&branches[branchIdx], std::min(x.lowSym, y.lowSym), x.count + y.count));
        branchIdx++;
    }
    NodeBuilder head = NodeBuilder::popQueue(pqueue);
    // Walk the tree assigning code lengths to symbols (not codes, initially, as we want to create canonical codes)
    setDepths(head.node, 0);
    // Now that all leaves have code lengths, we can create a canonical code tree from them
    // Sort the leaves by codeLength
    std::sort(leaves, leaves+symCount, [](HuffCodeTreeLeaf &a, HuffCodeTreeLeaf &b) 
    {
        if (a.depth > b.depth) return true; 
        if (a.depth == b.depth) return a.symbol < b.symbol;
        return false; 
    });
    // Iterate through all the leaves and branches at a given level, creating branches at the next level
    HuffCodeTreeLeaf *curLeaf = &leaves[0];
    HuffCodeTreeBranch *curBranch = &branches[0]; // note - we reuse the previous branches
    unsigned level = curLeaf->depth;
    branchIdx = 0;
    while (level)
    {
        HuffCodeTreeBranch *branchEnd = branches+branchIdx;  // Note the end of the branches created so far
        // we consume 2 at the current level and merge them to new node level-1
        // These will be from leaves if available, else branches
        while (curLeaf < leaves+symCount)
        {
            if (curLeaf->depth != level)
                break;
            if (curLeaf[1].depth != level)
            {
                assert(curBranch->depth == level);
                branches[branchIdx++].init(curLeaf, curBranch, level-1);
                curLeaf++;
                curBranch++;
                break;
            }
            branches[branchIdx++].init(curLeaf, curLeaf+1, level-1);
            curLeaf += 2;
        }
        while (curBranch < branchEnd)
        {
            assertex(curBranch->depth == level);
            assertex(curBranch[1].depth == level); // We should never end up with a 'spare' branch
            branches[branchIdx++].init(curBranch, curBranch+1, level-1);
            curBranch += 2;
        }
        level--;
    }
    // The root of the newly-created tree is at branches[branchIdx-1];
    root = &branches[branchIdx-1];
    setCodes(root, 0, 0);
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
//    { "JAMES", 0.00, 1000000 },
  //  { "JOHN", 0.00, 1000000 },
    //{ "RICHARD", 0.00, 300000 },
    #include "names"
};

class JHuffTest : public CppUnit::TestFixture  
{
    CPPUNIT_TEST_SUITE( JHuffTest  );
        CPPUNIT_TEST(huffTest);
    CPPUNIT_TEST_SUITE_END();

    void huffTest()
    {
        StringSymbolTable symbols;
        uint64_t frequencies[std::extent<decltype(usNames)>::value];
        unsigned idx = 0;
        for (auto &n: usNames)
        {
            symbols.addSymbol(n.name);
            frequencies[idx++] = n.count;
        }
        HuffBuilder builder(&symbols, frequencies);
        symbols.dumpAll();
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( JHuffTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( JHuffTest, "JHuffTest" );

#endif
