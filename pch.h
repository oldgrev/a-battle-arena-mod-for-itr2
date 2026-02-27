// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

// add headers that you want to pre-compile here
#include "framework.h"

// Suppress warnings caused by SDK types using double (UE5 FVector/FRotator) while mod
// code uses float.  These are safe narrowing conversions for the precision we need.
//   C4244: conversion from 'double' to 'float', possible loss of data
//   C4267: conversion from 'size_t' to 'int', possible loss of data
#pragma warning(disable: 4244 4267)

#pragma warning(push, 0)
#include ".\CppSDK\SDK.hpp"
#pragma warning(pop)

#endif //PCH_H
