/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2019 HPCC Systems®.

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

#ifndef UDPIPMAP_INCL
#define UDPIPMAP_INCL

#include "jsocket.hpp"
#include "udplib.hpp"
#include <functional>
#include <iterator>
#include <algorithm>


template<class T> class IpMapOf
{
private:
    class list
    {
    public:
        list(const IpAddress &_ip, const list *_next, std::function<T *(const IpAddress &)> tfunc) : ip(_ip), next(_next)
        {
            entry = tfunc(ip);
        }
        ~list()
        {
            delete entry;
        }
        const IpAddress ip;
        const list *next;
        T *entry;
    };

    class myIterator
    {
    private:
        const list * value = nullptr;
        int hash = 0;
        const std::atomic<const list *> *table = nullptr;

    public:
        typedef T                     value_type;
        typedef std::ptrdiff_t        difference_type;
        typedef T*                    pointer;
        typedef T&                    reference;
        typedef std::input_iterator_tag iterator_category;

        explicit myIterator(const list *_value, int _hash, const std::atomic<const list *> *_table)
        : value(_value), hash(_hash), table(_table)
        {
        }
        reference operator*() const { return *value->entry; }
        bool operator==(const myIterator& other) const { return value == other.value && hash==other.hash; }
        bool operator!=(const myIterator& other) const { return !(*this == other); }
        myIterator operator++(int)
        {
            myIterator ret = *this;
            ++(*this);
            return ret;
        }
        myIterator& operator++()
        {
            value = value->next;
            while (!value)
            {
                hash += 1;
                if (hash==256)
                    break;
                value = table[hash].load(std::memory_order_acquire);
            }
            return *this;
        }
    };

public:
    typedef std::function<T *(const IpAddress &)> creatorFunc;
    IpMapOf<T>(creatorFunc _tfunc) : tfunc(_tfunc)
    {
    }
    T &lookup(const IpAddress &) const;
    inline T &operator[](const IpAddress &ip) const { return lookup(ip); }
    myIterator begin() { return myIterator(first, firstHash, table); }
    myIterator end()   { return myIterator(nullptr, 256, nullptr); }

private:
    const creatorFunc tfunc;
    mutable std::atomic<const list *> table[256] = {};
    mutable CriticalSection lock;
    mutable unsigned firstHash = 256;
    mutable std::atomic<const list *> first { nullptr };
};

template<class T> T &IpMapOf<T>::lookup(const IpAddress &ip) const
{
   unsigned hash = ip.fasthash() & 0xff;
   for (;;)
   {
       const list *head = table[hash].load(std::memory_order_acquire);
       const list *finger = head;
       while (finger)
       {
           if (finger->ip.ipequals(ip))
               return *finger->entry;
           finger = finger->next;
       }
       // If we get here, we need to add a new entry. This should be rare, so ok to lock
       // Note that we only lock out other additions, not other lookups
       // I could have a lock per table-entry if I thought it was worthwhile
       CriticalBlock b(lock);
       if (table[hash].load(std::memory_order_acquire) != head)
           continue;  // NOTE - an alternative implementation would be to rescan the list inside the critsec, but this is cleaner
       finger = new list(ip, head, tfunc);
       table[hash].store(finger, std::memory_order_release);
       if (hash <= firstHash)
       {
           firstHash = hash;
           first = finger;
       }
       return *finger->entry;
   }
}

#endif
