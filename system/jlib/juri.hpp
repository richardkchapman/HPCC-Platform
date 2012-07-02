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

#ifndef __JURI__
#define __JURI__

#include <uriparser/Uri.h>
#include "jlib.hpp"
#include "jexcept.hpp"

// MORE - This is just a stub, structs below need to be commoned up with existing definitions elsewhere

// Supported URI schemes
enum URISchemeType
{
    URIScheme_error,
    URIScheme_hpcc,
    URIScheme_file
};

// Supported server types
enum URIServerType
{
    URIServer_local,  // Local file
    URIServer_dali,   // Names resolved by Dali
    URIServer_host    // Names resolved by DNS
};

// Supported file types
enum URIFileType
{
    URIFile_local,    // Local files
    URIFile_logic,    // Normal files
    URIFile_super,    // Super files
    URIFile_stream    // Stream files (to be implemented)
};

struct URIServerDescription
{
    StringAttr user;
    StringAttr host;
    unsigned port;
};

struct URIPathDescription
{
    StringAttr path;
    URIFileType type;
    StringAttr subname;  // Super files' sub
    unsigned index;      // Stream files
};

// ==================================================================================
/*
 * URIFileResolver is the interface that any resolver should implement to be used
 * by the URIResolution scheme, to provide a seamless interface to any HPCC engine
 * to handle files in a plethora of environments.
 *
 * This has not be thought about properly and does not concern
 * much of the initial URI investigations.
 */
//interface URIFileResolver
//{
//    // Returns a Read-only descriptor of a file. No Dali locks.
//    virtual IFileDescriptor *getFileDescriptor(StringAttr &filePath) = 0;
//    // Returns a distributed dali / local file
//    virtual IResolvedFile *getFile(StringAttr &filePath) = 0;
//    // Returns a distributed dali / local file form a pre-existing descriptor
//    virtual IResolvedFile *getFile(IFileDescriptor &fileDesc) = 0;
//    // Releases any lock and re-reads the information
//    virtual IFileDescriptor *releaseFile(IResolvedFile &file) = 0;
//};

// ==================================================================================
/*
 * URI deals with strings referring to paths that can be resolved in
 * many different ways. This object is immutable.
 *
 * Dali files (logic, super, stream), local files (on disk),
 * Web files (http, ftp, webdav) have different ways of resolving, and all of them
 * should have a consistent query mechanism from the HPCC engines point of view.
 *
 * The URI parser used is uriparser, from http://uriparser.sourceforge.net/
 */
class URI
{
    URISchemeType scheme;
    URIServerDescription server;
    URIPathDescription path;
    UriParserStateA state;
    UriUriA uri;

    void populateFields()
    {
        // Scheme (defines which resolver to use, see above)
        StringBuffer schemeStr(uri.scheme.afterLast - uri.scheme.first, uri.scheme.first);
        schemeStr.toLowerCase();
        if (strcmp(schemeStr.str(), "hpcc") == 0)
            scheme = URIScheme_hpcc;
        else if (strcmp(schemeStr.str(), "file") == 0)
            scheme = URIScheme_file;
        else
            scheme = URIScheme_error;

        // Server
        server.user.set(uri.userInfo.first, uri.userInfo.afterLast - uri.userInfo.first);
        server.host.set(uri.hostText.first, uri.hostText.afterLast - uri.hostText.first);
        StringAttr portStr(uri.portText.first, uri.portText.afterLast - uri.portText.first);
        server.port = atoi(portStr.get()); // More - use default ports?

        // Path
        UriPathSegmentA* cur = uri.pathHead;
        StringBuffer pathStr;
        if (uri.absolutePath || scheme == URIScheme_file)
            pathStr.append("/");
        bool first = true;
        while (cur)
        {
            if (!first)
                pathStr.append("/");
            pathStr.append(cur->text.afterLast - cur->text.first, cur->text.first);
            first = false;
            cur = cur->next;
        }
        path.path.set(pathStr.str());

        // Extra info
        if (scheme == URIScheme_hpcc)
        {
            StringBuffer query(uri.query.afterLast - uri.query.first, uri.query.first);
            query.toLowerCase();
            if (strcmp(query.str(), "super") == 0)
            {
                path.type = URIFile_super;
                path.subname.set(uri.fragment.first, uri.fragment.afterLast - uri.fragment.first);
                path.index = 0;
            }
            else if (strcmp(query.str(), "stream") == 0)
            {
                path.type = URIFile_stream;
                StringAttr index(uri.fragment.first, uri.fragment.afterLast - uri.fragment.first);
                path.index = atoi(index.get());
            }
            else
            {
                path.type = URIFile_logic;
                path.index = 0;
            }
        }
        else
        {
            path.type = URIFile_local;
            path.index = 0;
        }
    }

public:
    URI(const char* path)
    {
        state.uri = &uri;
        if (uriParseUriA(&state, path) != URI_SUCCESS)
        {
            uriFreeUriMembersA(&uri);
            throw MakeStringException(-1, "Invalid URI '%s'", path); // all free by now
        }
        populateFields(); // In a format we understand
        uriFreeUriMembersA(&uri);
    }

    // Helper, to validate URI before creating object
    static bool isURI(const char *path)
    {
        UriParserStateA state;
        UriUriA uri;
        state.uri = &uri;
        bool match = (uriParseUriA(&state, path) == URI_SUCCESS);
        uriFreeUriMembersA(&uri);
        return match;
    }

    // Immutable
    URISchemeType getScheme() const
    {
        return scheme;
    }
    // Immutable
    const URIServerDescription * const getServer() const
    {
        return &server;
    }
    // Immutable
    const URIPathDescription * const getPath() const
    {
        return &path;
    }
};

#endif /* __JURI__ */
