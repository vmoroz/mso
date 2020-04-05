// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once
#ifndef MSO_CPPEXTENSIONS_AUTORESTORE_H
#define MSO_CPPEXTENSIONS_AUTORESTORE_H

#include <cstdint>

namespace Mso {
//! Return compile-time size of an array.
//! It can be used instead of MSVC _countof macro
template <class T, std::size_t N>
constexpr std::size_t SizeOf([[maybe_unused]] T (&arr)[N]) noexcept
{
  return N;
}

} // namespace Mso

#endif // MSO_CPPEXTENSIONS_AUTORESTORE_H