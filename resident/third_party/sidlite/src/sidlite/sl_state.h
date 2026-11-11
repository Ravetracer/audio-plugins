/*
 * This file is part of ReSIDent's copy of SIDLite (libsidplayfp).
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

// State visitors: a class lists the fields that change while it runs once,
// in state(V&), and these size, write and read them. The bytes are the
// fields' own representation, so a state is only readable by the same build.

#ifndef SIDLITE_STATE_H
#define SIDLITE_STATE_H

#include <cstring>
#include <type_traits>

namespace SIDLite
{

struct StateSizer
{
    int size = 0;
    template<class T> void operator()(const T&)
    {
        static_assert(std::is_trivially_copyable<T>::value, "state fields must be plain data");
        size += static_cast<int>(sizeof(T));
    }
};

struct StateWriter
{
    unsigned char* p;
    template<class T> void operator()(const T& t)
    {
        std::memcpy(p, &t, sizeof(T));
        p += sizeof(T);
    }
};

struct StateReader
{
    const unsigned char* p;
    template<class T> void operator()(T& t)
    {
        std::memcpy(&t, p, sizeof(T));
        p += sizeof(T);
    }
};

}

#endif // SIDLITE_STATE_H
