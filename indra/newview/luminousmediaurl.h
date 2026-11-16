/**
 * @file luminousmediaurl.h
 * @brief How the media filter for media on prims reads a page's address: the host a browser or libcurl would connect
 * to, and what the alert shows. The standard library only.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Luminous Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
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
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMINOUS_MEDIA_URL_H
#define LUMINOUS_MEDIA_URL_H

#include <cctype>
#include <string>

namespace LuminousMediaURL
{
    /// A page's address as the media filter reads it.
    struct Parts
    {
        std::string scheme; ///< lower case; "http" for an address without one, as a browser and libcurl take it
        std::string host;   ///< lower case, without user:password@, port or a trailing dot; for an address without a
                            ///< host (data:, javascript: and the like) the scheme and a colon, "data:"
        std::string port;
        std::string path;   ///< from the first '/' after the host up to the query or fragment
        bool local = false; ///< nothing to fetch from a network: empty, about:, file:, secondlife:
    };

    inline std::string lower(std::string text)
    {
        for (char& c : text)
        {
            c = (char)std::tolower((unsigned char)c);
        }
        return text;
    }

    /// Without the spaces and control characters at either end, which a browser drops too.
    inline std::string trimmed(const std::string& text)
    {
        size_t first = 0;
        size_t last = text.size();
        while (first < last && (unsigned char)text[first] <= 0x20)
        {
            ++first;
        }
        while (last > first && (unsigned char)text[last - 1] <= 0x20)
        {
            --last;
        }
        return text.substr(first, last - first);
    }

    inline Parts parse(const std::string& address, int depth = 0)
    {
        Parts parts;
        const std::string url = trimmed(address);
        if (url.empty())
        {
            parts.local = true;
            return parts;
        }

        // A scheme: a letter, then letters, digits, '+', '-' or '.', up to a ':' that comes before any '/', '?', '#'.
        std::string rest = url;
        const size_t colon = url.find(':');
        const size_t stop = url.find_first_of("/?#\\");
        bool has_scheme = colon != std::string::npos && colon > 0 && (stop == std::string::npos || colon < stop)
                          && std::isalpha((unsigned char)url[0]);
        for (size_t i = 1; has_scheme && i < colon; ++i)
        {
            const char c = url[i];
            has_scheme = std::isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.';
        }
        // "host:8080/p" reads as a scheme to the rule above; a browser takes it as a host with a port.
        if (has_scheme && colon + 1 < url.size() && std::isdigit((unsigned char)url[colon + 1]))
        {
            has_scheme = false;
        }
        if (has_scheme)
        {
            parts.scheme = lower(url.substr(0, colon));
            rest = url.substr(colon + 1);
        }

        if (parts.scheme == "about" || parts.scheme == "file" || parts.scheme == "secondlife")
        {
            parts.local = true;
            return parts;
        }
        if (parts.scheme == "view-source" && depth < 4)
        {
            return parse(rest, depth + 1); // a wrapper: the page inside is what loads
        }

        size_t start = 0;
        while (start < rest.size() && (rest[start] == '/' || rest[start] == '\\'))
        {
            ++start;
        }
        if (has_scheme && start == 0)
        {
            // No host part (data:, javascript:, mailto: and the like): the scheme stands for the domain.
            parts.host = parts.scheme + ":";
            return parts;
        }

        // The host part ends at the first '/', '?', '#' or '\', as it does for a browser.
        const size_t end = rest.find_first_of("/?#\\", start);
        std::string authority = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (end != std::string::npos && (rest[end] == '/' || rest[end] == '\\'))
        {
            const size_t path_end = rest.find_first_of("?#", end);
            parts.path = rest.substr(end, path_end == std::string::npos ? std::string::npos : path_end - end);
            for (char& c : parts.path)
            {
                if (c == '\\')
                {
                    c = '/'; // as a browser reads it
                }
            }
        }
        const size_t at = authority.rfind('@');
        if (at != std::string::npos)
        {
            authority = authority.substr(at + 1);
        }
        if (!authority.empty() && authority[0] == '[')
        {
            const size_t close = authority.find(']');
            parts.host = authority.substr(0, close == std::string::npos ? std::string::npos : close + 1);
            if (close != std::string::npos && close + 1 < authority.size() && authority[close + 1] == ':')
            {
                parts.port = authority.substr(close + 2);
            }
        }
        else
        {
            const size_t port_colon = authority.rfind(':');
            parts.host = authority.substr(0, port_colon);
            if (port_colon != std::string::npos)
            {
                parts.port = authority.substr(port_colon + 1);
            }
        }
        parts.host = lower(parts.host);
        while (!parts.host.empty() && parts.host.back() == '.')
        {
            parts.host.pop_back();
        }
        if (parts.scheme.empty())
        {
            parts.scheme = "http";
        }
        return parts;
    }

    /// A listed domain allows or blocks itself and the domains under it: "example.com" matches "example.com" and
    /// "www.example.com", not "notexample.com".
    inline bool domainMatches(const std::string& domain, const std::string& listed)
    {
        std::string want = lower(trimmed(listed));
        while (!want.empty() && want.front() == '.')
        {
            want.erase(0, 1);
        }
        if (want.empty() || domain.size() < want.size())
        {
            return false;
        }
        if (domain == want)
        {
            return true;
        }
        return domain.size() > want.size() && domain.compare(domain.size() - want.size(), want.size(), want) == 0
               && domain[domain.size() - want.size() - 1] == '.';
    }

    /// The address as the alert shows it: the host and the first part of the path only. The query, the rest of the
    /// path and a user:password@ may hold a session's token, and the alert may be shown to others in a picture.
    inline std::string shown(const Parts& parts)
    {
        if (parts.host.empty() || parts.host.back() == ':')
        {
            return parts.host;
        }
        std::string path = parts.path;
        const size_t second = path.find_first_of("/\\", 1);
        if (second != std::string::npos && second + 1 < path.size())
        {
            path = path.substr(0, second + 1) + "...";
        }
        return parts.scheme + "://" + parts.host + (parts.port.empty() ? std::string() : ":" + parts.port) + path;
    }
}

#endif // LUMINOUS_MEDIA_URL_H
