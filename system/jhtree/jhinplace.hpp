/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2023 HPCC Systems®.

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

#ifndef JHINPLACE_HPP
#define JHINPLACE_HPP

#include "jiface.hpp"
#include "jhutil.hpp"
#include "hlzw.h"
#include "jcrc.hpp"
#include "jio.hpp"
#include "jfile.hpp"
#include "ctfile.hpp"

class InplaceNodeSearcher
{
public:
    InplaceNodeSearcher() = default;
    InplaceNodeSearcher(unsigned _count, const byte * data, size32_t _keyLen, const byte * _nullRow);
    void init(unsigned _count, const byte * data, size32_t _keyLen, const byte * _nullRow);

    //Find the first row that is >= the search row
    unsigned findGE(const unsigned len, const byte * search) const;
    size32_t getValueAt(unsigned int num, char *key) const;
    int compareValueAt(const char *src, unsigned int index) const;
    int compareValueAtFallback(const char *src, unsigned int index) const;

protected:
    const byte * nodeData = nullptr;
    const byte * nullRow = nullptr;
    size32_t count = 0;
    size32_t keyLen = 0;
};

//---------------------------------------------------------------------------------------------------------------------

class PartialMatchBuilder;
class PartialMatch : public CInterface
{
public:
    PartialMatch(PartialMatchBuilder * _builder, size32_t _len, const void * _data, unsigned _rowOffset, bool _isRoot)
    : builder(_builder), data(_len, _data), rowOffset(_rowOffset), isRoot(_isRoot)
    {
    }

    bool combine(size32_t newLen, const byte * newData, size32_t &common);
    bool removeLast();
    void serialize(MemoryBuffer & buffer);
    void serializeFirst(MemoryBuffer & out);
    bool squash();
    void trace(unsigned indent);
    void gatherSingleTextCounts(StringBuffer & prev, unsigned * counts);

    size32_t getSize();
    size32_t getCount();
    const byte * queryNullRow();
    bool isEnd() const { return data.length() == 0; }

protected:
    bool allNextAreEnd();
    unsigned appendRepeat(size32_t offset, size32_t copyOffset, byte repeatByte, size32_t repeatCount);
    void cacheSizes();
    void describeSquashed(StringBuffer & out);
    size32_t getMaxOffset();

protected:
    PartialMatchBuilder * builder;
    MemoryBuffer data;
    MemoryBuffer squashedData;
    CIArrayOf<PartialMatch> next;
    unsigned rowOffset;
    size32_t maxOffset = 0;
    size32_t size = 0;
    size32_t maxCount = 0;
    bool dirty = true;
    bool squashed = false;
    bool isRoot;
};


class PartialMatchBuilder
{
public:
    PartialMatchBuilder(size32_t _keyLen, const byte *_nullRow, bool _optimizeTrailing) : nullRow(_nullRow), keyLen(_keyLen), optimizeTrailing(_optimizeTrailing)
    {
        if (!nullRow)
            optimizeTrailing = false;
        //MORE: Options for #null bytes to include in a row, whether to squash etc.
    }

    size32_t add(size32_t len, const void * data);  // returns how many bytes of key matched previous row (MORE - would keylen - common be better)
    void removeLast();
    void serialize(MemoryBuffer & out);
    void trace();
    void gatherSingleTextCounts(unsigned * counts);

    size32_t queryKeyLen() const { return keyLen; }
    const byte * queryNullRow() const { return nullRow; }
    unsigned getCount();
    unsigned getSize();

public:
    unsigned numDuplicates = 0;

protected:
    Owned<PartialMatch> root;
    const byte * nullRow = nullptr;
    size32_t keyLen;
    bool optimizeTrailing = false;
};

//---------------------------------------------------------------------------------------------------------------------

class KeyBuildContext
{
public:
    ~KeyBuildContext() { delete [] nullRow; }

public:
    unsigned numKeyedDuplicates = 0;
    unsigned singleCounts[256] = { 0 };
    MemoryBuffer uncompressed;
    MemoryAttr compressed;
    const byte * nullRow = nullptr;
};

//---------------------------------------------------------------------------------------------------------------------

#define USE_ZSTD_COMPRESSION
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

class ZStdCDictionary : public CInterface
{
    ZSTD_CDict* cdict = nullptr;
public:
    ZStdCDictionary(const void* dictBuffer, size_t dictSize, int compressionLevel);
    operator const ZSTD_CDict *() const 
    {
        return cdict;
    }
    ~ZStdCDictionary();
};

class ZStdBlockCompressor : public CInterface
{
    // One context per thread
    ZSTD_CCtx *cctx = nullptr;
    Linked<const ZStdCDictionary> dict;
    MemoryBuffer compressed;
public:
    ZStdBlockCompressor(const ZStdCDictionary *_dict, unsigned maxCompressedSize);
    ~ZStdBlockCompressor();
    size_t queryMaxBlockSize() const;
    void setSize(size32_t len) { compressed.setLength(len); };
    size32_t getCompressedSize() const { return compressed.length(); };
    const void *queryCompressedData() const { return compressed.toByteArray(); };
    size_t compress(const void* src, size_t srcSize);
};

//---------------------------------------------------------------------------------------------------------------------

class jhtree_decl CJHInplaceTreeNode : public CJHSearchNode
{
public:
    ~CJHInplaceTreeNode();

    virtual void load(CKeyHdr *keyHdr, const void *rawData, offset_t pos, bool needCopy) override;
    virtual int compareValueAt(const char *src, unsigned int index) const override;
    virtual unsigned __int64 getSequence(unsigned int num) const override;
    virtual int locateGE(const char * search, unsigned minIndex) const override;
    virtual int locateGT(const char * search, unsigned minIndex) const override;
    virtual bool getKeyAt(unsigned int num, char *dest) const override;

protected:
    InplaceNodeSearcher searcher;
    const byte * positionData = nullptr;
    UnsignedArray payloadOffsets;
    byte * payload = nullptr;
    unsigned __int64 firstSequence = 0;
    size32_t keyLen = 0;
    size32_t keyCompareLen = 0;
    byte bytesPerPosition = 0;
    bool scaleFposByNodeSize = false;
    bool ownedPayload = false;
};


class jhtree_decl CJHInplaceBranchNode : public CJHInplaceTreeNode
{
public:
    virtual bool fetchPayload(unsigned int num, char *dest) const override;
    virtual size32_t getSizeAt(unsigned int num) const override;
    virtual offset_t getFPosAt(unsigned int num) const override;
};

class jhtree_decl CJHInplaceLeafNode : public CJHInplaceTreeNode
{
public:
    virtual bool fetchPayload(unsigned int num, char *dest) const override;
    virtual size32_t getSizeAt(unsigned int num) const override;
    virtual offset_t getFPosAt(unsigned int num) const override;
};

class jhtree_decl CInplaceWriteNode : public CWriteNode
{
public:
    CInplaceWriteNode(offset_t fpos, CKeyHdr *keyHdr, bool isLeafNode);

    virtual const void *getLastKeyValue() const override { return lastKeyValue.get(); }
    virtual unsigned __int64 getLastSequence() const override { return lastSequence; }

    void saveLastKey(const void *data, size32_t size, unsigned __int64 sequence);

protected:
    MemoryAttr lastKeyValue;
    unsigned __int64 lastSequence = 0;
    size32_t keyCompareLen = 0;
};

class jhtree_decl CInplaceBranchWriteNode : public CInplaceWriteNode
{
public:
    CInplaceBranchWriteNode(offset_t fpos, CKeyHdr *keyHdr, KeyBuildContext & _ctx);

    virtual bool add(offset_t pos, const void *data, size32_t size, unsigned __int64 sequence) override;
    virtual void write(IFileIOStream *, CRC32 *crc) override;

protected:
    unsigned getDataSize();

protected:
    KeyBuildContext & ctx;
    PartialMatchBuilder builder;
    Unsigned64Array positions;
    unsigned nodeSize;
    bool scaleFposByNodeSize = true;
};

class jhtree_decl CInplaceLeafWriteNode : public CInplaceWriteNode
{
public:
    CInplaceLeafWriteNode(offset_t fpos, CKeyHdr *keyHdr, KeyBuildContext & _ctx, size32_t _lastKeyedFIeldOffset);

    virtual bool add(offset_t pos, const void *data, size32_t size, unsigned __int64 sequence) override;
    virtual void write(IFileIOStream *, CRC32 *crc) override;

protected:
    unsigned getDataSize();

protected:
    KeyBuildContext & ctx;
    PartialMatchBuilder builder;
#ifdef USE_ZSTD_COMPRESSION
    Owned<ZStdBlockCompressor> zstdComp;
#else
    KeyCompressor lzwcomp;
#endif
    MemoryBuffer uncompressed;      // Much better if these could be shared by all nodes => refactor
    MemoryAttr compressed;
    UnsignedArray payloadLengths;
    Unsigned64Array positions;
    __uint64 minPosition = 0;
    __uint64 maxPosition = 0;
    unsigned nodeSize;
    size32_t keyLen = 0;
    size32_t lastKeyedFieldOffset = 0;
#ifndef USE_ZSTD_COMPRESSION
    unsigned numRowsInBlock = 0;
#endif
    bool scaleFposByNodeSize = false;
    bool isVariable = false;
    bool rowCompression = false;
};

class InplaceIndexCompressor : public CInterfaceOf<IIndexCompressor>
{
public:
    InplaceIndexCompressor(size32_t keyedSize, IHThorIndexWriteArg * helper);

    virtual const char *queryName() const override { return "inplace"; }
    virtual CWriteNode *createNode(offset_t _fpos, CKeyHdr *_keyHdr, bool isLeafNode) const override
    {
        if (isLeafNode)
            return new CInplaceLeafWriteNode(_fpos, _keyHdr, ctx, lastKeyedFieldOffset);
        else
            return new CInplaceBranchWriteNode(_fpos, _keyHdr, ctx);
    }

protected:
    mutable KeyBuildContext ctx;
    size32_t lastKeyedFieldOffset = 0;
};

#endif
