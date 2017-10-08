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
        //return translator->needsTranslate();
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
            const char *curFilter = filters->query().queryProp("@value");
            assertex(curFilter);
            // field = value is all I support for now
            const char *epos = strchr(curFilter,'=');
            assertex(epos);
            StringBuffer fieldName(epos-curFilter, curFilter);
            curFilter = epos+1;
            assertex (*curFilter == '\'');
            curFilter++;
            epos = strchr(curFilter, '\'');
            StringBuffer fieldVal(epos-curFilter, curFilter);
            unsigned fieldNum = inrec->getFieldNum(fieldName);
            assertex(fieldNum != -1);
            unsigned fieldOffset = offsetCalculator.getOffset(fieldNum);
            unsigned fieldSize = offsetCalculator.getSize(fieldNum);
            printf("Filtering: %s(%u,%u)=%s\n", fieldName.str(), fieldOffset, fieldSize, fieldVal.str());
            irc->append(createSingleKeySegmentMonitor(false, fieldOffset, fieldSize, fieldVal.str()));
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

