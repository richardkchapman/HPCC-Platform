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

#include "platform.h"
#include "v8.h"
#include "eclrtl.hpp"
#include "jexcept.hpp"
#include "jthread.hpp"

namespace javascriptLanguageHelper {

class V8JavascriptForeignContext : public CInterfaceOf<IForeignFunctionContext>
{
public:
    V8JavascriptForeignContext()
    {
        isolate = v8::Isolate::New();
        isolate->Enter();
        context = v8::Context::New();
        context->Enter();
    }
    ~V8JavascriptForeignContext()
    {
        script.Dispose();
        result.Dispose();
        context->Exit();
        context.Dispose();
        isolate->Exit();
        isolate->Dispose();
    }

    virtual void bindRealParam(const char *name, double val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Number::New(val));
    }
    virtual void bindSignedParam(const char *name, __int64 val)
    {
        // MORE - might need to check does not overflow 32 bits? Or store as a real?
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Integer::New(val));
    }
    virtual void bindUnsignedParam(const char *name, unsigned __int64 val)
    {
        // MORE - might need to check does not overflow 32 bits
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::Integer::NewFromUnsigned(val));
    }
    virtual void bindStringParam(const char *name, size32_t len, const char *val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::String::New(val, len));
    }
    virtual void bindVStringParam(const char *name, const char *val)
    {
        v8::HandleScope handle_scope;
        context->Global()->Set(v8::String::New(name), v8::String::New(val));
    }

    virtual double getRealResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Number::Cast(*result)->Value();
    }
    virtual __int64 getSignedResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Integer::Cast(*result)->Value();
    }
    virtual unsigned __int64 getUnsignedResult()
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;
        return v8::Integer::Cast(*result)->Value();
    }
    virtual void getStringResult(size32_t &__len, char * &__result)
    {
        assertex (!result.IsEmpty());
        v8::HandleScope handle_scope;   // May not strictly be needed?
        v8::String::AsciiValue ascii(result);
        const char *chars= *ascii;
        __len = strlen(chars);
        __result = (char *)rtlMalloc(__len);
        memcpy(__result, chars, __len);
    }

    virtual void compileEmbeddedScript(const char *text)
    {
        v8::HandleScope handle_scope;
        v8::Handle<v8::String> source = v8::String::New(text);
        v8::Handle<v8::Script> lscript = v8::Script::Compile(source);
        script = v8::Persistent<v8::Script>::New(lscript);
    }
    virtual void callFunction()
    {
        assertex (!script.IsEmpty());
        v8::HandleScope handle_scope;
        result = v8::Persistent<v8::Value>::New(script->Run());
    }

protected:
    v8::Isolate *isolate;
    v8::Persistent<v8::Context> context;
    v8::Persistent<v8::Script> script;
    v8::Persistent<v8::Value> result;
};

__thread V8JavascriptForeignContext * theContext;
__thread ThreadTermFunc threadHookChain;

static void releaseContext()
{
    ::Release(theContext);
    if (threadHookChain)
        (*threadHookChain)();
}

class V8JavascriptForeignPlugin : public CInterfaceOf<IForeignPlugin>
{
public:
    V8JavascriptForeignPlugin()
    {
        Link();  // Deliberately 'leak' in order to avoid freeing this global object
    }
    virtual IForeignFunctionContext *createCallContext()
    {
        if (!theContext)
        {
            theContext = new V8JavascriptForeignContext;
            threadHookChain = addThreadTermFunc(releaseContext);
        }
        return LINK(theContext);
    }
} thePlugin;


extern IForeignPlugin* getPlugin()
{
    return LINK(&thePlugin);
}

} // namespace
