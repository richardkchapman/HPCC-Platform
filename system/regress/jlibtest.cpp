/*##############################################################################

    Copyright (C) 2012 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

/**
 * This is a regression test for all jlib libraries. So far, it only
 * tests juri (the URI parser), but should add tests to other core libraries.
 */

#include "juri.hpp"
#include <iostream>

using namespace std;

static unsigned errors;
static unsigned tests;

// =============================================================== jURI - URI parser
void appendSchemeStr(URISchemeType scheme, StringBuffer& buf)
{
    switch(scheme)
    {
    case URIScheme_hpcc:
        buf.append("HPCC");
    case URIScheme_file:
        buf.append("FILE");
    default:
        buf.append("ERROR");
    }
}

void appendServerStr(const URIServerDescription * server, StringBuffer& buf)
{
    if (!server->user.isEmpty())
        buf.append(server->user.get()).append("@");
    buf.append(server->host.get());
    if (server->port)
        buf.append(":").append(server->port);
}

void appendPathStr(const URIPathDescription * path, StringBuffer& buf)
{
    buf.append(path->path.get());
    switch(path->type)
    {
    case URIFile_super:
        buf.append("?super");
        break;
    case URIFile_stream:
        buf.append("?stream");
        break;
    }
    if (path->index)
        buf.append("#").append(path->index);
    else if (path->subname.length())
        buf.append("#").append(path->subname.get());
}

/*
 * Parameters:
 *
 *     uri : URI to be tested
 *   isURI : if the string is to be recognised
 *  scheme : expected scheme
 *  server : expected server string
 *    path : expected path string
 */
void test_uri(const char * str, bool shouldBeURI, URISchemeType scheme=URIScheme_error, const char * server=NULL, const char * path=NULL)
{
    tests++;
    bool isURI = URI::isURI(str);

    // Bad parsing of URI (should never happen, this rely on uriparse library)
    if (!isURI)
    {
        if (shouldBeURI)
        {
            cout << "String: [" << str << "] should be an URI, but was not recognised as so" << endl;
            errors++;
        }
        return;
    }
    else if (!shouldBeURI)
    {
        cout << "String: [" << str << "] should not be an URI, but was recognised as so" << endl;
        errors++;
        return;
    }

    // Now, validate URI
    try
    {
        URI res(str);

        StringBuffer response;
        if (res.getScheme() != scheme)
        {
            StringBuffer request;
            appendSchemeStr(scheme, request);
            appendSchemeStr(res.getScheme(), response);
            cout << "Scheme: requested '" << request.str() << "', got '" << response.str() << "'" << endl;
            errors++;
            return;
        }
        if (scheme == URIScheme_error)
            return;        // No need to validate the rest

        appendServerStr(res.getServer(), response);
        if (strcmp(response.str(), server) != 0)
        {
            cout << "Server: requested '" << server << "', got '" << response.str() << "'" << endl;
            errors++;
            return;
        }
        response.clear();
        appendPathStr(res.getPath(), response);
        if (strcmp(response.str(), path) != 0)
        {
            cout << "Path: requested '" << path << "', got '" << response.str() << "'" << endl;
            errors++;
            return;
        }
    }
    catch (IException *e)
    {
        StringBuffer buf;
        cout << e->errorMessage(buf).str() << endl;
        e->Release();
        errors++;
        return;
    }
}

int main() {
    tests = 0;
    errors = 0;

    // URL
    test_uri("http://www.hpccsystems.com/", true);

    // Local files
    test_uri("file:///opt/HPCCSystems/examples/IMDB/ActorsInMovies.ecl", true, URIScheme_file, "", "/opt/HPCCSystems/examples/IMDB/ActorsInMovies.ecl");

    // Dali files
    test_uri("hpcc://mydali/path/to/file", true, URIScheme_hpcc, "mydali", "path/to/file");
    test_uri("hpcc://mydali/path/to/superfile?super", true, URIScheme_hpcc, "mydali", "path/to/superfile?super");
    test_uri("hpcc://mydali/path/to/superfile?super#subname", true, URIScheme_hpcc, "mydali", "path/to/superfile?super#subname");
    test_uri("hpcc://mydali/path/to/streamfile?stream", true, URIScheme_hpcc, "mydali", "path/to/streamfile?stream");
    test_uri("hpcc://mydali/path/to/streamfile?stream#047", true, URIScheme_hpcc, "mydali", "path/to/streamfile?stream#47");

    // Variations in Dali location
    test_uri("hpcc://mydali:7070/path/to/file", true, URIScheme_hpcc, "mydali:7070", "path/to/file");
    test_uri("hpcc://user@mydali:7070/path/to/file", true, URIScheme_hpcc, "user@mydali:7070", "path/to/file");
    test_uri("hpcc://user@mydali/path/to/file", true, URIScheme_hpcc, "user@mydali", "path/to/file");
    test_uri("hpcc://user:passwd@mydali:7070/path/to/file", true, URIScheme_hpcc, "user:passwd@mydali:7070", "path/to/file");
    test_uri("hpcc://user:passwd@mydali/path/to/file", true, URIScheme_hpcc, "user:passwd@mydali", "path/to/file");

    if (errors)
        cout << endl << "Tests failed: " << errors << " / " << tests << endl;
    else
        cout << endl << "All tests pass" << endl;
    return errors;
}
