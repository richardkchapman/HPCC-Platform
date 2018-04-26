/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

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

#ifndef _JHTREEI_INCL
#define _JHTREEI_INCL

#include "jmutex.hpp"
#include "jhutil.hpp"
#include "jqueue.tpp"
#include "ctfile.hpp"

#include "jhtree.hpp"
#include "bloom.hpp"

typedef OwningStringHTMapping<IKeyIndex> CKeyIndexMapping;
typedef OwningStringSuperHashTableOf<CKeyIndexMapping> CKeyIndexTable;
typedef CMRUCacheMaxCountOf<const char *, IKeyIndex, CKeyIndexMapping, CKeyIndexTable> CKeyIndexMRUCache;

typedef class MapStringToMyClass<IKeyIndex> KeyCache;

class CKeyStore
{
private:
    Mutex mutex;
    Mutex idmutex;
    CKeyIndexMRUCache keyIndexCache;
    int nextId;
    int getUniqId() { synchronized procedure(idmutex); return ++nextId; }
    IKeyIndex *doload(const char *fileName, unsigned crc, IReplicatedFile *part, IFileIO *iFileIO, IMemoryMappedFile *iMappedFile, bool isTLK, bool allowPreload);
public:
    CKeyStore();
    ~CKeyStore();
    IKeyIndex *load(const char *fileName, unsigned crc, bool isTLK, bool allowPreload);
    IKeyIndex *load(const char *fileName, unsigned crc, IFileIO *iFileIO, bool isTLK, bool allowPreload);
    IKeyIndex *load(const char *fileName, unsigned crc, IMemoryMappedFile *iMappedFile, bool isTLK, bool allowPreload);
    IKeyIndex *load(const char *fileName, unsigned crc, IReplicatedFile &part, bool isTLK, bool allowPreload);
    void clearCache(bool killAll);
    void clearCacheEntry(const char *name);
    void clearCacheEntry(const IFileIO *io);
    unsigned setKeyCacheLimit(unsigned limit);
    StringBuffer &getMetrics(StringBuffer &xml);
    void resetMetrics();
};

class CNodeCache;
enum request { LTE, GTE };

// INodeLoader impl.
interface INodeLoader
{
    virtual CJHTreeNode *loadNode(offset_t offset) = 0;
};

class jhtree_decl CKeyIndex : implements IKeyIndex, implements INodeLoader, public CInterface
{
    friend class CKeyStore;
    friend class CKeyCursor;

private:
    CKeyIndex(CKeyIndex &);

protected:
    int iD;
    StringAttr name;
    CriticalSection blobCacheCrit;
    Owned<CJHTreeBlobNode> cachedBlobNode;
    CIArrayOf<IndexBloomFilter> bloomFilters;
    offset_t cachedBlobNodePos;

    CKeyHdr *keyHdr;
    CNodeCache *cache;
    CJHTreeNode *rootNode;
    RelaxedAtomic<unsigned> keySeeks;
    RelaxedAtomic<unsigned> keyScans;
    offset_t latestGetNodeOffset;

    CJHTreeNode *loadNode(char *nodeData, offset_t pos, bool needsCopy);
    CJHTreeNode *getNode(offset_t offset, IContextLogger *ctx);
    CJHTreeBlobNode *getBlobNode(offset_t nodepos);


    CKeyIndex(int _iD, const char *_name);
    ~CKeyIndex();
    void init(KeyHdr &hdr, bool isTLK, bool allowPreload);
    void cacheNodes(CNodeCache *cache, offset_t nodePos, bool isTLK);
    void loadBloomFilters();
    
public:
    IMPLEMENT_IINTERFACE;
    virtual bool IsShared() const { return CInterface::IsShared(); }

// IKeyIndex impl.
    virtual IKeyCursor *getCursor(const SegMonitorList *segs, IContextLogger *ctx) override;

    virtual size32_t keySize();
    virtual bool hasPayload();
    virtual size32_t keyedSize();
    virtual bool isTopLevelKey() override;
    virtual bool isFullySorted() override;
    virtual __uint64 getPartitionFieldMask() override;
    virtual unsigned numPartitions() override;
    virtual unsigned getFlags() { return (unsigned char)keyHdr->getKeyType(); };

    virtual void dumpNode(FILE *out, offset_t pos, unsigned count, bool isRaw);

    virtual unsigned numParts() { return 1; }
    virtual IKeyIndex *queryPart(unsigned idx) { return idx ? NULL : this; }
    virtual unsigned queryScans() { return keyScans; }
    virtual unsigned querySeeks() { return keySeeks; }
    virtual const byte *loadBlob(unsigned __int64 blobid, size32_t &blobsize);
    virtual offset_t queryBlobHead() { return keyHdr->getHdrStruct()->blobHead; }
    virtual void resetCounts() { keyScans.store(0); keySeeks.store(0); }
    virtual offset_t queryLatestGetNodeOffset() const { return latestGetNodeOffset; }
    virtual offset_t queryMetadataHead();
    virtual IPropertyTree * getMetadata();

    bool bloomFilterReject(const SegMonitorList &segs) const;

    virtual unsigned getNodeSize() { return keyHdr->getNodeSize(); }
    virtual bool hasSpecialFileposition() const;
 
 // INodeLoader impl.
    virtual CJHTreeNode *loadNode(offset_t offset) = 0;
};

class jhtree_decl CMemKeyIndex : public CKeyIndex
{
private:
    Linked<IMemoryMappedFile> io;
public:
    CMemKeyIndex(int _iD, IMemoryMappedFile *_io, const char *_name, bool _isTLK);

    virtual const char *queryFileName() { return name.get(); }
    virtual const IFileIO *queryFileIO() const override { return nullptr; }
// INodeLoader impl.
    virtual CJHTreeNode *loadNode(offset_t offset);
};

class jhtree_decl CDiskKeyIndex : public CKeyIndex
{
private:
    Linked<IFileIO> io;
    void cacheNodes(CNodeCache *cache, offset_t firstnode, bool isTLK);
    
public:
    CDiskKeyIndex(int _iD, IFileIO *_io, const char *_name, bool _isTLK, bool _allowPreload);

    virtual const char *queryFileName() { return name.get(); }
    virtual const IFileIO *queryFileIO() const override { return io; }
// INodeLoader impl.
    virtual CJHTreeNode *loadNode(offset_t offset);
};

class jhtree_decl CKeyCursor : public IKeyCursor, public CInterface
{
private:
    IContextLogger *ctx;
    CKeyIndex &key;
    const SegMonitorList *segs;
    Owned<CJHTreeNode> node;
    unsigned int nodeKey;
    ConstPointerArray activeBlobs;
    unsigned seeks = 0;
    unsigned scans = 0;
    unsigned wildseeks = 0;
    unsigned skips = 0;
    unsigned nullSkips = 0;

    bool eof=false;
    bool matched=false;
    bool ownSegs = false;

    unsigned keySize = 0;
    const unsigned keyedSize = 0;
    char *keyBuffer = nullptr;

    CJHTreeNode *locateFirstNode();
    CJHTreeNode *locateLastNode();

public:
    IMPLEMENT_IINTERFACE;
    CKeyCursor(CKeyIndex &_key, const SegMonitorList *segs, IContextLogger *ctx);
    ~CKeyCursor();

    virtual bool next(char *dst);
    virtual bool first(char *dst);
    virtual bool last(char *dst);
    virtual bool gtEqual(const char *src, char *dst);
    virtual bool ltEqual(const char *src);
    virtual const char *queryName() const override;
    virtual size32_t getSize();
    virtual size32_t getKeyedSize() const;
    virtual offset_t getFPos(); 
    virtual void serializeCursorPos(MemoryBuffer &mb);
    virtual void deserializeCursorPos(MemoryBuffer &mb);
    virtual unsigned __int64 getSequence(); 
    virtual const byte *loadBlob(unsigned __int64 blobid, size32_t &blobsize);
    virtual void releaseBlobs();
    virtual void reset(unsigned sortFromSeg = 0);
    virtual bool lookup(bool exact, unsigned lastSeg) override;
    virtual bool lookupSkip(const void *seek, size32_t seekOffset, size32_t seeklen) override;
    virtual bool skipTo(const void *_seek, size32_t seekOffset, size32_t seeklen) override;
    virtual IKeyCursor *fixSortSegs(unsigned sortFieldOffset) override;

    virtual unsigned __int64 getCount() override;
    virtual unsigned __int64 checkCount(unsigned __int64 max) override;
    virtual unsigned __int64 getCurrentRangeCount(unsigned groupSegCount) override;
    virtual bool nextRange(unsigned groupSegCount) override;
    virtual const byte *queryKeyBuffer() override;
protected:
    CKeyCursor(const CKeyCursor &from, const SegMonitorList *segs);

    void reportExcessiveSeeks(unsigned numSeeks, unsigned lastSeg);
    void noteSeeks(unsigned lseeks, unsigned lscans, unsigned lwildseeks);
    void noteSkips(unsigned lskips, unsigned lnullSkips);


    inline void setLow(unsigned segNo)
    {
        segs->setLow(segNo, keyBuffer);
    }
    inline unsigned setLowAfter(size32_t offset)
    {
        return segs->setLowAfter(offset, keyBuffer);
    }
    inline bool incrementKey(unsigned segno) const
    {
        return segs->incrementKey(segno, keyBuffer);
    }
    inline void endRange(unsigned segno)
    {
        segs->endRange(segno, keyBuffer);
    }
};


#endif
