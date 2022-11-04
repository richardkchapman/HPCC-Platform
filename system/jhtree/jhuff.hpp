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

/*
  Huffman-based compression using field-specific global dictionaries
*/

#ifndef JHUFF_HPP
#define JHUFF_HPP

#include "jlib.hpp"
#include "jhtree.hpp"

interface ISymbolTableEntry : public IInterface
{
    virtual unsigned queryCodeLength() const = 0;
    virtual unsigned queryCode() const = 0;
    virtual void dump() const = 0;
};

interface IHuffSymbolTable : public IInterface
{
    virtual unsigned numSymbols() const = 0;
    virtual void setCode(unsigned symidx, unsigned codeLength, unsigned code) = 0;
    virtual void dump(unsigned idx) const = 0;
    virtual void dumpAll() const = 0;
    virtual ISymbolTableEntry &queryEntry(unsigned idx) const = 0;
};

interface IHuffBuilder : public IInterface
{
    virtual ISymbolTableEntry *decode(unsigned code) const = 0;
};

// Note - Updates the provided symbol table to allocate Huffman codes according to supplied frequency information
extern jhtree_decl IHuffBuilder *createHuffBuilder(IHuffSymbolTable *symbols, uint64_t *counts);
#endif
