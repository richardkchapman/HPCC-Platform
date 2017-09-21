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
#include "jptree.hpp"
#include "eclrtl.hpp"
#include "eclhelper.hpp"
#include "rtlds_imp.hpp"
#include "eclhelper_base.hpp"
#include "eclhelper_dyn.hpp"

#include "rtlfield.hpp"
#include "rtlrecord.hpp"
#include "rtldynfield.hpp"
#include "rtlkey.hpp"

class AnotherClonedMemoryBufferBuilder : public RtlRowBuilderBase // Common up once merged!
{
public:
    AnotherClonedMemoryBufferBuilder(MemoryBuffer & _buffer, unsigned _minSize)
        : buffer(_buffer), minSize(_minSize)
    {
        reserved = 0;
    }

    virtual byte * ensureCapacity(size32_t required, const char * fieldName)
    {
        if (required > reserved)
        {
            void * next = buffer.reserve(required-reserved);
            self = (byte *)next - reserved;
            reserved = required;
        }
        return self;
    }

    void finishRow(size32_t length)
    {
        assertex(length <= reserved);
        size32_t newLength = (buffer.length() - reserved) + length;
        buffer.setLength(newLength);
        self = NULL;
        reserved = 0;
    }
    virtual IEngineRowAllocator *queryAllocator() const
    {
        return NULL;
    }

protected:
    virtual byte * createSelf()
    {
        return ensureCapacity(minSize, NULL);
    }

protected:
    MemoryBuffer & buffer;
    size32_t minSize;
    size32_t reserved;
};



//---------------------------------------------------------------------------
static const RtlRecordTypeInfo &loadTypeInfo(IPropertyTree &xgmml, IRtlFieldTypeDeserializer *deserializer, const char *key)
{
    StringBuffer xpath;
    MemoryBuffer binInfo;
    xgmml.getPropBin(xpath.setf("att[@name='%s_binary']/value", key), binInfo);
    assertex(binInfo.length());
    const RtlTypeInfo *typeInfo = deserializer->deserialize(binInfo);
    assertex(typeInfo && typeInfo->getType()==type_record);
    return *(RtlRecordTypeInfo *) typeInfo;
}

static void readString(StringBuffer &out, const char * &in)
{
    for (;;)
    {
        char c = *in++;
        if (!c)
            throw MakeStringException(0, "Invalid filter - missing closing '");
        if (c=='\'')
            break;
        if (c=='\\')
            UNIMPLEMENTED;
        out.append(c);
    }
}
class ECLRTL_API CDynamicDiskReadArg : public CThorDiskReadArg
{
public:
    CDynamicDiskReadArg(IPropertyTree &_xgmml) : xgmml(_xgmml)
    {
        indeserializer.setown(createRtlFieldTypeDeserializer());
        outdeserializer.setown(createRtlFieldTypeDeserializer());
        in.setown(new CDynamicOutputMetaData(loadTypeInfo(xgmml, indeserializer, "input")));
        out.setown(new CDynamicOutputMetaData(loadTypeInfo(xgmml, outdeserializer, "output")));
        inrec = &in->queryRecordAccessor(true);
        numOffsets = inrec->getNumVarFields() + 1;
        translator.setown(createRecordTranslator(queryOutputMeta()->queryRecordAccessor(true), *inrec));
        if (xgmml.hasProp("att[@name=\"keyfilter\"]"))
            flags |= TDRkeyed;
    }
    virtual bool needTransform() override
    {
        return true;
    }
    virtual unsigned getFlags() override
    {
        return flags;
    }
    virtual void createSegmentMonitors(IIndexReadContext *irc) override
    {
        size_t * variableOffsets = (size_t *)alloca(numOffsets * sizeof(size_t));
        RtlRow offsetCalculator(*inrec, nullptr, numOffsets, variableOffsets);
        Owned<IPropertyTreeIterator> filters = xgmml.getElements("att[@name=\"keyfilter\"]");
        ForEach(*filters)
        {
            // Format of a filter is:
            // field[..n]: valuestring
            // value string format specifies ranges using a comma-separated list of ranges.
            // Each range is specified as paren lower, upper paren, where the paren is either ( or [ depending
            // on whether the specified bound is inclusive or exclusive.
            // If only one bound is specified then it is used for both upper and lower bound (only meaningful with [] )
            //
            // ( A means values > A - exclusive
            // [ means values >= A - inclusive
            // A ) means values < A - exclusive
            // A ] means values <= A - inclusive
            // For example:
            // [A] matches just A
            // (,A),(A,) matches all but A
            // (A] of [A) are both empty ranges
            // [A,B) means A*
            // Values use the ECL syntax for constants. String constants are always utf8. Binary use d'xx' format (hexpairs)
            // Note that binary serialization format is different

            const char *curFilter = filters->query().queryProp("@value");
            assertex(curFilter);
            const char *epos = strchr(curFilter,'=');
            assertex(epos);
            StringBuffer fieldName(epos-curFilter, curFilter);
            unsigned fieldNum = inrec->getFieldNum(fieldName);
            assertex(fieldNum != (unsigned) -1);
            unsigned fieldOffset = offsetCalculator.getOffset(fieldNum);
            unsigned fieldSize = offsetCalculator.getSize(fieldNum);
            const RtlTypeInfo *fieldType = inrec->queryType(fieldNum);
            curFilter = epos+1;
            if (*curFilter=='~')
            {
                UNIMPLEMENTED;  // use a regex?
            }
            else
            {
                MemoryBuffer lobuffer;
                MemoryBuffer hibuffer;
                Owned<IStringSet> filterSet = createStringSet(fieldSize);
                while (*curFilter)
                {
                    char startRange = *curFilter++;
                    if (startRange != '(' && startRange != '[')
                        throw MakeStringException(0, "Invalid filter string: expected [ or ( at start of range");
                    // Now we expect a constant - type depends on type of field. Assume string or int for now
                    StringBuffer upperString, lowerString;
                    if (*curFilter=='\'')
                    {
                        curFilter++;
                        readString(lowerString, curFilter);
                    }
                    else
                        UNIMPLEMENTED; // lowerInt = readInt(curFilter);
                    if (*curFilter == ',')
                    {
                        curFilter++;
                        if (*curFilter=='\'')
                        {
                            curFilter++;
                            readString(upperString, curFilter);
                        }
                        else
                            UNIMPLEMENTED; //upperInt = readInt(curFilter);
                    }
                    else
                        upperString.set(lowerString);
                    char endRange = *curFilter++;
                    if (endRange != ')' && endRange != ']')
                        throw MakeStringException(0, "Invalid filter string: expected ] or ) at end of range");
                    if (*curFilter==',')
                        curFilter++;
                    else if (*curFilter)
                        throw MakeStringException(0, "Invalid filter string: expected , between ranges");
                    printf("Filtering: %s(%u,%u)=%c%s,%s%c\n", fieldName.str(), fieldOffset, fieldSize, startRange, lowerString.str(), upperString.str(), endRange);
                    AnotherClonedMemoryBufferBuilder lobuilder(lobuffer.clear(), inrec->getMinRecordSize());
                    fieldType->buildUtf8(lobuilder, 0, inrec->queryField(fieldNum), lowerString.length(), lowerString.str());

                    AnotherClonedMemoryBufferBuilder hibuilder(hibuffer.clear(), inrec->getMinRecordSize());
                    fieldType->buildUtf8(hibuilder, 0, inrec->queryField(fieldNum), upperString.length(), upperString.str());

                    filterSet->addRange(lobuffer.toByteArray(), hibuffer.toByteArray());
                    if (startRange=='(')
                        filterSet->killRange(lobuffer.toByteArray(), lobuffer.toByteArray());
                    if (endRange==')')
                        filterSet->killRange(hibuffer.toByteArray(), hibuffer.toByteArray());
                }
                StringBuffer str;
                filterSet->describe(str);
                printf("Using filterset %s", str.str());
                irc->append(createKeySegmentMonitor(false, filterSet.getClear(), fieldOffset, fieldSize));
            }
        }
    }

    virtual IOutputMetaData * queryOutputMeta() override
    {
        return out;
    }
    virtual const char * getFileName() override final
    {
        return xgmml.queryProp("att[@name=\"_fileName\"]/@value");
    }
    virtual IOutputMetaData * queryDiskRecordSize() override final
    {
        return in;
    }
    virtual unsigned getFormatCrc() override
    {
        return xgmml.getPropInt("att[@name=\"formatCrc\"]/@value", 0);  // engines should treat 0 as 'ignore'
    }
    virtual size32_t transform(ARowBuilder & rowBuilder, const void * src) override
    {
        return translator->translate(rowBuilder, (const byte *) src);
    }
private:
    IPropertyTree &xgmml;
    unsigned numOffsets = 0;
    unsigned flags = 0;
    Owned<IRtlFieldTypeDeserializer> indeserializer;   // Owns the resulting ITypeInfo structures, so needs to be kept around
    Owned<IRtlFieldTypeDeserializer> outdeserializer;  // Owns the resulting ITypeInfo structures, so needs to be kept around
    Owned<IOutputMetaData> in;
    Owned<IOutputMetaData> out;
    const RtlRecord *inrec = nullptr;
    Owned<const IDynamicTransform> translator;
};

class ECLRTL_API CDynamicWorkUnitWriteArg : public CThorWorkUnitWriteArg
{
public:
    CDynamicWorkUnitWriteArg(IPropertyTree &_xgmml) : xgmml(_xgmml)
    {
        indeserializer.setown(createRtlFieldTypeDeserializer());
        in.setown(new CDynamicOutputMetaData(loadTypeInfo(xgmml, indeserializer, "input")));
    }
    virtual int getSequence() override final { return 0; }
    virtual IOutputMetaData * queryOutputMeta() override final { return in; }
private:
    IPropertyTree &xgmml;
    Owned<IRtlFieldTypeDeserializer> indeserializer;   // Owns the resulting ITypeInfo structures, so needs to be kept around
    Owned<IOutputMetaData> in;
};

extern ECLRTL_API IHThorArg *createDiskReadArg(IPropertyTree &xgmml)
{
    return new CDynamicDiskReadArg(xgmml);
}

extern ECLRTL_API IHThorArg *createWorkunitWriteArg(IPropertyTree &xgmml)
{
    return new CDynamicWorkUnitWriteArg(xgmml);
}

struct ECLRTL_API DynamicEclProcess : public EclProcess {
    virtual unsigned getActivityVersion() const override { return ACTIVITY_INTERFACE_VERSION; }
    virtual int perform(IGlobalCodeContext * gctx, unsigned wfid) override {
        ICodeContext * ctx;
        ctx = gctx->queryCodeContext();
        ctx->executeGraph("graph1",false,0,NULL);
        return 1U;
    }
};

extern ECLRTL_API IEclProcess* createDynamicEclProcess()
{
    return new DynamicEclProcess;
}

