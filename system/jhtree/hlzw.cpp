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

#include "platform.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include <stdlib.h>
#include <signal.h>

#ifndef _WIN32
#include <sys/time.h>
#include <sys/stat.h>
#include <assert.h>
#endif

#include "jmisc.hpp"
#include "hlzw.h"

KeyCompressor::~KeyCompressor()
{
    if (NULL != comp)
    {
        close();
        // temporary tracing
        WARNLOG("KeyCompressor not explicitly closed");
    }
}

// MORE - thoughts on row compressed keys
/*
 * 1. The ones where we can decompress a row at a time will be used if people say COMPRESSED(ROW). Not sure anyone does.
 * 2. The ability to compare without decompressing won't be used with new field filter mode.
 * 3. The flag to say "don't need a copy" - designed for memory-mapped indexes and the main reason to think about this format I think - was never passed in
 * 4. Unclear whether memory-mapped mode or row-compressed indexes or in-place compare have ever been used in anger
 * 5. Even if flag is set some nodes appear to not be compressed - or at least the decompress code supports that,
 *    though I don't see corresponding code at compress time. Testing suggests that non-leaf nodes are not row-compressed, but that's a slightly different issue.
 *    The decompress code suggests that a different expander is used in such cases? Maybe related to USE_RANDROWDIFF setting - but that is hard-wired.
 * 6. Don't support variable-size rows. So testing for rowexp in the var node code is redundant. Removed.
 * 7. A compression mode that did not compress keys but did compress payload would be useful.
 * 8. Is COMPRESSED(ROW) documented? YES. Included in regression suite? ONCE. Used in any known code? If the answer to all of the above is no perhaps should be deprecated.
 * 9. If test in regression suite fails, perhaps should be disabled sooner!
 *      - forced all keys to use COMPRESSED_ROW if fixed size, ran regression suite, got cores and valgrind issues on keyed_join3.ecl and others
 *      - errors if try to read a blob node
 * 10. COL_PREFIX is used on all indexes and affects all non-leaf nodes - but may not buy us a lot (and would not need to expand if was not set).
 *
 */
void KeyCompressor::open(void *blk,int blksize,bool _isVariable, bool rowcompression)
{
    isVariable = _isVariable;
    isBlob = false;
    curOffset = 0;
    ::Release(comp);
    comp = NULL;
    if (rowcompression&&!_isVariable) {
        if (USE_RANDROWDIFF)
            comp = createRandRDiffCompressor();
        else
            comp = createRDiffCompressor();
    }
    else
        comp = createLZWCompressor(true);
    comp->open(blk,blksize);
}

void KeyCompressor::openBlob(void *blk,int blksize)
{
    isVariable = false;
    isBlob = true;
    curOffset = 0;
    ::Release(comp);
    comp = NULL;
    comp = createLZWCompressor(true);
    comp->open(blk,blksize);
}

int KeyCompressor::writekey(offset_t fPtr, const char *key, unsigned datalength, unsigned __int64 sequence)
{
    assert(!isBlob);
    comp->startblock(); // start transaction
    // first write out length if variable
    if (isVariable) {
        KEYRECSIZE_T rs = datalength;
        _WINREV(rs);
        if (comp->write(&rs, sizeof(rs))!=sizeof(rs)) {
            close();
            return 0;
        }
    }
    // then write out fpos and key
    _WINREV(fPtr);
    if (comp->write(&fPtr,sizeof(offset_t))!=sizeof(offset_t)) {
        close();
        return 0;
    }
    if (comp->write(key,datalength)!=datalength) {
        close();
        return 0;
    }
    comp->commitblock();    // end transaction

    return 1;
}

unsigned KeyCompressor::writeBlob(const char *data, unsigned datalength)
{
    assert(isBlob);
    assert(datalength);
    if (!comp)
        return 0;


    unsigned originalOffset = curOffset;
    comp->startblock(); // start transaction
    char zero = 0;
    while (curOffset & 0xf)
    {
        if (comp->write(&zero,sizeof(zero))!=sizeof(zero)) {
            close();
            curOffset = originalOffset;
            return 0;
        }
        curOffset++;
    }

    unsigned rdatalength = datalength;
    _WINREV(rdatalength);
    if (comp->write(&rdatalength, sizeof(rdatalength))!=sizeof(rdatalength)) {
        close();
        curOffset = originalOffset;
        return 0;
    }
    curOffset += sizeof(datalength);

    unsigned written = 0;
    while (written < datalength && curOffset < 0x100000)
    {
        if (comp->write(data,sizeof(*data))!=sizeof(*data))
        {
            if (!written)
            {
                close();
                curOffset = originalOffset;
                return 0; // only room to put the length - don't!
            }
            break;
        }
        curOffset++;
        written++;
        data++;
        comp->startblock();
    }
    comp->commitblock();
    if (written != datalength)
        close();
    return written;
}


void KeyCompressor::close()
{ // gets called either when write failed or explicitly by client
    if (comp!=NULL) {
        comp->close();
        bufp = comp->bufptr();
        bufl = comp->buflen();
        comp->Release();
        comp = NULL;
    }
}


