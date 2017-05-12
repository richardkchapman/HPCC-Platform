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

#ifdef _USE_CPPUNIT
#include "unittests.hpp"
#include "jstats.h"
#include "jregexp.hpp"
#include "jfile.hpp"
#include "deftype.hpp"
#include "rmtfile.hpp"

/*
 * This is the main unittest driver for HPCC. From here,
 * all unit tests, be they internal or external (API),
 * will run.
 *
 * All internal unit tests, written on the same source
 * files as the implementation they're testing, can be
 * dynamically linked via the helper class below.
 *
 * All external unit tests (API tests, test-driven
 * development, interface documentation and general
 * usability tests) should be implemented as source
 * files within the same directory as this file, and
 * statically linked together.
 *
 * CPPUnit will automatically recognise and run them all.
 */

void usage()
{
    printf("\n"
    "Usage:\n"
    "    unittests <options> <testnames>\n"
    "\n"
    "Options:\n"
    "    -a  --all        Include all tests, including timing and stress tests\n"
    "    -d  --load path  Dynamically load a library/all libraries in a directory.\n"
    "                     By default, the HPCCSystems lib directory is loaded.\n"
    "    -e  --exact      Match subsequent test names exactly\n"
    "    -h  --help       Display this help text\n"
    "    -l  --list       List matching tests but do not execute them\n"
    "    -x  --exclude    Exclude subsequent test names\n"
    "\n");
}

bool matchName(const char *name, const StringArray &patterns)
{
    ForEachItemIn(idx, patterns)
    {
        bool match;
        const char *pattern = patterns.item(idx);
        if (strchr(pattern, '*'))
        {
            match = WildMatch(name, pattern, true);
        }
        else
            match = streq(name, pattern);
        if (match)
            return true;
    }
    return false;
}

LoadedObject *loadDll(const char *thisDll)
{
    try
    {
        DBGLOG("Loading %s", thisDll);
        return new LoadedObject(thisDll);
    }
    catch (IException *E)
    {
        E->Release();
    }
    catch (...)
    {
    }
    return NULL;
}

void loadDlls(IArray &objects, const char * libDirectory)
{
    const char * mask = "*" SharedObjectExtension;
    Owned<IFile> libDir = createIFile(libDirectory);
    Owned<IDirectoryIterator> libFiles = libDir->directoryFiles(mask,false,false);
    ForEach(*libFiles)
    {
        const char *thisDll = libFiles->query().queryFilename();
        if (!strstr(thisDll, "javaembed"))  // Bit of a hack, but loading this if java not present terminates...
            if (!strstr(thisDll, "pyembed"))      // These two clash, so ...
                if (!strstr(thisDll, "py3embed"))  // ... best to load neither...
                {
                    LoadedObject *loaded = loadDll(thisDll);
                    if (loaded)
                        objects.append(*loaded);
                }
    }
}

int main(int argc, char* argv[])
{
    InitModuleObjects();

    StringArray includeNames;
    StringArray excludeNames;
    StringArray loadLocations;
    bool wildMatch = true;
    bool exclude = false;
    bool includeAll = false;
    bool verbose = false;
    bool list = false;
    bool useDefaultLocations = true;
    for (int argNo = 1; argNo < argc; argNo++)
    {
        const char *arg = argv[argNo];
        if (arg[0]=='-')
        {
            if (streq(arg, "-x") || streq(arg, "--exclude"))
                exclude = true;
            else if (streq(arg, "-v") || streq(arg, "--verbose"))
                verbose = true;
            else if (streq(arg, "-e") || streq(arg, "--exact"))
                wildMatch = false;
            else if (streq(arg, "-a") || streq(arg, "--all"))
                includeAll = true;
            else if (streq(arg, "-l") || streq(arg, "--list"))
                list = true;
            else if (streq(arg, "-d") || streq(arg, "--load"))
            {
                useDefaultLocations = false;
                argNo++;
                if (argNo<argc)
                   loadLocations.append(argv[argNo]);
            }
            else
            {
                usage();
                exit(streq(arg, "-h") || streq(arg, "--help")?0:4);
            }
        }
        else
        {
            VStringBuffer pattern("*%s*", arg);
            if (wildMatch && !strchr(arg, '*'))
                arg = pattern.str();
            if (exclude)
                excludeNames.append(arg);
            else
                includeNames.append(arg);
        }
    }
    if (verbose)
        queryStderrLogMsgHandler()->setMessageFields(MSGFIELD_time);
    else
        removeLog();

    if (!includeNames.length())
        includeNames.append("*");

    if (!includeAll)
    {
        excludeNames.append("*stress*");
        excludeNames.append("*timing*");
    }

    if (useDefaultLocations)
    {
        // Default library location depends on the executable location...
        StringBuffer dir;
        splitFilename(argv[0], &dir, &dir, NULL, NULL);
        dir.replaceString(PATHSEPSTR "bin" PATHSEPSTR, PATHSEPSTR "lib" PATHSEPSTR);
        if (verbose)
            DBGLOG("Adding default library location %s", dir.str());
        loadLocations.append(dir);
#ifdef _DEBUG
        dir.replaceString(PATHSEPSTR "lib" PATHSEPSTR, PATHSEPSTR "libs" PATHSEPSTR);
        loadLocations.append(dir);
        if (verbose)
            DBGLOG("Adding default library location %s", dir.str());
#endif
    }
    IArray objects;
    ForEachItemIn(idx, loadLocations)
    {
        const char *location = loadLocations.item(idx);
        Owned<IFile> file = createIFile(location);
        switch (file->isDirectory())
        {
        case notFound:
            if (verbose && !useDefaultLocations)
                DBGLOG("Specified library location %s not found", location);
            break;
        case foundYes:
            loadDlls(objects, location);
            break;
        case foundNo:
            LoadedObject *loaded = loadDll(location);
            if (loaded)
                objects.append(*loaded);
            break;
        }
    }

    bool wasSuccessful = false;
    {
        // New scope as we need the TestRunner to be destroyed before unloading the dlls...
        CppUnit::TestFactoryRegistry &registry = CppUnit::TestFactoryRegistry::getRegistry();
        CppUnit::TextUi::TestRunner runner;
        CppUnit::Test *all = registry.makeTest();
        int numTests = all->getChildTestCount();
        for (int i = 0; i < numTests; i++)
        {
            CppUnit::Test *sub = all->getChildTestAt(i);
            std::string name = sub->getName();
            if (matchName(name.c_str(), includeNames))
            {
                if (matchName(name.c_str(), excludeNames))
                {
                    if (verbose)
                        DBGLOG("Excluding test %s", name.c_str());
                }
                else if (list)
                    printf("%s\n", name.c_str());
                else
                {
                    if (verbose)
                        DBGLOG("Including test %s", name.c_str());
                    runner.addTest(sub);
                }
            }
        }
        wasSuccessful = list || runner.run( "", false );
    }
    releaseAtoms();
    ClearTypeCache();   // Clear this cache before the file hooks are unloaded
    removeFileHooks();

    objects.kill();
    ExitModuleObjects();
    return wasSuccessful;
}


//MORE: This can't be included in jlib because of the dll dependency
class InternalStatisticsTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE( InternalStatisticsTest  );
        CPPUNIT_TEST(testMappings);
    CPPUNIT_TEST_SUITE_END();

    void testMappings()
    {
        try
        {
            verifyStatisticFunctions();
        }
        catch (IException * e)
        {
            StringBuffer msg;
            fprintf(stderr, "Failure: %s", e->errorMessage(msg).str());
            e->Release();
            ASSERT(false);
        }
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( InternalStatisticsTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( InternalStatisticsTest, "StatisticsTest" );

class CSharedStringTable
{
protected:
    unsigned htn = 0;
    unsigned n = 0;
    const char **table = nullptr;
    const char *shared_start = nullptr;
    const char *shared_end = nullptr;

    static unsigned hash(const char *key)
    {
        return hashc((const unsigned char *) key, strlen(key), 0);
    }

    void expand()
    {
        const char **t = table+htn; // more interesting going backwards
        htn = htn*2+1;
        const char **newtable = (const char **)calloc(sizeof(const char *),htn);
        while (t--!=table)
        {
            const char *c = *t;
            if (c)
            {
                unsigned h = hash(c) % htn;
                while (newtable[h])
                {
                    h++;
                    if (h==htn)
                        h = 0;
                }
                newtable[h] = c;
            }
        }
        ::free(table);
        table = newtable;
    }

    void add(const char *c)
    {
        if (n*4>htn*3)
            expand();
        unsigned i = hash(c) % htn;
        while (table[i])
            if (++i==htn)
                i = 0;
        table[i] = c;
        n++;
    }

public:
    CSharedStringTable(const char *init)
    {
        htn = 7;
        n = 0;
        table = (const char **)calloc(sizeof(const char *), htn);
        shared_start = init;
        while (*init)
        {
            add(init);
            init += strlen(init)+1;
        }
        shared_end = init;
    }

    ~CSharedStringTable()
    {
        ::free(table);
    }

    const char *strdup(const char *key)
    {
        unsigned i = hash(key) % htn;
        while (table[i])
        {
            if (streq(table[i], key))
                return table[i];
            if (++i==htn)
                i = 0;
        }
        return ::strdup(key);
    }

    void free(const char *str)
    {
        if (!is_shared_string(str))
            ::free((char *) str);
    }

    inline bool is_shared_string(const char *str)
    {
        return str >= shared_start && str < shared_end;
    }
};

class SharedStringTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE( SharedStringTest  );
        CPPUNIT_TEST(testSimple);
    CPPUNIT_TEST_SUITE_END();

    void testSimple()
    {
        const char *initstr = "A\0B\0C\0";
        CSharedStringTable table(initstr);
        const char * A = table.strdup("A");
        ASSERT(streq(A, "A"));
        ASSERT(A==initstr);
        ASSERT(table.is_shared_string(A));
        table.free(A);
        const char * C = table.strdup("C");
        ASSERT(streq(C, "C"));
        ASSERT(table.is_shared_string(C));
        table.free(C);
        const char * D = table.strdup("D");
        ASSERT(streq(D, "D"));
        ASSERT(!table.is_shared_string(D));
        table.free(D);
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( SharedStringTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( SharedStringTest, "SharedStringTest" );

//MORE: This can't be included in jlib because of the dll dependency
class StringBufferTest : public CppUnit::TestFixture
{
    CPPUNIT_TEST_SUITE( StringBufferTest  );
        CPPUNIT_TEST(testReplace);
    CPPUNIT_TEST_SUITE_END();

    void testReplace()
    {
        StringBuffer r ("1 bb c");
        r.replaceString(" ", "x");
        ASSERT(streq(r, "1xbbxc"));
    }
};

CPPUNIT_TEST_SUITE_REGISTRATION( StringBufferTest );
CPPUNIT_TEST_SUITE_NAMED_REGISTRATION( StringBufferTest, "StringBufferTest" );


#endif // _USE_CPPUNIT
