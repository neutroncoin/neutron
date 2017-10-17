// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_STREAMS_H
#define BITCOIN_STREAMS_H

#include "allocators.h"
#include "serialize.h"

#include <algorithm>
#include <assert.h>
#include <ios>
#include <limits>
#include <map>
#include <set>
#include <stdint.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

/** Double ended buffer combining vector and stream-like interfaces.
 *
 * >> and << read and write unformatted data using the above serialization templates.
 * Fills with data in linear time; some stringstream implementations take N^2 time.
 */
class CDataStream
{
protected:
    typedef CSerializeData vector_type;
    vector_type vch;
    unsigned int nReadPos;
    short state;
    short exceptmask;
public:
    int nType;
    int nVersion;

    typedef vector_type::allocator_type   allocator_type;
    typedef vector_type::size_type        size_type;
    typedef vector_type::difference_type  difference_type;
    typedef vector_type::reference        reference;
    typedef vector_type::const_reference  const_reference;
    typedef vector_type::value_type       value_type;
    typedef vector_type::iterator         iterator;
    typedef vector_type::const_iterator   const_iterator;
    typedef vector_type::reverse_iterator reverse_iterator;

    explicit CDataStream(int nTypeIn, int nVersionIn)
    {
        Init(nTypeIn, nVersionIn);
    }

    CDataStream(const_iterator pbegin, const_iterator pend, int nTypeIn, int nVersionIn) : vch(pbegin, pend)
    {
        Init(nTypeIn, nVersionIn);
    }

#if !defined(_MSC_VER) || _MSC_VER >= 1300
    CDataStream(const char* pbegin, const char* pend, int nTypeIn, int nVersionIn) : vch(pbegin, pend)
    {
        Init(nTypeIn, nVersionIn);
    }
#endif

    CDataStream(const vector_type& vchIn, int nTypeIn, int nVersionIn) : vch(vchIn.begin(), vchIn.end())
    {
        Init(nTypeIn, nVersionIn);
    }

    CDataStream(const std::vector<char>& vchIn, int nTypeIn, int nVersionIn) : vch(vchIn.begin(), vchIn.end())
    {
        Init(nTypeIn, nVersionIn);
    }

    CDataStream(const std::vector<unsigned char>& vchIn, int nTypeIn, int nVersionIn) : vch((char*)&vchIn.begin()[0], (char*)&vchIn.end()[0])
    {
        Init(nTypeIn, nVersionIn);
    }

    void Init(int nTypeIn, int nVersionIn)
    {
        nReadPos = 0;
        nType = nTypeIn;
        nVersion = nVersionIn;
        state = 0;
        exceptmask = std::ios::badbit | std::ios::failbit;
    }

    CDataStream& operator+=(const CDataStream& b)
    {
        vch.insert(vch.end(), b.begin(), b.end());
        return *this;
    }

    friend CDataStream operator+(const CDataStream& a, const CDataStream& b)
    {
        CDataStream ret = a;
        ret += b;
        return (ret);
    }

    std::string str() const
    {
        return (std::string(begin(), end()));
    }


    //
    // Vector subset
    //
    const_iterator begin() const                     { return vch.begin() + nReadPos; }
    iterator begin()                                 { return vch.begin() + nReadPos; }
    const_iterator end() const                       { return vch.end(); }
    iterator end()                                   { return vch.end(); }
    size_type size() const                           { return vch.size() - nReadPos; }
    bool empty() const                               { return vch.size() == nReadPos; }
    void resize(size_type n, value_type c=0)         { vch.resize(n + nReadPos, c); }
    void reserve(size_type n)                        { vch.reserve(n + nReadPos); }
    const_reference operator[](size_type pos) const  { return vch[pos + nReadPos]; }
    reference operator[](size_type pos)              { return vch[pos + nReadPos]; }
    void clear()                                     { vch.clear(); nReadPos = 0; }
    iterator insert(iterator it, const char& x=char()) { return vch.insert(it, x); }
    void insert(iterator it, size_type n, const char& x) { vch.insert(it, n, x); }


    void insert(iterator it, std::vector<char>::const_iterator first, std::vector<char>::const_iterator last)
    {
        assert(last - first >= 0);
        if (it == vch.begin() + nReadPos && (unsigned int)(last - first) <= nReadPos)
        {
            // special case for inserting at the front when there's room
            nReadPos -= (last - first);
            memcpy(&vch[nReadPos], &first[0], last - first);
        }
        else
            vch.insert(it, first, last);
    }

#if !defined(_MSC_VER) || _MSC_VER >= 1300
    void insert(iterator it, const char* first, const char* last)
    {
        assert(last - first >= 0);
        if (it == vch.begin() + nReadPos && (unsigned int)(last - first) <= nReadPos)
        {
            // special case for inserting at the front when there's room
            nReadPos -= (last - first);
            memcpy(&vch[nReadPos], &first[0], last - first);
        }
        else
            vch.insert(it, first, last);
    }
#endif

    iterator erase(iterator it)
    {
        if (it == vch.begin() + nReadPos)
        {
            // special case for erasing from the front
            if (++nReadPos >= vch.size())
            {
                // whenever we reach the end, we take the opportunity to clear the buffer
                nReadPos = 0;
                return vch.erase(vch.begin(), vch.end());
            }
            return vch.begin() + nReadPos;
        }
        else
            return vch.erase(it);
    }

    iterator erase(iterator first, iterator last)
    {
        if (first == vch.begin() + nReadPos)
        {
            // special case for erasing from the front
            if (last == vch.end())
            {
                nReadPos = 0;
                return vch.erase(vch.begin(), vch.end());
            }
            else
            {
                nReadPos = (last - vch.begin());
                return last;
            }
        }
        else
            return vch.erase(first, last);
    }

    inline void Compact()
    {
        vch.erase(vch.begin(), vch.begin() + nReadPos);
        nReadPos = 0;
    }

    bool Rewind(size_type n)
    {
        // Rewind by n characters if the buffer hasn't been compacted yet
        if (n > nReadPos)
            return false;
        nReadPos -= n;
        return true;
    }


    //
    // Stream subset
    //
    void setstate(short bits, const char* psz)
    {
        state |= bits;
        if (state & exceptmask)
            THROW_WITH_STACKTRACE(std::ios_base::failure(psz));
    }

    bool eof() const             { return size() == 0; }
    bool fail() const            { return state & (std::ios::badbit | std::ios::failbit); }
    bool good() const            { return !eof() && (state == 0); }
    void clear(short n)          { state = n; }  // name conflict with vector clear()
    short exceptions()           { return exceptmask; }
    short exceptions(short mask) { short prev = exceptmask; exceptmask = mask; setstate(0, "CDataStream"); return prev; }
    CDataStream* rdbuf()         { return this; }
    int in_avail()               { return size(); }

    void SetType(int n)          { nType = n; }
    int GetType()                { return nType; }
    void SetVersion(int n)       { nVersion = n; }
    int GetVersion()             { return nVersion; }
    void ReadVersion()           { *this >> nVersion; }
    void WriteVersion()          { *this << nVersion; }

    CDataStream& read(char* pch, int nSize)
    {
        // Read from the beginning of the buffer
        assert(nSize >= 0);
        unsigned int nReadPosNext = nReadPos + nSize;
        if (nReadPosNext >= vch.size())
        {
            if (nReadPosNext > vch.size())
            {
                setstate(std::ios::failbit, "CDataStream::read() : end of data");
                memset(pch, 0, nSize);
                nSize = vch.size() - nReadPos;
            }
            memcpy(pch, &vch[nReadPos], nSize);
            nReadPos = 0;
            vch.clear();
            return (*this);
        }
        memcpy(pch, &vch[nReadPos], nSize);
        nReadPos = nReadPosNext;
        return (*this);
    }

    CDataStream& ignore(int nSize)
    {
        // Ignore from the beginning of the buffer
        assert(nSize >= 0);
        unsigned int nReadPosNext = nReadPos + nSize;
        if (nReadPosNext >= vch.size())
        {
            if (nReadPosNext > vch.size())
                setstate(std::ios::failbit, "CDataStream::ignore() : end of data");
            nReadPos = 0;
            vch.clear();
            return (*this);
        }
        nReadPos = nReadPosNext;
        return (*this);
    }

    CDataStream& write(const char* pch, int nSize)
    {
        // Write to the end of the buffer
        assert(nSize >= 0);
        vch.insert(vch.end(), pch, pch + nSize);
        return (*this);
    }

    template<typename Stream>
    void Serialize(Stream& s, int nType, int nVersion) const
    {
        // Special case: stream << stream concatenates like stream += stream
        if (!vch.empty())
            s.write((char*)&vch[0], vch.size() * sizeof(vch[0]));
    }

    template<typename T>
    unsigned int GetSerializeSize(const T& obj)
    {
        // Tells the size of the object if serialized to this stream
        return ::GetSerializeSize(obj, nType, nVersion);
    }

    template<typename T>
    CDataStream& operator<<(const T& obj)
    {
        // Serialize to this stream
        ::Serialize(*this, obj, nType, nVersion);
        return (*this);
    }

    template<typename T>
    CDataStream& operator>>(T& obj)
    {
        // Unserialize from this stream
        ::Unserialize(*this, obj, nType, nVersion);
        return (*this);
    }

    void GetAndClear(CSerializeData &data) {
        data.insert(data.end(), begin(), end());
        clear();
    }
};


/** RAII wrapper for FILE*.
 *
 * Will automatically close the file when it goes out of scope if not null.
 * If you're returning the file pointer, return file.release().
 * If you need to close the file early, use file.fclose() instead of fclose(file).
 */
class CAutoFile
{
protected:
    FILE* file;
    short state;
    short exceptmask;
public:
    int nType;
    int nVersion;

    CAutoFile(FILE* filenew, int nTypeIn, int nVersionIn)
    {
        file = filenew;
        nType = nTypeIn;
        nVersion = nVersionIn;
        state = 0;
        exceptmask = std::ios::badbit | std::ios::failbit;
    }

    ~CAutoFile()
    {
        fclose();
    }

    void fclose()
    {
        if (file != NULL && file != stdin && file != stdout && file != stderr)
            ::fclose(file);
        file = NULL;
    }

    FILE* release()             { FILE* ret = file; file = NULL; return ret; }
    operator FILE*()            { return file; }
    FILE* operator->()          { return file; }
    FILE& operator*()           { return *file; }
    FILE** operator&()          { return &file; }
    FILE* operator=(FILE* pnew) { return file = pnew; }
    bool operator!()            { return (file == NULL); }


    //
    // Stream subset
    //
    void setstate(short bits, const char* psz)
    {
        state |= bits;
        if (state & exceptmask)
            THROW_WITH_STACKTRACE(std::ios_base::failure(psz));
    }

    bool fail() const            { return state & (std::ios::badbit | std::ios::failbit); }
    bool good() const            { return state == 0; }
    void clear(short n = 0)      { state = n; }
    short exceptions()           { return exceptmask; }
    short exceptions(short mask) { short prev = exceptmask; exceptmask = mask; setstate(0, "CAutoFile"); return prev; }

    void SetType(int n)          { nType = n; }
    int GetType()                { return nType; }
    void SetVersion(int n)       { nVersion = n; }
    int GetVersion()             { return nVersion; }
    void ReadVersion()           { *this >> nVersion; }
    void WriteVersion()          { *this << nVersion; }

    CAutoFile& read(char* pch, size_t nSize)
    {
        if (!file)
            throw std::ios_base::failure("CAutoFile::read : file handle is NULL");
        if (fread(pch, 1, nSize, file) != nSize)
            setstate(std::ios::failbit, feof(file) ? "CAutoFile::read : end of file" : "CAutoFile::read : fread failed");
        return (*this);
    }

    CAutoFile& write(const char* pch, size_t nSize)
    {
        if (!file)
            throw std::ios_base::failure("CAutoFile::write : file handle is NULL");
        if (fwrite(pch, 1, nSize, file) != nSize)
            setstate(std::ios::failbit, "CAutoFile::write : write failed");
        return (*this);
    }

    template<typename T>
    unsigned int GetSerializeSize(const T& obj)
    {
        // Tells the size of the object if serialized to this stream
        return ::GetSerializeSize(obj, nType, nVersion);
    }

    template<typename T>
    CAutoFile& operator<<(const T& obj)
    {
        // Serialize to this stream
        if (!file)
            throw std::ios_base::failure("CAutoFile::operator<< : file handle is NULL");
        ::Serialize(*this, obj, nType, nVersion);
        return (*this);
    }

    template<typename T>
    CAutoFile& operator>>(T& obj)
    {
        // Unserialize from this stream
        if (!file)
            throw std::ios_base::failure("CAutoFile::operator>> : file handle is NULL");
        ::Unserialize(*this, obj, nType, nVersion);
        return (*this);
    }
};

#endif // BITCOIN_STREAMS_H
