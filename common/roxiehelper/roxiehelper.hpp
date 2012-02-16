/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

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

#ifndef ROXIEHELPER_HPP
#define ROXIEHELPER_HPP

#include "thorxmlwrite.hpp"
#include "roxiehelper.ipp"
#include "roxiemem.hpp"
#include "mpbase.hpp"
#include "workunit.hpp"
#include "jisem.hpp"
#include "jqueue.tpp"
#include "thorcommon.ipp"
#include "rtlread_imp.hpp"

#ifdef _WIN32
 #ifdef ROXIEHELPER_EXPORTS
  #define ROXIEHELPER_API __declspec(dllexport)
 #else
  #define ROXIEHELPER_API __declspec(dllimport)
 #endif
#else
 #define ROXIEHELPER_API
#endif

//#pragma message("**** ROXIEHELPER.HPP ***")

//========================================================================================= 

class ROXIEHELPER_API HttpHelper : public CInterface
{
private:
    bool _isHttp;
    StringBuffer authToken;
public:
    IMPLEMENT_IINTERFACE;
    HttpHelper() { _isHttp = false; };
    bool isHttp() { return _isHttp; };
    void setIsHttp(bool __isHttp) { _isHttp = __isHttp; };
    const char *queryAuthToken() { return authToken.str(); };
    void setAuthToken(const char *_authToken) { authToken.clear().append(_authToken); };
};

//========================================================================================= 

interface SafeSocket : extends IInterface
{
    virtual ISocket *querySocket()  = 0;
    virtual size32_t write(const void *buf, size32_t size, bool takeOwnership=false) = 0;
    virtual bool readBlock(MemoryBuffer &ret, unsigned maxBlockSize, unsigned timeout = (unsigned) WAIT_FOREVER) = 0;
    virtual bool readBlock(StringBuffer &ret, unsigned timeout, HttpHelper *pHttpHelper, bool &, bool &, unsigned maxBlockSize) = 0;
    virtual void setHttpMode(const char *queryName, bool arrayMode) = 0;
    virtual void setHeartBeat() = 0;
    virtual bool sendHeartBeat(const IContextLogger &logctx) = 0;
    virtual void flush() = 0;
    virtual unsigned bytesOut() const = 0;
    virtual bool checkConnection() const = 0;
    virtual void sendException(const char *source, unsigned code, const char *message, bool isBlocked, const IContextLogger &logctx) = 0;

    // TO be removed and replaced with better mechanism when SafeSocket merged with tht new output sequencer...
    // until then you may need to lock using this if you are making multiple calls and they need to stay together in the output
    virtual CriticalSection &queryCrit() = 0;
};

class ROXIEHELPER_API CSafeSocket : public CInterface, implements SafeSocket
{
protected:
    Linked<ISocket> sock;
    bool httpMode;
    bool heartbeat;
    StringBuffer xmlhead;
    StringBuffer xmltail;
    PointerArray queued;
    UnsignedArray lengths;
    unsigned sent;
    CriticalSection crit;

public:
    IMPLEMENT_IINTERFACE;
    CSafeSocket(ISocket *_sock);
    ~CSafeSocket();
    
    ISocket *querySocket() { return sock; }
    virtual CriticalSection &queryCrit() { return crit; };
    size32_t write(const void *buf, size32_t size, bool takeOwnership=false);
    bool readBlock(MemoryBuffer &ret, unsigned maxBlockSize, unsigned timeout = (unsigned) WAIT_FOREVER);
    bool readBlock(StringBuffer &ret, unsigned timeout, HttpHelper *pHttpHelper, bool &, bool &, unsigned maxBlockSize);
    void setHttpMode(const char *queryName, bool arrayMode);
    void setHeartBeat();
    bool sendHeartBeat(const IContextLogger &logctx);
    void flush();
    unsigned bytesOut() const;
    bool checkConnection() const;
    void sendException(const char *source, unsigned code, const char *message, bool isBlocked, const IContextLogger &logctx);
};

//==============================================================================================================
class ROXIEHELPER_API FlushingStringBuffer : extends CInterface, implements IXmlStreamFlusher, implements IInterface
{
    // MORE this code is yukky. Overdue for cleanup!

    SafeSocket *sock;
    StringBuffer name;
    StringBuffer tail;
    unsigned sequenceNumber;
    unsigned rowCount;
    unsigned emptyLength;
    const IContextLogger &logctx;
    CriticalSection crit;
    PointerArray queued;
    UnsignedArray lengths;

    bool needsFlush(bool closing);
    void startBlock();
public:
    bool isXml;      // controls whether xml elements are output
    bool isRaw;      // controls whether output as binary or ascii
    bool isBlocked;
    bool isHttp;
    bool isSoap;
    bool isEmpty;
    bool extend;
    bool trim;
    bool tagClosed;
    StringAttr queryName;
    StringBuffer s;

    IMPLEMENT_IINTERFACE;

    FlushingStringBuffer(SafeSocket *_sock, bool _isBlocked, bool _isXml, bool _isRaw, bool _isHttp, const IContextLogger &_logctx);
    ~FlushingStringBuffer();
    void append(char data) {append(1, &data);}
    void append(const char *data);
    void append(unsigned len, const char *data);
    void appendf(const char *format, ...) __attribute__((format(printf, 2, 3)));
    void encodeXML(const char *x, unsigned flags=0, unsigned len=(unsigned)-1, bool utf8=false);
    virtual void flushXML(StringBuffer &current, bool isClosing);
    void flush(bool closing) ;
    void *getPayload(size32_t &length);
    void startDataset(const char *elementName, const char *resultName, unsigned sequence, bool _extend = false);
    void startScalar(const char *resultName, unsigned sequence);
    void incrementRowCount();
};

//==============================================================================================================

class ROXIEHELPER_API OwnedRowArray
{
public:
    OwnedRowArray() {}
    ~OwnedRowArray() { clear(); }


    void clear();
    void clearPart(aindex_t from, aindex_t to);
    void replace(const void * row, aindex_t pos);
    void append(const void * row) { buff.append(row); }
    aindex_t ordinality() const { return buff.ordinality(); }
    const void * * getArray() { return buff.getArray(); }
    bool isItem(aindex_t pos) const { return buff.isItem(pos); }
    const void * item(aindex_t pos) { return buff.item(pos); }
    const void * itemClear(aindex_t pos) { const void * ret = buff.item(pos); buff.replace(NULL, pos); return ret; }

protected:
    ConstPointerArray buff;
};

//==============================================================================================================

interface IFileDescriptor;
interface IAgentContext;
class ROXIEHELPER_API ClusterWriteHandler : public CInterface
{
public:
    ClusterWriteHandler(char const * _logicalName, char const * _activityType);
    void addCluster(char const * cluster);
    void getLocalPhysicalFilename(StringAttr & out) const;
    void splitPhysicalFilename(StringBuffer & dir, StringBuffer & base) const;
    void copyPhysical(IFile * source, bool noCopy) const;
    void setDescriptorParts(IFileDescriptor * desc, char const * basename, IPropertyTree * attrs) const;
    void finish(IFile * file) const;
    void getClusters(StringArray &clusters) const;

private:
    virtual void getTempFilename(StringAttr & out) const ;

private:
    StringAttr logicalName;
    StringBuffer physicalName;
    StringBuffer physicalDir;
    StringBuffer physicalBase;
    StringAttr activityType;
    StringAttr localClusterName;
    Owned<IGroup> localCluster; 
    IArrayOf<IGroup> remoteNodes;
    StringArray remoteClusters;
};

//==============================================================================================================

ROXIEHELPER_API void serializeRoxieRow(const void *row, MemoryBuffer &buf, IOutputRowSerializer *serializer);
ROXIEHELPER_API const void *deserializeRoxieRow(MemoryBuffer &buf, IOutputRowDeserializer *rowDeserializer, IEngineRowAllocator *rowAllocator);

class ROXIEHELPER_API RestartableQueue : public QueueOf<const void, true>
{
public:
    void serializeRestartInfo(MemoryBuffer &buf, IOutputRowSerializer *serializer) const;
    void deserializeRestartInfo(MemoryBuffer &buf, IOutputRowDeserializer *rowDeserializer, IEngineRowAllocator *rowAllocator);
};

//==============================================================================================================

/*
 * By registering all member variables of an activity using this class, they can be easily reset to 0
 * per child-query, and serialized/deserialized for checkpoint recovery purposes.
 */

class ROXIEHELPER_API CRoxieStateVariables
{
    class StateVariableHelper : public CInterface
    {
    public:
        IMPLEMENT_IINTERFACE;
        virtual void serialize(MemoryBuffer &to) const = 0;
        virtual void deserialize(MemoryBuffer &from) = 0;
    };

    template <class TYPE> class StateVariableSubHelper : public StateVariableHelper
    {
    protected:
        TYPE &var;
    public:
        StateVariableSubHelper(TYPE &_var) : var(_var) {}
        virtual void serialize(MemoryBuffer &to) const { to.append(var); }
        virtual void deserialize(MemoryBuffer &from) { from.read(var); }
    };

    class StateVariableOwnedIExceptionHelper : public StateVariableHelper
    {
    protected:
        Owned<IException> &var;
    public:
        StateVariableOwnedIExceptionHelper(Owned<IException> &_var) : var(_var) {}
        virtual void serialize(MemoryBuffer &to) const
        {
            if (var)
            {
                bool nonNull = true;
                int errAudience = (int) var->errorAudience();
                int errCode = var->errorCode();
                StringBuffer msg;
                var->errorMessage(msg);
                to.append(nonNull).append(errAudience).append(errCode).append(msg);
            }
            else
            {
                bool nonNull = false;
                to.append(nonNull);
            }
        }
        virtual void deserialize(MemoryBuffer &from)
        {
            bool nonNull;
            from.read(nonNull);
            if (nonNull)
            {
                int errAudience;
                int errCode;
                StringAttr errMsg;
                from.read(errAudience);
                from.read(errCode);
                from.read(errMsg);
                var.setown(MakeStringException((MessageAudience) errAudience, errCode, "%s", errMsg.get()));
            }
            else
                var.clear();
        }
    };

    class NamedStateVariableOwnedIExceptionHelper : public StateVariableOwnedIExceptionHelper
    {
        unsigned activityId;
        StringAttr name;
    public:
        NamedStateVariableOwnedIExceptionHelper(Owned<IException> &_var, unsigned _activityId, const char *_name)
          : StateVariableOwnedIExceptionHelper(_var), activityId(_activityId), name(_name)
        {}
        virtual void serialize(MemoryBuffer &to) const
        {
            if (traceLevel>0)
            {
                if (var)
                {
                    StringBuffer s;
                    var->errorMessage(s);
                    DBGLOG("Activity %d serializing %s value %d:%s", activityId, name.get(), var->errorCode(), s.str());
                }
                else
                    DBGLOG("Activity %d serializing %s value NULL", activityId, name.get());
            }
            StateVariableOwnedIExceptionHelper::serialize(to);
        }
        virtual void deserialize(MemoryBuffer &from)
        {
            StateVariableOwnedIExceptionHelper::deserialize(from);
            if (traceLevel>0)
            {
                if (var)
                {
                    StringBuffer s;
                    var->errorMessage(s);
                    DBGLOG("Activity %d deserialized %s value %d:%s", activityId, name.get(), var->errorCode(), s.str());
                }
                else
                    DBGLOG("Activity %d deserialized %s value NULL", activityId, name.get());
            }
        }
    };

    class StateVariableInterruptableSemaphoreHelper : public StateVariableHelper
    {
    protected:
        InterruptableSemaphore &var;
    public:
        StateVariableInterruptableSemaphoreHelper(InterruptableSemaphore &_var) : var(_var) {}
        virtual void serialize(MemoryBuffer &to) const
        {
            // MORE - and the exception...
            unsigned count = var.queryCount();
            to.append(count);
        }
        virtual void deserialize(MemoryBuffer &from)
        {
            // MORE - and the exception...
            unsigned count;
            from.read(count);
            while(count--)
                var.signal();
        }
    };

    class NamedStateVariableInterruptableSemaphoreHelper : public StateVariableInterruptableSemaphoreHelper
    {
        unsigned activityId;
        StringAttr name;
    public:
        NamedStateVariableInterruptableSemaphoreHelper(InterruptableSemaphore &_var, unsigned _activityId, const char *_name)
          : StateVariableInterruptableSemaphoreHelper(_var), activityId(_activityId), name(_name)
        {}
        virtual void serialize(MemoryBuffer &to) const
        {
            if (traceLevel>0)
                DBGLOG("Activity %d serializing InterruptableSemaphore %s", activityId, name.get());
            StateVariableInterruptableSemaphoreHelper::serialize(to);
        }
        virtual void deserialize(MemoryBuffer &from)
        {
            StateVariableInterruptableSemaphoreHelper::deserialize(from);
            if (traceLevel>0)
                DBGLOG("Activity %d deserialized InterruptableSemaphore %s", activityId, name.get());
        }
    };

    template <class TYPE> class NamedStateVariableSubHelper : public StateVariableSubHelper<TYPE>
    {
        unsigned activityId;
        StringAttr name;
    public:
        NamedStateVariableSubHelper(TYPE &_var, unsigned _activityId, const char *_name)
          : StateVariableSubHelper<TYPE>(_var), activityId(_activityId), name(_name)
        {}
        virtual void serialize(MemoryBuffer &to) const
        {
            if (traceLevel>0)
            {
                StringBuffer s;
                s.append(StateVariableSubHelper<TYPE>::var);
                DBGLOG("Activity %d serializing %s value %s", activityId, name.get(), s.str());
            }
            StateVariableSubHelper<TYPE>::serialize(to);
        }
        virtual void deserialize(MemoryBuffer &from)
        {
            StateVariableSubHelper<TYPE>::deserialize(from);
            if (traceLevel>0)
            {
                StringBuffer s;
                s.append(StateVariableSubHelper<TYPE>::var);
                DBGLOG("Activity %d deserialized %s value %s", activityId, name.get(), s.str());
            }
        }
    };

    CIArrayOf<StateVariableHelper> registered;
    void _register(StateVariableHelper &helper)
    {
        registered.append(helper);
    }
public:
    void registerVariable(bool &var)      { _register(*new StateVariableSubHelper<bool>(var)); }
    void registerVariable(byte &var)      { _register(*new StateVariableSubHelper<byte>(var)); }
    void registerVariable(unsigned &var)  { _register(*new StateVariableSubHelper<unsigned>(var)); }
    void registerVariable(__uint64 &var)  { _register(*new StateVariableSubHelper<__uint64>(var)); }
    void registerVariable(Owned<IException> &var)  { _register(*new StateVariableOwnedIExceptionHelper(var)); }
    void registerVariable(InterruptableSemaphore &var)  { _register(*new StateVariableInterruptableSemaphoreHelper(var)); }

    void registerNamedVariable(bool &var, unsigned id, const char *name)      { _register(*new NamedStateVariableSubHelper<bool>(var, id, name)); }
    void registerNamedVariable(byte &var, unsigned id, const char *name)      { _register(*new NamedStateVariableSubHelper<byte>(var, id, name)); }
    void registerNamedVariable(unsigned &var, unsigned id, const char *name)  { _register(*new NamedStateVariableSubHelper<unsigned>(var, id, name)); }
    void registerNamedVariable(__uint64 &var, unsigned id, const char *name)  { _register(*new NamedStateVariableSubHelper<__uint64>(var, id, name)); }
    void registerNamedVariable(Owned<IException> &var, unsigned id, const char *name)  { _register(*new NamedStateVariableOwnedIExceptionHelper(var, id, name)); }
    void registerNamedVariable(InterruptableSemaphore &var, unsigned id, const char *name)  { _register(*new NamedStateVariableInterruptableSemaphoreHelper(var, id, name)); }

    void serialize(MemoryBuffer &to) const
    {
        ForEachItemIn(idx, registered)
        {
            registered.item(idx).serialize(to);
        }
    }
    void deserialize(MemoryBuffer &from)
    {
        ForEachItemIn(idx, registered)
        {
            registered.item(idx).deserialize(from);
        }
    }
};

//==============================================================================================================

ROXIEHELPER_API StringBuffer & mangleHelperFileName(StringBuffer & out, const char * in, const char * wuid, unsigned int flags);
ROXIEHELPER_API StringBuffer & mangleLocalTempFilename(StringBuffer & out, char const * in);
ROXIEHELPER_API StringBuffer & expandLogicalFilename(StringBuffer & logicalName, const char * fname, IConstWorkUnit * wu, bool resolveLocally);

ROXIEHELPER_API IActivityRestartContext *loadRestartContext(const IPropertyTree *serialized);
ROXIEHELPER_API IActivityRestartContext *createRestartContext();
#endif // ROXIEHELPER_HPP
