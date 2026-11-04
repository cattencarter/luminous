/**
 * @file llmslinclude.h
 * @brief Include resolution for hand-written MSL: header-only and dependency-free
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Viewer Lab contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLMSLINCLUDE_H
#define LL_LLMSLINCLUDE_H

// MTLCompileOptions has no include path, so the native program loader (LLNativeProgram) and the offline
// check (luminous/scripts/msl-check) expand `#include "x"` themselves before the text reaches the compiler.
// Paths are relative to one root (app_settings/shaders/metal) with '/' separators; nothing outside the
// root is ever read. Only the standard library is used, so the tool builds this header on its own.

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace LL::MSL
{

struct IncludeResult
{
    std::string text;               ///< the expanded source; empty when error is set
    std::vector<std::string> files; ///< every file read, once each, in the order first read, root-relative
    std::string error;              ///< empty when it worked
};

/// read(path) gets a root-relative path with '/' separators and returns the file's text, or nullopt.
using ReadFile = std::function<std::optional<std::string>(const std::string& path)>;

/// Lexically normalises a '/'-separated relative path ("a/./b/../c" -> "a/c"); empty when it leaves the
/// root, is absolute or is empty.
inline std::string normalizePath(const std::string& path)
{
    if (path.empty() || path[0] == '/')
    {
        return {};
    }
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size())
    {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
        {
            end = path.size();
        }
        const std::string part = path.substr(start, end - start);
        if (part == "..")
        {
            if (parts.empty())
            {
                return {};
            }
            parts.pop_back();
        }
        else if (!part.empty() && part != ".")
        {
            parts.push_back(part);
        }
        start = end + 1;
    }
    std::string out;
    for (const std::string& part : parts)
    {
        out += out.empty() ? part : "/" + part;
    }
    return out;
}

namespace detail
{
    /// The directory part of a root-relative path, with its trailing '/'; empty at the root.
    inline std::string directoryOf(const std::string& path)
    {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    }

    /// When line (code only, comments blanked) is `#include "name"`, gives name; spaces may stand before
    /// and after '#'. `#include <x>` and every other line give nullopt.
    inline std::optional<std::string> quotedInclude(const std::string& line)
    {
        size_t at = line.find_first_not_of(" \t");
        if (at == std::string::npos || line[at] != '#')
        {
            return std::nullopt;
        }
        at = line.find_first_not_of(" \t", at + 1);
        if (at == std::string::npos || line.compare(at, 7, "include") != 0)
        {
            return std::nullopt;
        }
        at = line.find_first_not_of(" \t", at + 7);
        if (at == std::string::npos || line[at] != '"')
        {
            return std::nullopt;
        }
        const size_t close = line.find('"', at + 1);
        if (close == std::string::npos)
        {
            return std::nullopt;
        }
        return line.substr(at + 1, close - at - 1);
    }

    /// The line with its comments blanked; inComment carries a block comment over lines. String
    /// literals are not tracked: an include line has none before its name.
    inline std::string codeOf(const std::string& line, bool& inComment)
    {
        std::string code(line.size(), ' ');
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (inComment)
            {
                if (line.compare(i, 2, "*/") == 0)
                {
                    inComment = false;
                    ++i;
                }
                continue;
            }
            if (line.compare(i, 2, "//") == 0)
            {
                break;
            }
            if (line.compare(i, 2, "/*") == 0)
            {
                inComment = true;
                ++i;
                continue;
            }
            code[i] = line[i];
        }
        return code;
    }

    /// open holds the files being expanded, from the first down to this one; seen every file read so far.
    inline bool expand(const std::string& file, const ReadFile& read, std::vector<std::string>& open,
                       std::set<std::string>& seen, IncludeResult& out, int depth)
    {
        if (depth > 64)
        {
            out.error = "includes nest deeper than 64 at '" + file + "'";
            return false;
        }
        const std::optional<std::string> text = read(file);
        if (!text)
        {
            out.error = "cannot read '" + file + "'";
            return false;
        }
        if (seen.insert(file).second)
        {
            out.files.push_back(file);
        }
        open.push_back(file);
        out.text += "#line 1 \"" + file + "\"\n";
        bool inComment = false;
        size_t start = 0;
        int number = 0;
        while (start < text->size())
        {
            size_t end = text->find('\n', start);
            if (end == std::string::npos)
            {
                end = text->size();
            }
            const std::string line = text->substr(start, end - start);
            start = end + 1;
            ++number;
            const std::optional<std::string> name = quotedInclude(codeOf(line, inComment));
            if (!name)
            {
                out.text += line;
                out.text += '\n';
                continue;
            }
            // Beside the includer first, then from the root.
            std::string found;
            const std::string beside = normalizePath(directoryOf(file) + *name);
            const std::string rooted = normalizePath(*name);
            if (beside.empty() && rooted.empty())
            {
                out.error = file + ":" + std::to_string(number) + ": '" + *name + "' leaves the shader root";
                return false;
            }
            if (!beside.empty() && (seen.count(beside) || read(beside)))
            {
                found = beside;
            }
            else if (!rooted.empty() && (seen.count(rooted) || read(rooted)))
            {
                found = rooted;
            }
            else
            {
                out.error = file + ":" + std::to_string(number) + ": cannot find include '" + *name + "'";
                return false;
            }
            // Every include is expanded, wherever it stands: one inside an #if the compiler skips still gives
            // its text, which the compiler then skips, and a header included twice keeps out by its include
            // guard. Only a file that includes itself, directly or through others, is an error.
            if (std::find(open.begin(), open.end(), found) != open.end())
            {
                out.error = file + ":" + std::to_string(number) + ": '" + found + "' includes itself";
                return false;
            }
            if (!expand(found, read, open, seen, out, depth + 1))
            {
                return false;
            }
            // Back to the line after the include in the includer.
            out.text += "#line " + std::to_string(number + 1) + " \"" + file + "\"\n";
        }
        open.pop_back();
        return true;
    }
}

/// file is root-relative. `#include "x"` resolves against the including file's directory first, then the root; a path
/// that leaves the root (..) is an error; every include is expanded where it stands, so headers keep out a second copy
/// with their own include guards, and an include cycle is an error; `#include <x>` stays as written. Each included file is wrapped in `#line 1 "<path>"` and followed by a `#line` back
/// to the includer, so compiler messages name the right file and line. files lists every file read, in order.
inline IncludeResult resolveIncludes(const std::string& file, const ReadFile& read)
{
    IncludeResult out;
    const std::string start = normalizePath(file);
    if (start.empty())
    {
        out.error = "'" + file + "' leaves the shader root";
        return out;
    }
    std::vector<std::string> open;
    std::set<std::string> seen;
    if (!detail::expand(start, read, open, seen, out, 0))
    {
        out.text.clear();
    }
    return out;
}

} // namespace LL::MSL

#endif // LL_LLMSLINCLUDE_H
