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
    URIFile_logic,    // Normal files
    URIFile_super,    // Super files
    URIFile_stream    // Stream files (to be implemented)
};

struct URIServerDescription
{
    StringAttr user;
    StringAttr passwd;
    StringAttr host;
    unsigned port;
};

struct URIPathDescription
{
    StringAttr path;
    URIFileType type;
    // Super files' sub
    StringAttr subname;
    // Stream files
    unsigned index;
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
 * many different ways. This object is immutable
 *
 * Dali files (logic, super, stream), local files (on disk),
 * Web files (http, ftp, webdav) have different ways of resolving, and all of them
 * should have a consistent query mechanism from the HPCC engines point of view.
 */
class URI
{
    URISchemeType scheme;
    URIServerDescription server;
    URIPathDescription path;

    /*
     * We have to be very strict here, since the enum is what will tell
     * which URIFileResolver to use (see above).
     */
    void validateScheme(StringAttr &str)
    {
        str.toUpperCase();
        if (strcmp(str.get(), "HPCC") == 0)
            scheme = URIScheme_hpcc;
        else if (strcmp(str.get(), "FILE") == 0)
            scheme = URIScheme_file;
        else
            throw MakeStringException(-1, "String '%s' is not a supported scheme", str.get());
    }

    void validateServer(StringAttr &str)
    {
        server.port = 0;
        bool hasUserHostSep = false;
        switch (scheme)
        {
        case URIScheme_hpcc:
            break;
        case URIScheme_file:
            break;
        default:
            throw MakeStringException(-1, "Validate scheme first");
        }
    }

    void validatePath(StringAttr &str)
    {
    }

public:
    URI(const char* str)
    {
        // Parse string
        StringAttr uri(str);
        // Validate each part separately, in order
        validateScheme(uri);
        validateServer(uri);
        validatePath(uri);
    }

    // Helper, to validate URI before creating object
    static bool isURI(const char *path)
    {
        return true;
    }

    // Return by copy, as we need this object to be immutable
    URISchemeType getScheme() const
    {
        return scheme;
    }
    // Return by copy, as we need this object to be immutable
    URIServerDescription getServer() const
    {
        return server;
    }
    // Return by copy, as we need this object to be immutable
    URIPathDescription getPath() const
    {
        return path;
    }
};

#endif /* __JURI__ */
