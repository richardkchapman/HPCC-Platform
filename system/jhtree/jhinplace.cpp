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

#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <alloca.h>
#endif
#include <algorithm>

#include "jmisc.hpp"
#include "jset.hpp"
#include "hlzw.h"

#include "ctfile.hpp"
#include "jhinplace.hpp"
#include "jstats.h"

/* Notes on zstd API
Block level API

    Frame metadata cost is typically ~12 bytes, which can be non-negligible for very small blocks (< 100 bytes).
    But users will have to take in charge needed metadata to regenerate data, such as compressed and content sizes.

    A few rules to respect :
    - Compressing and decompressing require a context structure
      + Use ZSTD_createCCtx() and ZSTD_createDCtx()
    - It is necessary to init context before starting
      + compression : any ZSTD_compressBegin*() variant, including with dictionary
      *   Probably ZSTDLIB_STATIC_API size_t ZSTD_compressBegin_usingCDict(ZSTD_CCtx* cctx, const ZSTD_CDict* cdict); 
      + decompression : any ZSTD_decompressBegin*() variant, including with dictionary
      + copyCCtx() and copyDCtx() can be used too
    - Block size is limited, it must be <= ZSTD_getBlockSize() <= ZSTD_BLOCKSIZE_MAX == 128 KB
      + If input is larger than a block size, it's necessary to split input data into multiple blocks
      + For inputs larger than a single block, consider using regular ZSTD_compress() instead.
        Frame metadata is not that costly, and quickly becomes negligible as source size grows larger than a block.
    - When a block is considered not compressible enough, ZSTD_compressBlock() result will be 0 (zero) !
      ===> In which case, nothing is produced into `dst` !
      + User __must__ test for such outcome and deal directly with uncompressed data
      + A block cannot be declared incompressible if ZSTD_compressBlock() return value was != 0.
        Doing so would mess up with statistics history, leading to potential data corruption.
      + ZSTD_decompressBlock() _doesn't accept uncompressed data as input_ !!
      + In case of multiple successive blocks, should some of them be uncompressed,
        decoder must be informed of their existence in order to follow proper history.
        Use ZSTD_insertBlock() for such a case.


Raw zstd block functions

ZSTDLIB_STATIC_API size_t ZSTD_getBlockSize   (const ZSTD_CCtx* cctx);
ZSTDLIB_STATIC_API size_t ZSTD_compressBlock  (ZSTD_CCtx* cctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);
ZSTDLIB_STATIC_API size_t ZSTD_decompressBlock(ZSTD_DCtx* dctx, void* dst, size_t dstCapacity, const void* src, size_t srcSize);

*/

ZStdCDictionary::ZStdCDictionary(const void* dictBuffer, size_t dictSize, int compressionLevel)
{
    cdict = ZSTD_createCDict(dictBuffer, dictSize, compressionLevel);
}

ZStdCDictionary::~ZStdCDictionary()
{
    ZSTD_freeCDict(cdict);
}

ZStdBlockCompressor::ZStdBlockCompressor(const ZStdCDictionary *_dict, unsigned maxCompressedSize) : dict(_dict)
{
    cctx = ZSTD_createCCtx();
    ZSTD_compressBegin_usingCDict(cctx, *dict);
    compressed.ensureCapacity(maxCompressedSize);
}

ZStdBlockCompressor::~ZStdBlockCompressor()
{
    ZSTD_freeCCtx(cctx);
}

size_t ZStdBlockCompressor::queryMaxBlockSize() const
{
    return ZSTD_getBlockSize(cctx);
}

size_t ZStdBlockCompressor::compress(const void* src, size_t srcSize) // MORE - probably want to indicate current capacity, which is reduced by what has currently been written to node OTHER than compressed rows
{
    // MORE - handle case where it refuses (not compressible) or not enough room
    // Non-compressible will need marking as such somehow... Tricky to mix compressed and non-compressed rows
    size_t ret = ZSTD_compressBlock(cctx, compressed.ensureCapacity(0), compressed.capacity(), src, srcSize);
    assertex(ret);
    compressed.reserve(ret);
    return ret;
}

#ifdef _DEBUG
//#define SANITY_CHECK_INPLACE_BUILDER     // painfully expensive consistency check
//#define TRACE_BUILDING
#endif

static constexpr size32_t minRepeatCount = 2;       // minimum number of times a 0x00 or 0x20 is repeated to generate a special opcode
static constexpr size32_t minRepeatXCount = 3;      // minimum number of times a byte is repeated to generate a special opcode
static constexpr size32_t maxQuotedCount = 31 + 256; // maximum number of characters that can be quoted in a row
static constexpr size32_t repeatDelta = (minRepeatCount - 1);
static constexpr size32_t repeatXDelta = (minRepeatXCount - 1);

constexpr size32_t getMinRepeatCount(byte value)
{
    return (value == 0x00 || value == 0x20) ? minRepeatCount : minRepeatXCount;
}

enum SquashOp : byte
{
    SqZero      = 0x00,           // repeated zeros
    SqSpace     = 0x01,           // repeated spaces
    SqQuote     = 0x02,           // Quoted text
    SqRepeat    = 0x03,           // any character repeated
    SqOption    = 0x04,           // an set of option bytes
    SqSingle    = 0x05,           // a single character - count indicates which of the common values it is.
    SqSingleX   = 0x06,           // A SqSingle followed by a single byte

    //What other options are worth doing?
    //SqXSingle   = 0x07,         // A single byte followed by a SqSingle?  Maybe 2.5% on my sample index.
    //SqMatchX                    // Matches a string at offset (cur-1-count) - delta8/16.  Only likely to help with repeated trailing key segments
};

//inline unsigned reverseBytes(unsigned value) { return __builtin_bswap32(value); }

//MORE: What other special values are worth encoding in real life indexes?
static constexpr byte specialSingleChars[31] = {
    0, 1, 2, 3, 4, 5, 6, 7,                 // 1-8      Useful for small numbers
    ' ', '0', 'F', 'M', 'U',                // 9-13     Single padding and gender
                                            // 14-31    Currently unused.
};

/*

How can you store non-leaf nodes that are compressed, don't need decompression to compare, and are efficient to resolve?

The following compression aims to implement that.  It aims to be cache efficient, rather than minimizing the number
of byte comparisons - in particular it scans blocks of bytes rather than binary chopping them.

The compressed format is an alternating tree of string matches and byte options.

Take the following list of names:
(abel, abigail, absalom, barry, barty, bill, brian, briany) (5, 8, 8, 6, 6, 5, 5, 6) = 49bytes

This is the suggested internal representation

match(0)
    opt(2, { a , b }, {3, 8})
        match(1, "b")
    opt(3, { e, i, s }, { 1, 2, 3})
        match(1, "l")                   #end
        match(4, "gail")                #end
        match(4, "alom")                #end
    opt(3, { a, i, r }, { 2, 3, 5 })
        match(1, "r")
        opt(2, { r, t}, { 1, 2 })
            match(1, "y")               #end
            match(1, "y")               #end
        match(3, "ill")                 #end
        match(3, "ian")
        opt(2, { ' ', 'y' }, { 1, 2})
            match(0)                    #end
            match(0)                    #end

The candidate in-memory layout would be:

     0x00
      0x02 0x11 A B count(0x03 0x08) offset{22 [57])
       0x01 B
        0x03 0x01 E I S offset(2 7 [12])
         0x01 L
         0x04 G A I L
         0x04 A L O M
       0x03 0x11 A I R count(0x02 0x03 0x05) offset(12 16 [24])
        0x01 R
         0x02 0x01 R T offset(0x02 [0x04])
          0x01 Y
          0x01 Y
        0x03 I L L
        0x03 I A N
         0x02 0x01 ' ' Y offset(0x01 [0x02])
          0x00
          0x00

Compression optimizations:
    All lengths use packed integers
    All arrays of counts or offsets use a number of bytes (nibbles?) required for the largest number they are storing.
    #End is implied by a range of 1 count rather than stored as a flag
    Store count-2 for options since there must be at least 2 alternatives
    Store count-1 for the counts of the option branches
    Don't store the offset after the last option branch since it is never needed.
    Use a scaled array for the filepositions, and use a bit to indicate scale by the node size.
    Use a special size code (0) for duplicates - option with nothing more to match on any branches.

Done:
  * Add code to trace the node cache sizes and the time taken to search branch nodes.
    Use this to compare i) normal ii) new format iii) new format with direct search
  * Ensure that the allocated node result is padded with 3 zero bytes to allow misaligned access.
  * Experiminent with updating offset only - code size and speed - seemed slower on first version
  * Move serialize code in builder inside a sanity check flag.

Next Steps:
  * Implement findGT (inline in terms of a common function). - delay until new format introduced
    Add to unit tests as well as node code.
=> Have a complete implementation that is useful for speed testing

  * Use leading bit count operations to implement packing/unpacking operations
  * Allow repeated values and matches to the null record to be compressed
      Use size bytes 0xF0-0xFF to represent special sequences.
      Bottom bit indicates if there is a following match (rather than opt)
      0xF0/F1 <count> <byte> - byte is repeated <count> times
      0xF2/F3 <count> - matches items from the null row
      0xF4 <count> - A compound repeat follows, don't assume end has been reached
  * Create null rows in index code and pass through
  * Use unsigned read to check for repeated spaces?
  * Check padding of file positions to 8 bytes
=> Complete implementation for leaf nodes.

  * Ensure that match sequences never span the key boundary
  * Make sure the correct key length is passed through
  * Add code in the comparison to spot a match once the keylength has been reached, even if more data follows.
  * Allow options where some are 0 length - not needed for comparison, only getValueAt().
    (Set a bit on the option, include the last offset, and check the offset delta != 0).  assert not set if in keyed portion
    or have a bitset of null lengths
  * Document all the assumptions/ideas in the design.  Fixed length compare key, variable length trailing.
=> Experiment with using the same compression for leaf nodes.
  o Using rows as they are
  o Using lz4 compression with a separate compressed index of offsets into the compressed data

Future potential optimizations
    compare generated code updating offset rather than search in compare code -> is it more efficient?
    Compare in 4byte chunks and use __builtin_bswap32 to reverse bytes when they differ
    Don't save the last count in an options list (it must = next-prev)
    Move the countBytes=0 processing into the caller function, same file filepos packing, and check not zero in extract function
    Allow nibbles for sizes of arrays
    Store offset-1 for the offsets of the option branches
    Use bits in the sizeinfo to indicate special cases...
    Could have varing sizes for lead and non-leaf nodes, but I suspect it wouldn't save much.
    Add stats on number of nodes, average null compare length, zero compare length option fan out etc.  Number of bytes
    saved by trailing null suppression.
    Optimized/simplified numExtraBytesFromFirst() that can only support 4 bytes.
    Is big or little endian better for the packed integers?
    ?Use LZ END to compress payload in leaf nodes?

Different ways of storing the payload:
  * Small fixed size, store as is, offset calculated
  * small compressed/variablesize.  store size as a byte, but accumulate to find the offsets.
  * large compressed.  store offset as a word

NOTES:
    The format is assumed to have a fixed length key, potentially followed by a variable length payload that is never compared.
    Always guarantee 3 bytes after last item in an array to allow misaligned read access
    Once you finish building subtree the representation will not change.  Can cache the serialized size and only recalculate if the tree changes.
    Currently planned for non-leaf nodes only, but it could also be used for leaf nodes, with an offset table to take you to a separate payload.

*/


//For the following the efficiency of serialization isn't a big concern, speed of deserialization is.

constexpr unsigned maxRepeatCount = 32 + 255;
void serializeOp(MemoryBuffer & out, SquashOp op, size32_t count)
{
    assertex(count != 0 && count <= maxRepeatCount);
    if (count <= 31)
    {
        out.append((byte)((op << 5) | count));
    }
    else
    {
        out.append((byte)(op << 5));
        out.append((byte)(count - 32));
    }
}

unsigned sizeSerializedOp(size32_t count)
{
    return (count <= 31) ? 1 : 2;
}


unsigned getSpecialSingleCharIndex(byte search)
{
    for (unsigned i=0; i < sizeof(specialSingleChars); i++)
    {
        if (specialSingleChars[i] == search)
            return i+1;
    }
    return 0;
}

//Packing using a top bit to indicate more.  Revisit if it isn't great.  Could alternatively use rtl packed format, but want to avoid the overhead of a call
static unsigned sizePacked(unsigned __int64 value)
{
    unsigned size = 1;
    while (value >= 0x80)
    {
        value >>= 7;
        size++;
    }
    return size;

    //The following is slower because of the division by 7, also likely to be slower for small values
    //    unsigned bits = getMostSignificantBit(value|1);
    //    return (bits + 6) / 7;
}


static void serializePacked(MemoryBuffer & out, unsigned __int64  value)
{
    constexpr unsigned maxBytes = 9;
    unsigned index = maxBytes;
    byte result[maxBytes];

    result[--index] = value & 0x7f;
    while (value >= 0x80)
    {
        value >>= 7;
        result[--index] = (value & 0x7f) | 0x80;
    }
    out.append(maxBytes - index, result + index);
}

inline unsigned readPacked32(const byte * & cur)
{
    byte next = *cur++;
    unsigned value = next;
    if (unlikely(next >= 0x80))
    {
        value = value & 0x7f;
        do
        {
            next = *cur++;
            value = (value << 7) | (next & 0x7f);
        } while (next & 0x80);
    }
    return value;
}

inline unsigned __int64 readPacked64(const byte * & cur)
{
    byte next = *cur++;
    unsigned value = next;
    if (unlikely(next >= 0x80))
    {
        value = value & 0x7f;
        do
        {
            next = *cur++;
            value = (value << 7) | (next & 0x7f);
        } while (next & 0x80);
    }
    return value;
}
//----- special packed format - code is untested

inline unsigned getLeadingMask(byte extraBytes) { return (0x100U - (1U << (8-extraBytes))); }
inline unsigned getLeadingValueMask(byte extraBytes) { return (~getLeadingMask(extraBytes)) >> 1; }

static void serializePacked2(MemoryBuffer & out, size32_t value)
{
    //Efficiency of serialization is not the key consideration
    byte mask = 0;
    unsigned size = sizePacked(value);
    constexpr unsigned maxBytes = 9;
    byte result[maxBytes];

    for (unsigned i=1; i < size; i++)
    {
        result[maxBytes - i] = value;
        value >>= 8;
        mask = 0x80 | (mask >> 1);
    }
    unsigned start = maxBytes - size;
    result[start] |= mask;
    out.append(size, result + start);
}

inline unsigned numExtraBytesFromFirst1(byte first)
{
    if (first >= 0xF0)
        if (first >= 0xFC)
            if (first >= 0xFE)
                if (first >= 0xFF)
                    return 8;
                else
                    return 7;
            else
                return 6;
        else
            if (first >= 0xF8)
                return 5;
            else
                return 4;
    else
        if (first >= 0xC0)
            if (first >= 0xE0)
                return 3;
            else
                return 2;
        else
            return (first >> 7);    // (first >= 0x80) ? 1 : 0
}

inline unsigned numExtraBytesFromFirst2(byte first)
{
    //Surely should be faster, but seems slower on AMD. Retest in its actual context
    unsigned value = first;
    return countLeadingUnsetBits(~(value << 24));
}

inline unsigned numExtraBytesFromFirst(byte first)
{
    return numExtraBytesFromFirst1(first);
}

inline unsigned readPacked2(const byte * & cur)
{
    byte first = *cur++;
    unsigned extraBytes = numExtraBytesFromFirst(first);
    unsigned value = first & getLeadingValueMask(extraBytes);
    while (extraBytes--)
    {
        value <<= 8;
        value |= *cur++;
    }
    return value;
}

//-------------------

static unsigned bytesRequired(unsigned __int64 value)
{
    unsigned bytes = 1;
    while (value >= 0x100)
    {
        value >>= 8;
        bytes++;
    }
    return bytes;
}

void serializeBytes(MemoryBuffer & out, unsigned __int64 value, unsigned bytes)
{
    for (unsigned i=0; i < bytes; i++)
    {
        out.append((byte)value);
        value >>= 8;
    }
    assertex(value == 0);
}

unsigned readBytesEntry32(const byte * address, unsigned index, unsigned bytes)
{
    dbgassertex(bytes != 0);

    //Non aligned access, assumes little endian, but avoids a loop
    const byte * addrValue = address + index * bytes;
    unsigned value = *(const unsigned *)addrValue;
    unsigned mask = 0xFFFFFFFF >> ((4 - bytes) * 8);
    return (value & mask);
}

//MORE: Inline would probably speed this up
unsigned readBytesEntry16(const byte * address, unsigned index, unsigned bytes)
{
    dbgassertex(bytes != 0);

    //Use a shift (works for bytes = 1 or 2) to avoid a multiply
    const byte * addrValue = address + (index << (bytes - 1));
    unsigned short value = *(const unsigned short *)addrValue;
    if (bytes == 1)
        value = (byte)value;
    return value;
}

unsigned __int64 readBytesEntry64(const byte * address, unsigned index, unsigned bytes)
{
    dbgassertex(bytes != 0);

    //Non aligned access, assumes little endian, but avoids a loop
    const unsigned __int64 * pValue = (const unsigned __int64 *)(address + index * bytes);
    unsigned __int64 value = *pValue;
    unsigned __int64 mask = U64C(0xFFFFFFFFFFFF) >> ((8 - bytes) * 8);
    return (value & mask);
}

//=========================================================================================================

class PartialMatchBuilder;
//---------------------------------------------------------------------------------------------------------------------

bool PartialMatch::allNextAreEnd()
{
    ForEachItemIn(i, next)
    {
        if (!next.item(i).isEnd())
            return false;
    }
    return true;
}

void PartialMatch::cacheSizes()
{
    if (!dirty)
        return;

    squash();
    dirty = false;
    if (squashed && squashedData.length())
        size = squashedData.length();
    else
        size = 0;

    maxOffset = 0;
    unsigned numNext = next.ordinality();
    if (numNext)
    {
        if (allNextAreEnd())
        {
            size += sizeSerializedOp(numNext-1);  // count of options
            size += 1;                      // end marker
            maxCount = numNext;
        }
        else
        {
            size32_t offset = 0;
            maxCount = 0;
            for (unsigned i=0; i < numNext; i++)
            {
                maxCount += next.item(i).getCount();
                maxOffset = offset;
                offset += next.item(i).getSize();
            }

            size += sizeSerializedOp(numNext-1);  // count of options
            size += 1;                      // count and offset table information
            size += numNext;                // bytes of data

            //Space for the count table - if it is required
            if (maxCount != numNext)
                size += (numNext * bytesRequired(maxCount-1));

            //offset table.
            size += (numNext - 1) * bytesRequired(maxOffset);

            size += offset;                       // embedded tree
        }
    }
    else
        maxCount = 1;
}

bool PartialMatch::combine(size32_t newLen, const byte * newData, size32_t &common)
{
    size32_t curLen = data.length();
    const byte * curData = (const byte *)data.toByteArray();
    unsigned compareLen = std::min(newLen, curLen);
    unsigned matchLen;
    for (matchLen = 0; matchLen < compareLen; matchLen++)
    {
        if (curData[matchLen] != newData[matchLen])
            break;
    }

    if (matchLen || isRoot)
    {
        common = matchLen;
        dirty = true;
        if (next.ordinality() == 0)
        {
            next.append(*new PartialMatch(builder, curLen - matchLen, curData + matchLen, rowOffset + matchLen, false));
            next.append(*new PartialMatch(builder, newLen - matchLen, newData + matchLen, rowOffset + matchLen, false));
            data.setLength(matchLen);
            squashed = false;
            return true;
        }

        if (matchLen == curLen) // Is this only when trimming trailing? Implies curLen was not same as compareLen I think?
        {
            //Either add a new option, or combine with the last option
            size32_t dummy;
            if (next.tos().combine(newLen - matchLen, newData + matchLen, dummy))
                return true;
            next.append(*new PartialMatch(builder, newLen - matchLen, newData + matchLen, rowOffset + matchLen, false));
            return true;
        }

        //Split this node into two
        Owned<PartialMatch> childNode = new PartialMatch(builder, curLen-matchLen, curData + matchLen, rowOffset + matchLen, false);
        next.swapWith(childNode->next);
        next.append(*childNode.getClear());
        next.append(*new PartialMatch(builder, newLen - matchLen, newData + matchLen, rowOffset + matchLen, false));
        data.setLength(matchLen);
        squashed = false;
        return true;
    }
    common = 0;
    return false;
}

size32_t PartialMatch::getSize()
{
    cacheSizes();
    return size;
}

size32_t PartialMatch::getCount()
{
    cacheSizes();
    return maxCount;
}

size32_t PartialMatch::getMaxOffset()
{
    cacheSizes();
    return maxOffset;
}

bool PartialMatch::removeLast()
{
    dirty = true;
    if (next.ordinality() == 0)
        return true; // merge with caller
    if (next.tos().removeLast())
    {
        if (next.ordinality() == 2)
        {
            //Remove the second entry, and merge this element with the first
            Linked<PartialMatch> first = &next.item(0);
            next.kill();
            data.append(first->data);
            next.swapWith(first->next);
            squashed = false;
            return false;
        }
        else
        {
            next.pop();
            return false;
        }
    }
    return false;
}

void PartialMatch::serialize(MemoryBuffer & out)
{
    squash();

    size32_t originalPos = out.length();
    if (squashed && squashedData.length())
        out.append(squashedData);
    else
    {
        unsigned skip = isRoot ? 0 : 1;
        assertex (data.length() <= skip);
    }

    unsigned numNext = next.ordinality();
    if (numNext)
    {
        if (allNextAreEnd())
        {
            builder->numDuplicates++;
            serializeOp(out, SqOption, numNext-1);  // count of options
            out.append((byte)0);
        }
        else
        {
            byte offsetBytes = bytesRequired(getMaxOffset());
            byte countBytes = bytesRequired(getCount()-1);

            byte sizeInfo = offsetBytes;
            if (getCount() != numNext)
                sizeInfo |= (countBytes << 3);

            serializeOp(out, SqOption, numNext-1);  // count of options
            out.append(sizeInfo);

            for (unsigned iFirst = 0; iFirst < numNext; iFirst++)
            {
                next.item(iFirst).serializeFirst(out);
            }

            //Space for the count table - if it is required
            if (getCount() != numNext)
            {
                unsigned runningCount = 0;
                for (unsigned iCount = 0; iCount < numNext; iCount++)
                {
                    runningCount += next.item(iCount).getCount();
                    serializeBytes(out, runningCount-1, countBytes);
                }
                assertex(getCount() == runningCount);
            }

            unsigned offset = 0;
            for (unsigned iOff=1; iOff < numNext; iOff++)
            {
                offset += next.item(iOff-1).getSize();
                serializeBytes(out, offset, offsetBytes);
            }

            for (unsigned iNext=0; iNext < numNext; iNext++)
                next.item(iNext).serialize(out);
        }
    }

    size32_t newPos = out.length();
    assertex(newPos - originalPos == getSize());
}

unsigned PartialMatch::appendRepeat(size32_t offset, size32_t copyOffset, byte repeatByte, size32_t repeatCount)
{
    unsigned numOps = 0;
    const byte * source = data.bytes();
    size32_t copySize = (offset - copyOffset) - repeatCount;
    if (copySize)
    {
//        if (copySize <= 3)
        {
            bool useSpecial = true;
            bool prevSpecial = false;
            for (unsigned i=0; i < copySize; i++)
            {
                if (getSpecialSingleCharIndex(source[copyOffset+i]) == 0)
                {
                    if (!prevSpecial)
                    {
                        useSpecial = false;
                        break;
                    }
                    prevSpecial = false;
                }
                else
                    prevSpecial = true;
            }

            if (useSpecial)
            {
                for (unsigned i=0; i < copySize; i++)
                {
                    unsigned special = getSpecialSingleCharIndex(source[copyOffset+i]);
                    if (i + 1 < copySize)
                    {
                        byte nextByte = source[copyOffset+i+1];
                        unsigned nextSpecial = getSpecialSingleCharIndex(nextByte);
                        if (nextSpecial == 0)
                        {
                            serializeOp(squashedData, SqSingleX, special);
                            squashedData.append(nextByte);
                            i++;
                        }
                        else
                            serializeOp(squashedData, SqSingle, special);
                    }
                    else
                        serializeOp(squashedData, SqSingle, special);
                }
                copySize = 0;
            }
        }

        while (copySize)
        {
            size32_t chunkSize = std::min(copySize, maxQuotedCount);
            serializeOp(squashedData, SqQuote, chunkSize);
            squashedData.append(chunkSize, source+copyOffset);
            copyOffset += chunkSize;
            copySize -= chunkSize;
            numOps++;
        }
    }
    if (repeatCount)
    {
        switch (repeatByte)
        {
        case 0x00:
            serializeOp(squashedData, SqZero, repeatCount - repeatDelta);
            break;
        case 0x20:
            serializeOp(squashedData, SqSpace, repeatCount - repeatDelta);
            break;
        default:
            serializeOp(squashedData, SqRepeat, repeatCount - repeatXDelta);
            squashedData.append(repeatByte);
            break;
        }
        numOps++;
    }
    return numOps;
}

bool PartialMatch::squash()
{
    if (!squashed)
    {
        squashed = true;
        squashedData.clear();

        //Always squash if you have some text - avoids length calculation elsewhere
        size32_t startOffset = isRoot ? 0 : 1;
        size32_t keyLen = builder->queryKeyLen();
        if (data.length() > startOffset)
        {
            const byte * source = data.bytes();
            size32_t maxOffset = data.length();

            unsigned copyOffset = startOffset;
            unsigned repeatCount = 0;
            byte prevByte = 0;
            const byte * nullRow = queryNullRow();
            for (unsigned offset = startOffset; offset < maxOffset; offset++)
            {
                byte nextByte = source[offset];

                //MORE Add support for compressing against the null row at somepoint
                //MORE: Ensure that quoted strings are do not span the key boundary?
                if (nextByte == prevByte)
                {
                    repeatCount++;
                    if (repeatCount >= maxRepeatCount + repeatDelta)
                    {
                        appendRepeat(offset+1, copyOffset, prevByte, repeatCount);
                        copyOffset = offset+1;
                        repeatCount = 0;
                    }
                }
                else
                {
                    //MORE: Revisit and make this more sophisticated.  Trade off between space and comparison time.
                    //  if space or /0 decrease the threshold by 1.
                    //  if the start of the string then reduce the threshold.
                    //  If no child entries increase the threshold (since it may require a special continuation byte)?
                    //  After the keyed component compress more aggressively
                    if (repeatCount > 3)
                    {
                        appendRepeat(offset, copyOffset, prevByte, repeatCount);
                        copyOffset = offset;
                    }
                    repeatCount = 1;
                    prevByte = nextByte;
                }
            }

            if (repeatCount < getMinRepeatCount(prevByte))
                repeatCount = 0;

            appendRepeat(maxOffset, copyOffset, prevByte, repeatCount);
        }
    }
    return dirty;
}

const byte * PartialMatch::queryNullRow()
{
    return builder->queryNullRow();
}

//MORE: Pass this in...
void PartialMatch::serializeFirst(MemoryBuffer & out)
{
    if (data.length())
        out.append(data.bytes()[0]);
    else if (rowOffset < builder->queryKeyLen())
    {
        assertex(queryNullRow());
        out.append(queryNullRow()[rowOffset]);
    }
    else
        out.append((byte)0);    // will probably be harmless - potentially appending an extra 0 byte to shorter payloads.
}

void PartialMatch::describeSquashed(StringBuffer & out)
{
    const byte * cur = squashedData.bytes();
    const byte * end = cur + squashedData.length();
    while (cur < end)
    {
        byte nextByte = *cur++;
        SquashOp op = (SquashOp)(nextByte >> 5);
        unsigned count = (nextByte & 0x1f);
        if (count == 0)
            count = 32 + *cur++;

        switch (op)
        {
        case SqZero:
            out.appendf("ze(%u) ", count);
            break;
        case SqSpace:
            out.appendf("sp(%u) ", count);
            break;
        case SqQuote:
            out.appendf("qu(%u,", count);
            for (unsigned i=0; i < count; i++)
                out.appendf(" %02x", cur[i]);
            out.append(") ");
            cur += count;
            break;
        case SqRepeat:
            out.appendf("re(%u, %02x) ", count, *cur);
            cur++;
            break;
        case SqSingle:
            out.appendf("si(%u) ", count);
            break;
        case SqSingleX:
            out.appendf("sx(%u %02x) ", count, *cur);
            cur++;
            break;
        }
    }
}


void PartialMatch::gatherSingleTextCounts(StringBuffer & prev, unsigned * singleCounts)
{
    const byte * cur = squashedData.bytes();
    const byte * end = cur + squashedData.length();
    unsigned offset = rowOffset;
    while (cur < end)
    {
        byte nextByte = *cur++;
        SquashOp op = (SquashOp)(nextByte >> 5);
        unsigned count = (nextByte & 0x1f);
        if (count == 0)
            count = 32 + *cur++;

        switch (op)
        {
        case SqZero:
        case SqSpace:
            offset += count;
            break;
        case SqQuote:
            if (count == 1)
                singleCounts[*cur]++;
#ifdef TRACE_BUILDING
            //Horrible code to estimate how beneficial it would be to match strings that have previously occurred
            //Should never be released enabled.
            if (count >= 3 && next.ordinality() == 0)
            {
                StringBuffer s;
                for (unsigned i=0; i < count; i++)
                    s.appendf(" %2x", cur[i]);
                if (strstr(prev, s.str()))
                    DBGLOG("%p: @%u %u %s", this, offset, count, s.str());
                prev.append("$").append(s.str());
            }
#endif
            cur += count;
            offset += count;
            break;
        case SqRepeat:
            cur++;
            offset += count;
            break;
        case SqSingle:
            offset++;
            break;
        case SqSingleX:
            cur++;
            offset+=2;
            break;
        }
    }

    ForEachItemIn(i, next)
        next.item(i).gatherSingleTextCounts(prev, singleCounts);
}

void PartialMatch::trace(unsigned indent)
{
    StringBuffer squashedText;
    describeSquashed(squashedText);

    StringBuffer clean;
    StringBuffer dataHex;
    for (unsigned i=0; i < data.length(); i++)
    {
        byte next = data.bytes()[i];
        dataHex.appendhex(next, true);
        if (isprint(next))
            clean.append(next);
        else
            clean.append('.');
    }

    StringBuffer text;
    text.appendf("%*s(%s[%s] %u:%u[%u] [%s]", indent, "", clean.str(), dataHex.str(), data.length(), squashedData.length(), getSize(), squashedText.str());
    if (next.ordinality())
    {
        DBGLOG("%s, %u{\n", text.str(), next.ordinality());
        ForEachItemIn(i, next)
            next.item(i).trace(indent+1);
        DBGLOG("%*s})", indent, "");
    }
    else
        DBGLOG("%s)", text.str());
}

//---------------------------------------------------------------------------------------------------------------------

size32_t PartialMatchBuilder::add(size32_t len, const void * data)
{
    if (optimizeTrailing && (len == keyLen))
    {
        const byte * newRow = (const byte *)data;
        while (len && (newRow[len-1] == nullRow[len-1]))
            len--;
    }
    size32_t common = 0;
    if (!root)
        root.set(new PartialMatch(this, len, data, 0, true));
    else
        root->combine(len, (const byte *)data, common);

#ifdef SANITY_CHECK_INPLACE_BUILDER
    MemoryBuffer out;
    root->serialize(out);
#endif
    return common;
}

void PartialMatchBuilder::removeLast()
{
    root->removeLast();
}


void PartialMatchBuilder::serialize(MemoryBuffer & out)
{
    root->serialize(out);
}

void PartialMatchBuilder::gatherSingleTextCounts(unsigned * counts)
{
    StringBuffer prev;
    if (root)
        root->gatherSingleTextCounts(prev, counts);
}

void PartialMatchBuilder::trace()
{
    if (root)
        root->trace(0);
    printf("\n");
#if 0
    MemoryBuffer out;
    root->serialize(out);
    for (unsigned i=0; i < out.length(); i++)
        printf("%02x ", out.bytes()[i]);
    printf("\n\n");
#endif
}

unsigned PartialMatchBuilder::getCount()
{
    return root ? root->getCount() : 0;
}

unsigned PartialMatchBuilder::getSize()
{
    return root ? root->getSize() : 0;
}

//---------------------------------------------------------------------------------------------------------------------

InplaceNodeSearcher::InplaceNodeSearcher(unsigned _count, const byte * data, size32_t _keyLen, const byte * _nullRow)
: nodeData(data), nullRow(_nullRow), count(_count), keyLen(_keyLen)
{
}

void InplaceNodeSearcher::init(unsigned _count, const byte * data, size32_t _keyLen, const byte * _nullRow)
{
    count = _count;
    nodeData = data;
    keyLen = _keyLen;
    nullRow = _nullRow;
}

int InplaceNodeSearcher::compareValueAt(const char * search, unsigned int compareIndex) const
{
    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    while (offset < keyLen)
    {
        byte next = *finger++;
        SquashOp op = (SquashOp)(next >> 5);
        unsigned count = (next & 0x1f);
        if (count == 0)
            count = 32 + *finger++;

        switch (op)
        {
        case SqQuote:
        {
            unsigned numBytes = count;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                const byte nextFinger = finger[i];
                if (nextFinger != nextSearch)
                {
                    if (offset + i >= keyLen)
                        return 0;

                    if (nextFinger > nextSearch)
                    {
                        //This entry is larger than the search value => we have a match
                        return -1;
                    }
                    else
                    {
                        //This entry (and all children) are less than the search value
                        //=> the next entry is the match
                        return +1;
                    }
                }
            }
            search += numBytes;
            offset += numBytes;
            finger += numBytes;
            break;
        }
        case SqSingle:
        {
            const byte nextSearch = search[0];
            const byte nextFinger = specialSingleChars[count-1];
            if (nextFinger != nextSearch)
            {
                if (nextFinger > nextSearch)
                    return -1;
                else
                    return +1;
            }
            search += 1;
            offset += 1;
            break;
        }
        case SqSingleX:
        {
            {
                const byte nextSearch = search[0];
                const byte nextFinger = specialSingleChars[count-1];
                if (nextFinger != nextSearch)
                {
                    if (nextFinger > nextSearch)
                        return -1;
                    else
                        return +1;
                }
            }
            {
                const byte nextSearch = search[1];
                const byte nextFinger = *finger++;
                if (nextFinger != nextSearch)
                {
                    if (nextFinger > nextSearch)
                        return -1;
                    else
                        return +1;
                }
            }
            search += 2;
            offset += 2;
            break;
        }
        case SqZero:
        case SqSpace:
        {
            const byte nextFinger = "\x00\x20"[op];
            unsigned numBytes = count + repeatDelta;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                if (nextFinger != nextSearch)
                {
                    if (offset + i >= keyLen)
                        return 0;
                    if (nextFinger > nextSearch)
                        return -1;
                    else
                        return +1;
                }
            }
            search += numBytes;
            offset += numBytes;
            break;
        }
        case SqRepeat:
        {
            const byte nextFinger = *finger++;
            unsigned numBytes = count + repeatXDelta;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                if (nextFinger != nextSearch)
                {
                    if (offset + i >= keyLen)
                        return 0;
                    if (nextFinger > nextSearch)
                        return -1;
                    else
                        return +1;
                }
            }
            search += numBytes;
            offset += numBytes;
            break;
        }
        case SqOption:
        {
            const unsigned numOptions = count+1;
            byte sizeInfo = *finger++;
            //Top two bits are currently spare - it may make sense to move before count and use them for repetition
            byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
            byte bytesPerOffset = (sizeInfo & 7);

            //MORE: Duplicates can occur on the last byte of the key - if so we have a match
            if (bytesPerOffset == 0)
            {
                dbgassertex(resultNext == resultPrev+numOptions);
                return 0;
            }

            const byte * counts = finger + numOptions; // counts follow the data
            const byte nextSearch = search[0];
            for (unsigned i=0; i < numOptions; i++)
            {
                const byte nextFinger = finger[i];
                if (nextFinger > nextSearch)
                {
                    //This entry is greater than search => item(i) is the correct entry
                    if (i == 0)
                        return -1;

                    unsigned delta;
                    if (bytesPerCount == 0)
                        delta = i;
                    else
                        delta = readBytesEntry16(counts, i-1, bytesPerCount) + 1;
                    unsigned matchIndex = resultPrev + delta;
                    return (compareIndex >= matchIndex) ? -1 : +1;
                }
                else if (nextFinger < nextSearch)
                {
                    //The entry is < search => keep looping
                }
                else
                {
                    if (bytesPerCount == 0)
                    {
                        resultPrev += i;
                        resultNext = resultPrev+1;
                    }
                    else
                    {
                        //Exact match.  Reduce the range of the match counts using the running counts
                        //stored for each of the options, and continue matching.
                        resultNext = resultPrev + readBytesEntry16(counts, i, bytesPerCount)+1;
                        if (i > 0)
                            resultPrev += readBytesEntry16(counts, i-1, bytesPerCount)+1;
                    }

                    //If the compareIndex is < the lower bound for the match index the search value must be higher
                    if (compareIndex < resultPrev)
                        return +1;

                    //If the compareIndex is >= the upper bound for the match index the search value must be lower
                    if (compareIndex >= resultNext)
                        return -1;

                    const byte * offsets = counts + numOptions * bytesPerCount;
                    const byte * next = offsets + (numOptions-1) * bytesPerOffset;
                    finger = next;
                    if (i > 0)
                        finger += readBytesEntry16(offsets, i-1, bytesPerOffset);
                    search++;
                    offset++;
                    //Use a goto because we can't continue the outer loop from an inner loop
                    goto nextTree;
                }
            }

            //Search is > all elements
            return +1;
        }
        }

    nextTree:
        ;
    }

    return 0;
}

//Find the first row that is >= the search row
unsigned InplaceNodeSearcher::findGE(const unsigned len, const byte * search) const
{
    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    while (offset < keyLen)
    {
        byte next = *finger++;
        SquashOp op = (SquashOp)(next >> 5);
        unsigned count = (next & 0x1f);
        if (count == 0)
            count = 32 + *finger++;

        switch (op)
        {
        case SqQuote:
        {
            unsigned numBytes = count;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                const byte nextFinger = finger[i];
                if (nextFinger != nextSearch)
                {
                    //The following is not needed if this is only used to search <= keyLen
                    //if (offset + i >= keyLen) return resultPrev;

                    if (nextFinger > nextSearch)
                    {
                        //This entry is larger than the search value => we have a match
                        return resultPrev;
                    }
                    else
                    {
                        //This entry (and all children) are less than the search value
                        //=> the next entry is the match
                        return resultNext;
                    }
                }
            }
            search += numBytes;
            offset += numBytes;
            finger += numBytes;
            break;
        }
        case SqSingle:
        {
            const byte nextSearch = search[0];
            const byte nextFinger = specialSingleChars[count-1];
            if (nextFinger != nextSearch)
            {
                if (nextFinger > nextSearch)
                    return resultPrev;
                else
                    return resultNext;
            }
            search += 1;
            offset += 1;
            break;
        }
        case SqSingleX:
        {
            {
                const byte nextSearch = search[0];
                const byte nextFinger = specialSingleChars[count-1];
                if (nextFinger != nextSearch)
                {
                    if (nextFinger > nextSearch)
                        return resultPrev;
                    else
                        return resultNext;
                }
            }
            {
                const byte nextSearch = search[1];
                const byte nextFinger = *finger++;
                if (nextFinger != nextSearch)
                {
                    if (nextFinger > nextSearch)
                        return resultPrev;
                    else
                        return resultNext;
                }
            }
            search += 2;
            offset += 2;
            break;
        }
        case SqZero:
        case SqSpace:
        {
            const byte nextFinger = (op == SqZero) ? 0 : ' ';
            unsigned numBytes = count + repeatDelta;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                if (nextFinger != nextSearch)
                {
                    //The following is not needed if this is only used to search <= keyLen
                    //if (offset + i >= keyLen) return resultPrev;

                    if (nextFinger > nextSearch)
                        return resultPrev;
                    else
                        return resultNext;
                }
            }
            search += numBytes;
            offset += numBytes;
            break;
        }
        case SqRepeat:
        {
            const byte nextFinger = *finger++;
            unsigned numBytes = count + repeatXDelta;
            for (unsigned i=0; i < numBytes; i++)
            {
                const byte nextSearch = search[i];
                if (nextFinger != nextSearch)
                {
                    //The following is not needed if this is only used to search <= keyLen
                    //if (offset + i >= keyLen) return resultPrev;

                    if (nextFinger > nextSearch)
                        return resultPrev;
                    else
                        return resultNext;
                }
            }
            search += numBytes;
            offset += numBytes;
            break;
        }
        case SqOption:
        {
            //Read early to give more time to load before they are used
            byte sizeInfo = *finger++;
            const byte nextSearch = search[0];

            const unsigned numOptions = count+1;
            //Top two bits are currently spare - it may make sense to move before count and use them for repetition
            //Could also use fewer bits for the count if there was a use for the other bits.
            byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
            byte bytesPerOffset = (sizeInfo & 7);
            dbgassertex(bytesPerCount <= 2);
            dbgassertex(bytesPerOffset <= 2);

            //MORE: Duplicates can occur on the last byte of the key - if so we have a match
            //MORE: This is never executed - it should never be generated....
            dbgassertex(bytesPerOffset != 0);
            #if 0
            if (bytesPerOffset == 0)
            {
                dbgassertex(resultNext == resultPrev+numOptions);
                break;
            }
            #endif

            const byte * counts = finger + numOptions; // counts follow the data
            for (unsigned i=0; i < numOptions; i++)
            {
                const byte nextFinger = finger[i];
                if (likely(nextFinger < nextSearch))
                {
                    //The entry is < search => keep looping
                    continue;
                }

                if (nextFinger > nextSearch)
                {
                    //This entry is greater than search => this is the correct entry
                    if (bytesPerCount == 0)
                        return resultPrev + i;
                    if (i == 0)
                        return resultPrev;
                    return resultPrev + readBytesEntry16(counts, i-1, bytesPerCount) + 1;
                }

                const byte * offsets = counts + numOptions * bytesPerCount;

                //Avoid multiplications..  only works for 1-2 bytes per offset
                //const byte * next = offsets + (numOptions-1) * bytesPerOffset;
                const byte * next = offsets + ((numOptions-1) << (bytesPerOffset -1));

                if (i > 0)
                {
                    next += readBytesEntry16(offsets, i-1, bytesPerOffset);
                    __builtin_prefetch(next);
                }

                if (bytesPerCount == 0)
                {
                    resultPrev += i;
                    resultNext = resultPrev+1;
                }
                else
                {
                    //Exact match.  Reduce the range of the match counts using the running counts
                    //stored for each of the options, and continue matching.
                    resultNext = resultPrev + readBytesEntry16(counts, i, bytesPerCount)+1;
                    if (i > 0)
                        resultPrev += readBytesEntry16(counts, i-1, bytesPerCount)+1;
                }

                finger = next;
                search++;
                offset++;
                //Use a goto because we can't continue the outer loop from an inner loop
                goto nextTree;
            }

            //Did not match any => next value matches
            return resultNext;
        }
        }

    nextTree:
        ;
    }

    return resultPrev;
}

size32_t InplaceNodeSearcher::getValueAt(unsigned int searchIndex, char *key) const
{
    if (searchIndex >= count)
        return 0;

    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    while (offset < keyLen)
    {
        byte next = *finger++;
        SquashOp op = (SquashOp)(next >> 5);
        unsigned count = (next & 0x1f);
        if (count == 0)
            count = 32 + *finger++;

        switch (op)
        {
        case SqQuote:
        {
            unsigned i=0;
            unsigned numBytes = count;
            for (; i < numBytes; i++)
                key[offset+i] = finger[i];
            offset += numBytes;
            finger += numBytes;
            break;
        }
        case SqSingle:
        {
            key[offset] = specialSingleChars[count-1];
            offset += 1;
            break;
        }
        case SqSingleX:
        {
            key[offset] = specialSingleChars[count-1];
            key[offset+1] = finger[0];
            offset += 2;
            finger++;
            break;
        }
        case SqZero:
        case SqSpace:
        {
            const byte nextFinger = "\x00\x20"[op];
            unsigned numBytes = count + repeatDelta;
            for (unsigned i=0; i < numBytes; i++)
                key[offset+i] = nextFinger;
            offset += numBytes;
            break;
    }
        case SqRepeat:
        {
            const byte nextFinger = *finger++;
            unsigned numBytes = count + repeatXDelta;
            for (unsigned i=0; i < numBytes; i++)
                key[offset+i] = nextFinger;

            offset += numBytes;
            break;
        }
        case SqOption:
        {
            const unsigned numOptions = count+1;
            byte sizeInfo = *finger++;
            //Top two bits are currently spare - it may make sense to move before count and use them for repetition
            byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
            byte bytesPerOffset = (sizeInfo & 7);

            //MORE: Duplicates can occur after the last byte of the key - bytesPerOffset is set to 0 if this occurs
            if (bytesPerOffset == 0)
                break;

            const byte * counts = finger + numOptions; // counts follow the data
            unsigned option = 0;
            unsigned countPrev = 0;
            unsigned countNext = 1;
            if (bytesPerCount == 0)
            {
                option = searchIndex - resultPrev;
                countPrev = option;
                countNext = option+1;
            }
            else
            {
                countPrev = 0;
                for (unsigned i=0; i < numOptions; i++)
                {
                    countNext = readBytesEntry16(counts, i, bytesPerCount)+1;
                    if (searchIndex < resultPrev + countNext)
                    {
                        option = i;
                        break;
                    }
                    countPrev = countNext;
                }
            }
            key[offset++] = finger[option];

            resultNext = resultPrev + countNext;
            resultPrev = resultPrev + countPrev;

            const byte * offsets = counts + numOptions * bytesPerCount;
            const byte * next = offsets + (numOptions-1) * bytesPerOffset;
            finger = next;
            if (option > 0)
                finger += readBytesEntry16(offsets, option-1, bytesPerOffset);
            break;
        }
        }
    }

    return offset;
}

int InplaceNodeSearcher::compareValueAtFallback(const char *src, unsigned int index) const
{
    char temp[256] = { 0 };
    getValueAt(index, temp);
    return strcmp(src, temp);
}


//---------------------------------------------------------------------------------------------------------------------

CJHInplaceTreeNode::~CJHInplaceTreeNode()
{
    if (ownedPayload)
        free(payload);
}

int CJHInplaceTreeNode::compareValueAt(const char *src, unsigned int index) const
{
    dbgassertex(index < hdr.numKeys);
    return searcher.compareValueAt(src, index);
}

unsigned __int64 CJHInplaceTreeNode::getSequence(unsigned int index) const
{
    if (index >= hdr.numKeys) return 0;
    return firstSequence + index;
}

int CJHInplaceTreeNode::locateGE(const char * search, unsigned minIndex) const
{
    if (hdr.numKeys == 0) return 0;

#ifdef TIME_NODE_SEARCH
    CCycleTimer timer;
#endif

    unsigned int match = searcher.findGE(keyCompareLen, (const byte *)search);
    if (match < minIndex)
        match = minIndex;

#ifdef TIME_NODE_SEARCH
    unsigned __int64 elapsed = timer.elapsedCycles();
    if (isBranch())
        branchSearchCycles += elapsed;
    else
        leafSearchCycles += elapsed;
#endif

    return match;
}

// Only used for counts, so performance is less critical - implement fully once everything else is implemented
int CJHInplaceTreeNode::locateGT(const char * search, unsigned minIndex) const
{
    unsigned int a = minIndex;
    int b = getNumKeys();
    // Locate first record greater than src
    while ((int)a<b)
    {
        //MORE: Not sure why the index 'i' is subtly different to the GTE version
        //I suspect no good reason, and may mess up cache locality.
        int i = a+(b+1-a)/2;
        int rc = compareValueAt(search, i-1);
        if (rc>=0)
            a = i;
        else
            b = i-1;
    }
    return a;
}


void CJHInplaceTreeNode::load(CKeyHdr *_keyHdr, const void *rawData, offset_t _fpos, bool needCopy)
{
    CJHSearchNode::load(_keyHdr, rawData, _fpos, needCopy);
    keyCompareLen = keyHdr->getNodeKeyLength();
    keyLen = _keyHdr->getMaxKeyLength();
    if (isBranch())
        keyLen = keyHdr->getNodeKeyLength();

    const byte * nullRow = nullptr; //MORE: This should be implemented
    unsigned numKeys = hdr.numKeys;
    if (numKeys)
    {
        size32_t len = hdr.keyBytes;
        const size32_t padding = 8 - 1; // Ensure that unsigned8 values can be read "legally"
        const byte * data = ((const byte *)rawData) + sizeof(hdr);
        keyBuf = (char *) allocMem(len + padding);
        memcpy(keyBuf, data, len);
        memset(keyBuf+len, 0, padding);
        expandedSize = len;

        //Filepositions are stored as a packed base and an (optional) list of scaled compressed deltas
        data = (const byte *)keyBuf;
        byte sizeMask = *data++;
        if (sizeMask & 0x80)
            scaleFposByNodeSize = true;
        bytesPerPosition = (sizeMask & 0xF);

        firstSequence = readPacked64(data);
        positionData = data;

        if (bytesPerPosition != 0)
            data += (bytesPerPosition * (numKeys -1));

        if (isLeaf())
        {
            //Extra position serialized for leaves..
            data += bytesPerPosition;

            if (keyHdr->isVariable())
            {
                payloadOffsets.ensureCapacity(numKeys);
                unsigned offset = 0;
                const unsigned bytesPerPayloadLength = 2;
                for (unsigned i=0; i < numKeys; i++)
                {
                    offset += readBytesEntry32(data, i, bytesPerPayloadLength);
                    payloadOffsets.append(offset);
                }
                data += numKeys * bytesPerPayloadLength;
            }

            if (keyLen != keyCompareLen)
            {
                if (sizeMask & 0x40)
                {
                    size32_t uncompressedLen = readPacked32(data);
                    payload = (byte *)data;
                    data += uncompressedLen;
                }
                else
                {
                    //Currently the uncompressed data is still in memory (if it was moved to the end it could be thrown away)
                    //However that provides the scope to dynamically throw away the decompressed data
                    size32_t compressedLen = readPacked32(data);
                    Owned<IExpander> exp = createLZWExpander(true);
                    int len=exp->init(data);
                    if (len!=0)
                    {
                        payload = (byte *)malloc(len);
                        exp->expand(payload);
                        ownedPayload = true;
                    }
                    data += compressedLen;
                    expandedSize += len;
                }
            }
        }

        searcher.init(numKeys, data, keyCompareLen, nullRow);
    }
}

bool CJHInplaceTreeNode::getKeyAt(unsigned int index, char *dst) const
{
    if (index >= hdr.numKeys) return false;
    if (dst)
        searcher.getValueAt(index, dst);
    return true;
}

//---------------------------------------------------------------------------------------------------------------------

offset_t CJHInplaceBranchNode::getFPosAt(unsigned int index) const
{
    if (index >= hdr.numKeys) return 0;

    offset_t delta = 0;
    if ((bytesPerPosition > 0) && (index != 0))
        delta = readBytesEntry64(positionData, index-1, bytesPerPosition);
    else
        delta = index;

    if (scaleFposByNodeSize)
        delta *= getNodeDiskSize();

    return firstSequence + delta;
}


bool CJHInplaceBranchNode::fetchPayload(unsigned int num, char *dest) const
{
    throwUnexpected();
}

size32_t CJHInplaceBranchNode::getSizeAt(unsigned int index) const
{
    throwUnexpected();
}

//---------------------------------------------------------------------------------------------------------------------


offset_t CJHInplaceLeafNode::getFPosAt(unsigned int index) const
{
    throwUnexpected();
}

bool CJHInplaceLeafNode::fetchPayload(unsigned int index, char *dst) const
{
    if (index >= hdr.numKeys) return false;
    if (dst)
    {
        unsigned len = keyCompareLen;
        if (payloadOffsets.ordinality())
        {
            size32_t offset = 0;
            if (index)
                offset = payloadOffsets.item(index-1);
            size32_t endOffset = payloadOffsets.item(index);
            size32_t copyLen = endOffset - offset;

            if (copyLen)
            {
                memcpy(dst + len, payload + offset, copyLen);
                len += copyLen;
            }
        }
        if (keyHdr->hasSpecialFileposition())
        {
            offset_t filePosition = firstSequence;
            if (bytesPerPosition > 0)
                filePosition += readBytesEntry64(positionData, index, bytesPerPosition);
            memcpy(dst+len, &filePosition, sizeof(offset_t));
        }
    }
    return true;
}


size32_t CJHInplaceLeafNode::getSizeAt(unsigned int index) const
{
    //MORE: Change for variable length keys
    if (keyHdr->hasSpecialFileposition())
        return keyLen + sizeof(offset_t);
    else
        return keyLen;
}

//=========================================================================================================

CInplaceWriteNode::CInplaceWriteNode(offset_t _fpos, CKeyHdr *_keyHdr, bool isLeafNode)
: CWriteNode(_fpos, _keyHdr, isLeafNode)
{
    hdr.compressionType = InplaceCompression;
    keyCompareLen = keyHdr->getNodeKeyLength();
    lastKeyValue.allocate(keyCompareLen);
    lastSequence = 0;
}

void CInplaceWriteNode::saveLastKey(const void *data, size32_t size, unsigned __int64 sequence)
{
    memcpy(lastKeyValue.bufferBase(), data, keyCompareLen);
    lastSequence = sequence;
}


//---------------------------------------------------------------------------------------------------------

CInplaceBranchWriteNode::CInplaceBranchWriteNode(offset_t _fpos, CKeyHdr *_keyHdr, KeyBuildContext & _ctx)
: CInplaceWriteNode(_fpos, _keyHdr, false), ctx(_ctx), builder(keyCompareLen, _ctx.nullRow, false)
{
    nodeSize = _keyHdr->getNodeSize();
}

bool CInplaceBranchWriteNode::add(offset_t pos, const void * _data, size32_t size, unsigned __int64 sequence)
{
    if (0xffff == hdr.numKeys)
        return false;

    const byte * data = (const byte *)_data;
    unsigned oldSize = getDataSize();
    builder.add(size, data);
    if (positions.ordinality())
        assertex(positions.tos() <= pos);
    positions.append(pos);
    if (getDataSize() > maxBytes-hdr.keyBytes)
    {
        if (getDataSize() > maxBytes-hdr.keyBytes)
        {
            builder.removeLast();
            positions.pop();
            unsigned nowSize = getDataSize();
            assertex(oldSize == nowSize);
#ifdef TRACE_BUILDING
            printf("---- branch ----\n");
            builder.trace();
#endif
            return false;
        }
    }
    if (scaleFposByNodeSize && ((pos % nodeSize) != 0))
        scaleFposByNodeSize = false;

    saveLastKey(data, size, sequence);
    hdr.numKeys++;
    return true;
}

unsigned CInplaceBranchWriteNode::getDataSize()
{
    if (positions.ordinality() == 0)
        return 0;

    //MORE: Cache this, and calculate the incremental increase in size

    //Compress the filepositions by
    //a) storing them as deltas from the first
    //b) scaling by nodeSize if possible.
    //c) storing in the minimum number of bytes possible.
    unsigned __int64 firstPosition = positions.item(0);
    unsigned __int64 maxDeltaPosition = positions.tos() - firstPosition;
    if (scaleFposByNodeSize)
        maxDeltaPosition /= nodeSize;
    unsigned bytesPerPosition = 0;
    if (maxDeltaPosition + 1 != positions.ordinality())
        bytesPerPosition = bytesRequired(maxDeltaPosition);

    unsigned posSize = 1 + sizePacked(firstPosition) + bytesPerPosition * (positions.ordinality() - 1);
    return posSize + builder.getSize();
}

void CInplaceBranchWriteNode::write(IFileIOStream *out, CRC32 *crc)
{
    hdr.keyBytes = getDataSize();

    MemoryBuffer data;
    data.setBuffer(maxBytes, keyPtr, false);
    data.setWritePos(0);

    if (positions.ordinality())
    {
        //Pack these by scaling and reducing the number of bytes
        unsigned __int64 firstPosition = positions.item(0);
        unsigned __int64 maxPosition = positions.tos() - firstPosition;
        if (scaleFposByNodeSize)
            maxPosition /= nodeSize;

        unsigned bytesPerPosition = 0;
        if (maxPosition + 1 != positions.ordinality())
            bytesPerPosition = bytesRequired(maxPosition);

        byte sizeMask = (byte)bytesPerPosition | (scaleFposByNodeSize ? 0x80 : 0);
        data.append(sizeMask);
        serializePacked(data, firstPosition);

        if (bytesPerPosition != 0)
        {
            for (unsigned i=1; i < positions.ordinality(); i++)
            {
                unsigned __int64 delta = positions.item(i) - firstPosition;
                if (scaleFposByNodeSize)
                    delta /= nodeSize;
                serializeBytes(data, delta, bytesPerPosition);
            }
        }

        builder.serialize(data);
        ctx.numKeyedDuplicates += builder.numDuplicates;
        builder.gatherSingleTextCounts(ctx.singleCounts);

        assertex(data.length() == getDataSize());
    }

    CWriteNode::write(out, crc);
}


//=========================================================================================================

CInplaceLeafWriteNode::CInplaceLeafWriteNode(offset_t _fpos, CKeyHdr *_keyHdr, KeyBuildContext & _ctx, size32_t _lastKeyedFieldOffset)
: CInplaceWriteNode(_fpos, _keyHdr, true), ctx(_ctx), builder(keyCompareLen, _ctx.nullRow, false), lastKeyedFieldOffset(_lastKeyedFieldOffset)
{
    nodeSize = _keyHdr->getNodeSize();
    keyLen = keyHdr->getMaxKeyLength();
    isVariable = keyHdr->isVariable();
    rowCompression = (keyHdr->getKeyType()&HTREE_QUICK_COMPRESSED_KEY)==HTREE_QUICK_COMPRESSED_KEY;

    uncompressed.clear();
    compressed.allocate(nodeSize);
}

bool CInplaceLeafWriteNode::add(offset_t pos, const void * _data, size32_t size, unsigned __int64 sequence)
{
    if (0xffff == hdr.numKeys)
        return false;

    if ((0 == hdr.numKeys) && (keyLen != keyCompareLen))
#ifdef USE_ZSTD_COMPRESSION
        zstdComp.setown(new ZStdBlockCompressor(new ZStdCDictionary(nullptr, 0, 20), nodeSize));  // MORE - use a sensible dictionary (somehow!) and don't leak it
        // MORE - nodesize is wrong too
#else
        lzwcomp.open(compressed.mem(), nodeSize, isVariable, rowCompression);   // The payload portion may be stored lzw-compressed
#endif
    
    __uint64 savedMinPosition = minPosition;
    __uint64 savedMaxPosition = maxPosition;
    const byte * data = (const byte *)_data;
    unsigned oldSize = getDataSize();
    size32_t commonBytes = builder.add(keyCompareLen, data);
#ifndef USE_ZSTD_COMPRESSION
    // Decide whether to start a new block, based on (a) size of this one and (b) how many of the keyed fields have changed value
    if (numRowsInBlock)
    {
        if (commonBytes == keyCompareLen)
        {
            // Start a new block if this one has got larger than some threshold
        }
        else
        {
            if (commonBytes >= lastKeyedFieldOffset)
                ; // Just one field changed
            else
                ; // more then one
            // Start a new block if this one has got larger than some other, smaller, threshold. If we want to, we can factor in how many fields in the key
            // have changed, and also how much space is left on the node.
            // It's possible that a row would fit on this node in the current lzwcomp, but not in a new one - but if so, is squeezing a row in a good idea?
            // It may depend on whether the NEXT row has a matching key...
            // Some stats on average number of rows per key / all-but-last-key might be useful
            lzwcomp.close();
            size32_t buflen = lzwcomp.buflen();  // MORE - need to store this info too! And numRowsInBlock
            lzwcomp.open(((byte *) compressed.mem())+buflen, nodeSize, isVariable, rowCompression);   // The payload portion may be stored lzw-compressed
            numRowsInBlock = 0;
        }
    }
#endif
    if (positions.ordinality())
    {
        if (pos < minPosition)
            minPosition = pos;
        if (pos > maxPosition)
            maxPosition = pos;
    }
    else
    {
        minPosition = pos;
        maxPosition = pos;
    }

    positions.append(pos);

    const char * extraData = (const char *)(data + keyCompareLen);
    size32_t extraSize = size - keyCompareLen;

    //Save the uncompressed version in case it is smaller than the compressed version...
    //but only up to a certain size - to prevent very large compression ratios creating giant buffers.
    const size32_t uncompressedLimit = 0x2000;   // perhaps nodeSize would be better?
    const size32_t prevUncompressedLen = uncompressed.length();
    if (uncompressed.length() < uncompressedLimit)
        uncompressed.append(extraSize, extraData);
    if (isVariable)
        payloadLengths.append(extraSize);

    size32_t required = getDataSize();              // The size currently required to write out this node, including the new key but NOT the new payload
    size32_t safetyMargin = (keyLen != keyCompareLen) ? 16 : 0;  // I think the test is "is there a payload"
    size32_t available = maxBytes - safetyMargin;   // Total amount we can fit on a node

    bool hasSpace = (required <= available);
    if (hasSpace && (size != keyCompareLen))        // See if we can fit the payload (compressed) into this node
    {
#ifdef USE_ZSTD_COMPRESSION
        size32_t oldCompressedSize = zstdComp->getCompressedSize();
        size32_t thisRowSize = zstdComp->compress(extraData, extraSize);
        size32_t zstdSpace = oldCompressedSize + (maxBytes - required);
        if (thisRowSize > zstdSpace)
        {
            zstdComp->setSize(oldCompressedSize);  
            hasSpace = false;
        }
#else
        size32_t oldLzwSize = lzwcomp.buflen();
        size32_t lzwSpace = oldLzwSize + (maxBytes - required);
        if (!lzwcomp.adjustLimit(lzwSpace) || !lzwcomp.write(extraData, extraSize))
        {
            oldSize += (lzwcomp.buflen() - oldLzwSize);
            hasSpace = false;
        }
        else
            numRowsInBlock++;
#endif
    }

    if (!hasSpace)
    {
        //Reverse all the operations that have just been performed
        if (isVariable)
            payloadLengths.pop();
        uncompressed.setLength(prevUncompressedLen);

        builder.removeLast();
        positions.pop();
        minPosition = savedMinPosition;
        maxPosition = savedMaxPosition;
        unsigned nowSize = getDataSize();
        assertex(oldSize == nowSize);
#ifdef TRACE_BUILDING
        printf("---- leaf ----\n");
        builder.trace();
#endif
        return false;
    }

    saveLastKey(data, size, sequence);
    hdr.numKeys++;
    return true;
}

unsigned CInplaceLeafWriteNode::getDataSize()
{
    if (positions.ordinality() == 0)
        return 0;

    //MORE: Cache this, and calculate the incremental increase in size

    //Compress the filepositions by
    //a) storing them as deltas from the first
    //b) scaling by nodeSize if possible.
    //c) storing in the minimum number of bytes possible.
    unsigned bytesPerPosition = 0;
    if (minPosition != maxPosition)
        bytesPerPosition = bytesRequired(maxPosition-minPosition);

    unsigned posSize = 1 + sizePacked(minPosition) + bytesPerPosition * positions.ordinality();
    unsigned offsetSize = payloadLengths.ordinality() * 2;  // MORE: Could probably compress
    unsigned payloadSize = 0;
    if (keyLen != keyCompareLen)
    {
#ifdef USE_ZSTD_COMPRESSION
        size32_t payloadLen = zstdComp->getCompressedSize();
#else
        size32_t payloadLen = lzwcomp.buflen() + ((byte *) lzwcomp.bufptr() - (byte *) compressed.mem());
#endif
        payloadSize = sizePacked(payloadLen) + payloadLen;
    }
    return posSize + offsetSize + payloadSize + builder.getSize();
}

void CInplaceLeafWriteNode::write(IFileIOStream *out, CRC32 *crc)
{
#ifndef USE_ZSTD_COMPRESSION
    if (keyLen != keyCompareLen)
        lzwcomp.close();
#endif
    hdr.keyBytes = getDataSize();

    MemoryBuffer data;
    data.setBuffer(maxBytes, keyPtr, false);
    data.setWritePos(0);

    if (positions.ordinality())
    {
        //Pack these by scaling and reducing the number of bytes
        unsigned bytesPerPosition = 0;
        if (minPosition != maxPosition)
            bytesPerPosition = bytesRequired(maxPosition-minPosition);

        bool useUncompressed = false;
        byte sizeMask = (byte)bytesPerPosition | (scaleFposByNodeSize ? 0x80 : 0) | (useUncompressed ? 0x40 : 0);
        data.append(sizeMask);
        serializePacked(data, minPosition);
        if (bytesPerPosition != 0)
        {
            for (unsigned i=0; i < positions.ordinality(); i++)
            {
                unsigned __int64 delta = positions.item(i) - minPosition;
                serializeBytes(data, delta, bytesPerPosition);
            }
        }

        if (isVariable)
        {
            //MORE: These may well benefit from packing....
            ForEachItemIn(i, payloadLengths)
                data.append((unsigned short)payloadLengths.item(i));
        }

        if (keyLen != keyCompareLen)
        {
            if (useUncompressed)
            {
                if (isVariable)
                    serializePacked(data, uncompressed.length());
                data.append(uncompressed.length(), uncompressed.bufferBase());
            }
            else if (keyLen != keyCompareLen)
            {
#ifdef USE_ZSTD_COMPRESSION
                size32_t payloadLen = zstdComp->getCompressedSize();
                serializePacked(data, payloadLen);
                data.append(payloadLen, zstdComp->queryCompressedData());
#else
                size32_t payloadLen = lzwcomp.buflen() + ((byte *) lzwcomp.bufptr() - (byte *) compressed.get()); // Length includes all prior blocks too
                serializePacked(data, payloadLen);
                data.append(payloadLen, compressed.get());
#endif
            }
        }

        builder.serialize(data);
        ctx.numKeyedDuplicates += builder.numDuplicates;
//        builder.gatherSingleTextCounts(ctx.singleCounts);

        assertex(data.length() == getDataSize());
    }

    CWriteNode::write(out, crc);
}


//=========================================================================================================


InplaceIndexCompressor::InplaceIndexCompressor(size32_t keyedSize, IHThorIndexWriteArg * helper)
{
    IOutputMetaData * format = helper->queryDiskRecordSize();
    if (format)
    {
        //Create a representation of the null row - potentially used for the new compression algorithm
        byte * nullRow = new byte[keyedSize];
        RtlStaticRowBuilder rowBuilder(nullRow, keyedSize);

        auto & meta = format->queryRecordAccessor(true);
        size32_t offset = 0;
        for (unsigned idx = 0; idx < meta.getNumFields() && offset < keyedSize; idx++)
        {
            const RtlFieldInfo *field = meta.queryField(idx);
            offset = field->type->buildNull(rowBuilder, offset, field);
            lastKeyedFieldOffset = offset;
        }
        ctx.nullRow = nullRow;
    }
    else
        lastKeyedFieldOffset = keyedSize-1;
}

#ifdef _USE_CPPUNIT
#include "unittests.hpp"
#include "eclrtl.hpp"

class TestInplaceNodeSearcher : public InplaceNodeSearcher
{
public:
    TestInplaceNodeSearcher(unsigned _count, const byte * data, size32_t _keyLen, const byte * _nullRow) : InplaceNodeSearcher(_count,  data, _keyLen, _nullRow)
    {
    }

    void doFind(const char * search)
    {
        unsigned match = findGE(strlen(search), (const byte *)search);
        printf("('%s': %u) ", search, match);
    }

    void find(const char * search)
    {
        StringBuffer text;
        doFind(search);
        doFind(text.clear().append(strlen(search)-1, search));
        doFind(text.clear().append(strlen(search)-1, search).append('a'));
        doFind(text.clear().append(strlen(search)-1, search).append('z'));
        doFind(text.clear().append(search).append(' '));
        doFind(text.clear().append(search).append('\t'));
        doFind(text.clear().append(search).append('z'));
        printf("\n");
    }
};


static int normalizeCompare(int x)
{
    return (x < 0) ? -1 : (x > 0) ? 1 : 0;
}
class InplaceIndexTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE( InplaceIndexTest  );
        //CPPUNIT_TEST(testBytesFromFirstTiming);
        CPPUNIT_TEST(testSearching);
    CPPUNIT_TEST_SUITE_END();

    void testBytesFromFirstTiming()
    {
        for (unsigned i=0; i <= 0xff; i++)
            assertex(numExtraBytesFromFirst1(i) == numExtraBytesFromFirst2(i));
        unsigned total = 0;
        {
            CCycleTimer timer;
            for (unsigned i=0; i < 0xffffff; i++)
                total += numExtraBytesFromFirst1(i);
            printf("%llu\n", timer.elapsedNs());
        }
        {
            CCycleTimer timer;
            for (unsigned i2=0; i2 < 0xffffff; i2++)
                total += numExtraBytesFromFirst2(i2);
            printf("%llu\n", timer.elapsedNs());
        }
        printf("%u\n", total);
    }

    void testSearching()
    {
        const size32_t keyLen = 8;
        bool optimizeTrailing = false;  // remove trailing bytes that match the null row.
        const byte * nullRow = (const byte *)"                                   ";
        PartialMatchBuilder builder(keyLen, nullRow, optimizeTrailing);

        const char * entries[] = {
            "abel    ",
            "abigail ",
            "absalom ",
            "barry   ",
            "barty   ",
            "bill    ",
            "brian   ",
            "briany  ",
            "charlie ",
            "charlie ",
            "chhhhhhs",
            "georgina",
            "georgina",
            "georginb",
            "jim     ",
            "jimmy   ",
            "john    ",
        };
        unsigned numEntries = _elements_in(entries);
        for (unsigned i=0; i < numEntries; i++)
        {
            const char * entry = entries[i];
            builder.add(strlen(entry), entry);
        }

        MemoryBuffer buffer;
        builder.serialize(buffer);
        DBGLOG("Raw size = %u", keyLen * numEntries);
        DBGLOG("Serialized size = %u", buffer.length());
        builder.trace();

        TestInplaceNodeSearcher searcher(builder.getCount(), buffer.bytes(), keyLen, nullRow);

        for (unsigned i=0; i < numEntries; i++)
        {
            const char * entry = entries[i];
            StringBuffer s;
            find(entry, [&searcher,&s,numEntries](const char * search) {
                unsigned match = searcher.findGE(strlen(search), (const byte *)search);
                s.appendf("('%s': %u) ", search, match);
                if (match > 0  && !(searcher.compareValueAt(search, match-1) >= 0))
                {
                    s.append("<");
                    //assertex(searcher.compareValueAt(search, match-1) >= 0);
                }
                if (match < numEntries && !(searcher.compareValueAt(search, match) <= 0))
                {
                    s.append(">");
                    //assertex(searcher.compareValueAt(search, match) <= 0);
                }
            });
            DBGLOG("%s", s.str());
        }

        for (unsigned i=0; i < numEntries; i++)
        {
            char result[256] = {0};
            const char * entry = entries[i];

            if (!searcher.getValueAt(i, result))
                printf("%u: getValue() failed\n", i);
            else if (!streq(entry, result))
                printf("%u: '%s', '%s'\n", i, entry, result);

            auto callback = [numEntries, entries, &searcher](const char * search)
            {
                for (unsigned j= 0; j < numEntries; j++)
                {
                    int expected = normalizeCompare(strcmp(search, entries[j]));
                    int actual = normalizeCompare(searcher.compareValueAt(search, j));
                    if (expected != actual)
                        printf("compareValueAt('%s', %u)=%d, expected %d\n", search, j, actual, expected);
                }
            };
            find(entry, callback);
        }

        exit(0);
    }

    void find(const char * search, std::function<void(const char *)> callback)
    {
        callback(search);

        unsigned searchLen = strlen(search);
        unsigned trimLen = rtlTrimStrLen(searchLen, search);
        StringBuffer text;
        text.clear().append(search).setCharAt(trimLen-1, ' '); callback(text);
        text.clear().append(search).setCharAt(trimLen-1, 'a'); callback(text);
        text.clear().append(search).setCharAt(trimLen-1, 'z'); callback(text);
        if (searchLen != trimLen)
        {
            text.clear().append(search).setCharAt(trimLen, '\t'); callback(text);
            text.clear().append(search).setCharAt(trimLen, 'a'); callback(text);
            text.clear().append(search).setCharAt(trimLen, 'z'); callback(text);
        }
    }

};

CPPUNIT_TEST_SUITE_REGISTRATION( InplaceIndexTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( InplaceIndexTest, "InplaceIndexTest" );

#endif


#if 0
//Old code:
int InplaceNodeSearcher::compareValueAt(const char * search, unsigned int compareIndex) const
{
    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    for (;;)
    {
        //An alternating sequence of match, opt blocks
        const unsigned numBytes = readPacked32(finger);
        for (unsigned i=0; i < numBytes; i++)
        {
            const byte nextSearch = search[i];
            const byte nextFinger = finger[i];
            if (nextFinger > nextSearch)
            {
                //This entry is larger than the search value => we have a match
                return -1;
            }
            else if (nextFinger < nextSearch)
            {
                //This entry (and all children) are less than the search value
                return +1;
            }
        }
        search += numBytes;
        offset += numBytes;
        if (resultNext == resultPrev+1)
            break;

        finger += numBytes;

        //Now an opt block
        const unsigned numOptions = readPacked32(finger) + 2;
        byte sizeInfo = *finger++;
        //Top two bits are currently spare - it may make sense to move before count and use them for repetition
        byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
        byte bytesPerOffset = (sizeInfo & 7);

        //MORE: Duplicates can occur on the last byte of the key - if so we have a match
        if (bytesPerOffset == 0)
            break;

        const byte * counts = finger + numOptions; // counts follow the data
        const byte nextSearch = search[0];
        for (unsigned i=0; i < numOptions; i++)
        {
            const byte nextFinger = finger[i];
            if (nextFinger > nextSearch)
            {
                //This entry is greater than search => item(i) is the correct entry
                if (i == 0)
                    return -1;

                unsigned delta;
                if (bytesPerCount == 0)
                    delta = i;
                else
                    delta = readBytesEntry32(counts, i-1, bytesPerCount) + 1;
                unsigned matchIndex = resultPrev + delta;
                return (compareIndex >= matchIndex) ? -1 : +1;
            }
            else if (nextFinger < nextSearch)
            {
                //The entry is < search => keep looping
            }
            else
            {
                if (bytesPerCount == 0)
                {
                    resultPrev += i;
                    resultNext = resultPrev+1;
                }
                else
                {
                    //Exact match.  Reduce the range of the match counts using the running counts
                    //stored for each of the options, and continue matching.
                    resultNext = resultPrev + readBytesEntry32(counts, i, bytesPerCount)+1;
                    if (i > 0)
                        resultPrev += readBytesEntry32(counts, i-1, bytesPerCount)+1;
                }

                //If the compareIndex is < the lower bound for the match index the search value must be higher
                if (compareIndex < resultPrev)
                    return +1;

                //If the compareIndex is >= the upper bound for the match index the search value must be lower
                if (compareIndex >= resultNext)
                    return -1;

                const byte * offsets = counts + numOptions * bytesPerCount;
                const byte * next = offsets + (numOptions-1) * bytesPerOffset;
                finger = next;
                if (i > 0)
                    finger += readBytesEntry32(offsets, i-1, bytesPerOffset);
                search++;
                offset++;
                //Use a goto because we can't continue the outer loop from an inner loop
                goto nextTree;
            }
        }
        //Search is > all elements
        return +1;

    nextTree:
        ;
    }

    //compare with the null value
    for (unsigned i=offset; i < keyLen; i++)
    {
        const byte nextSearch = search[i-offset];
        const byte nextFinger = nullRow[i];
        if (nextFinger > nextSearch)
            return -1;
        else if (nextFinger < nextSearch)
            return +1;
    }

    return 0;
}

//Find the first row that is >= the search row
unsigned InplaceNodeSearcher::findGE(const unsigned len, const byte * search) const
{
    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    for (;;)
    {
        //An alternating sequence of match, opt blocks
        const unsigned numBytes = readPacked32(finger);
        unsigned i=0;
#if 0
        if (unlikely(numBytes >= 4))
        {
            do
            {
                //Technically undefined behaviour because the data was not originally
                //defined as unsigned values, access will be misaligned.
                const unsigned nextSearch = *(const unsigned *)(search + i);
                const unsigned nextFinger = *(const unsigned *)(finger + i);
                if (nextSearch != nextFinger)
                {
                    const unsigned revNextSearch = reverseBytes(nextSearch);
                    const unsigned revNextFinger = reverseBytes(nextFinger);
                    if (revNextFinger > revNextSearch)
                        return resultPrev;
                    else
                        return resultNext;
                }
                i += 4;
            } while (i + 4 <= numBytes);
        }
#endif

        for (; i < numBytes; i++)
        {
            const byte nextSearch = search[i];
            const byte nextFinger = finger[i];
            if (nextFinger > nextSearch)
            {
                //This entry is larger than the search value => we have a match
                return resultPrev;
            }
            else if (nextFinger < nextSearch)
            {
                //This entry (and all children) are less than the search value
                //=> the next entry is the match
                return resultNext;
            }
        }
        search += numBytes;
        offset += numBytes;
        if (resultNext == resultPrev+1)
            break;

        finger += numBytes;

        //Now an opt block
        const unsigned numOptions = readPacked32(finger) + 2;
        byte sizeInfo = *finger++;
        //Top two bits are currently spare - it may make sense to move before count and use them for repetition
        byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
        byte bytesPerOffset = (sizeInfo & 7);

        //MORE: Duplicates can occur on the last byte of the key - if so we have a match
        if (bytesPerOffset == 0)
        {
            dbgassertex(resultNext == resultPrev+numOptions);
            break;
        }

        const byte * counts = finger + numOptions; // counts follow the data
        const byte nextSearch = search[0];
        for (unsigned i=0; i < numOptions; i++)
        {
            const byte nextFinger = finger[i];
            if (nextFinger > nextSearch)
            {
                //This entry is greater than search => this is the correct entry
                if (bytesPerCount == 0)
                    return resultPrev + i;
                if (i == 0)
                    return resultPrev;
                return resultPrev + readBytesEntry32(counts, i-1, bytesPerCount) + 1;
            }
            else if (nextFinger < nextSearch)
            {
                //The entry is < search => keep looping
            }
            else
            {
                if (bytesPerCount == 0)
                {
                    resultPrev += i;
                    resultNext = resultPrev+1;
                }
                else
                {
                    //Exact match.  Reduce the range of the match counts using the running counts
                    //stored for each of the options, and continue matching.
                    resultNext = resultPrev + readBytesEntry32(counts, i, bytesPerCount)+1;
                    if (i > 0)
                        resultPrev += readBytesEntry32(counts, i-1, bytesPerCount)+1;
                }

                const byte * offsets = counts + numOptions * bytesPerCount;
                const byte * next = offsets + (numOptions-1) * bytesPerOffset;
                finger = next;
                if (i > 0)
                    finger += readBytesEntry32(offsets, i-1, bytesPerOffset);
                search++;
                offset++;
                //Use a goto because we can't continue the outer loop from an inner loop
                goto nextTree;
            }
        }
        //Did not match any => next value matches
        return resultNext;

    nextTree:
        ;
    }

    //compare with the null value
    for (unsigned i=offset; i < keyLen; i++)
    {
        const byte nextSearch = search[i-offset];
        const byte nextFinger = nullRow[i];
        if (nextFinger > nextSearch)
            return resultPrev;
        else if (nextFinger < nextSearch)
            return resultNext;
    }
    return resultPrev;
}

bool InplaceNodeSearcher::getValueAt(unsigned int searchIndex, char *key) const
{
    if (searchIndex >= count)
        return false;

    unsigned resultPrev = 0;
    unsigned resultNext = count;
    const byte * finger = nodeData;
    unsigned offset = 0;

    for (;;)
    {
        //An alternating sequence of match, opt blocks
        const unsigned numBytes = readPacked32(finger);
        for (unsigned i=0; i < numBytes; i++)
            key[offset+i] = finger[i];
        offset += numBytes;
        if (resultNext == resultPrev+1)
            break;

        finger += numBytes;

        //Now an opt block
        const unsigned numOptions = readPacked32(finger) + 2;
        byte sizeInfo = *finger++;
        //Top two bits are currently spare - it may make sense to move before count and use them for repetition
        byte bytesPerCount = (sizeInfo >> 3) & 7; // could pack other information in....
        byte bytesPerOffset = (sizeInfo & 7);

        //MORE: Duplicates can occur after the last byte of the key - bytesPerOffset is set to 0 if this occurs
        if (bytesPerOffset == 0)
            break;

        const byte * counts = finger + numOptions; // counts follow the data
        unsigned option = 0;
        unsigned countPrev = 0;
        unsigned countNext = 1;
        if (bytesPerCount == 0)
        {
            option = searchIndex - resultPrev;
            countPrev = option;
            countNext = option+1;
        }
        else
        {
            countPrev = 0;
            for (unsigned i=0; i < numOptions; i++)
            {
                countNext = readBytesEntry32(counts, i, bytesPerCount)+1;
                if (searchIndex < resultPrev + countNext)
                {
                    option = i;
                    break;
                }
                countPrev = countNext;
            }
        }
        key[offset++] = finger[option];

        resultNext = resultPrev + countNext;
        resultPrev = resultPrev + countPrev;

        const byte * offsets = counts + numOptions * bytesPerCount;
        const byte * next = offsets + (numOptions-1) * bytesPerOffset;
        finger = next;
        if (option > 0)
            finger += readBytesEntry32(offsets, option-1, bytesPerOffset);
    }

    //Finally pad with any values from the nullRow.
    for (unsigned i=offset; i < keyLen; i++)
        key[i] = nullRow[i];
    return true;
}

#endif