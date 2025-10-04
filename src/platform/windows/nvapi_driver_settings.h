#pragma once

#ifndef __success
  #define NVAPI_WRAPPER_DEFINED___success
  #define __success(expr)
#endif

#ifndef __in
  #define NVAPI_WRAPPER_DEFINED___in
  #define __in
#endif

#ifndef __in_bcount
  #define NVAPI_WRAPPER_DEFINED___in_bcount
  #define __in_bcount(...)
#endif

#ifndef __in_ecount
  #define NVAPI_WRAPPER_DEFINED___in_ecount
  #define __in_ecount(...)
#endif

#ifndef __in_opt
  #define NVAPI_WRAPPER_DEFINED___in_opt
  #define __in_opt
#endif

#ifndef __inout
  #define NVAPI_WRAPPER_DEFINED___inout
  #define __inout
#endif

#ifndef __inout_ecount_full
  #define NVAPI_WRAPPER_DEFINED___inout_ecount_full
  #define __inout_ecount_full(...)
#endif

#ifndef __inout_ecount_part_opt
  #define NVAPI_WRAPPER_DEFINED___inout_ecount_part_opt
  #define __inout_ecount_part_opt(...)
#endif

#ifndef __inout_opt
  #define NVAPI_WRAPPER_DEFINED___inout_opt
  #define __inout_opt
#endif

#ifndef __out
  #define NVAPI_WRAPPER_DEFINED___out
  #define __out
#endif

#ifndef __out_ecount_full_opt
  #define NVAPI_WRAPPER_DEFINED___out_ecount_full_opt
  #define __out_ecount_full_opt(...)
#endif

#ifndef __out_ecount_opt
  #define NVAPI_WRAPPER_DEFINED___out_ecount_opt
  #define __out_ecount_opt(...)
#endif

#ifndef __out_opt
  #define NVAPI_WRAPPER_DEFINED___out_opt
  #define __out_opt
#endif

#include <nvapi.h>
#include <NvApiDriverSettings.h>

#ifdef NVAPI_WRAPPER_DEFINED___success
  #undef __success
  #undef NVAPI_WRAPPER_DEFINED___success
#endif

#ifdef NVAPI_WRAPPER_DEFINED___in
  #undef __in
  #undef NVAPI_WRAPPER_DEFINED___in
#endif

#ifdef NVAPI_WRAPPER_DEFINED___in_bcount
  #undef __in_bcount
  #undef NVAPI_WRAPPER_DEFINED___in_bcount
#endif

#ifdef NVAPI_WRAPPER_DEFINED___in_ecount
  #undef __in_ecount
  #undef NVAPI_WRAPPER_DEFINED___in_ecount
#endif

#ifdef NVAPI_WRAPPER_DEFINED___in_opt
  #undef __in_opt
  #undef NVAPI_WRAPPER_DEFINED___in_opt
#endif

#ifdef NVAPI_WRAPPER_DEFINED___inout
  #undef __inout
  #undef NVAPI_WRAPPER_DEFINED___inout
#endif

#ifdef NVAPI_WRAPPER_DEFINED___inout_ecount_full
  #undef __inout_ecount_full
  #undef NVAPI_WRAPPER_DEFINED___inout_ecount_full
#endif

#ifdef NVAPI_WRAPPER_DEFINED___inout_ecount_part_opt
  #undef __inout_ecount_part_opt
  #undef NVAPI_WRAPPER_DEFINED___inout_ecount_part_opt
#endif

#ifdef NVAPI_WRAPPER_DEFINED___inout_opt
  #undef __inout_opt
  #undef NVAPI_WRAPPER_DEFINED___inout_opt
#endif

#ifdef NVAPI_WRAPPER_DEFINED___out
  #undef __out
  #undef NVAPI_WRAPPER_DEFINED___out
#endif

#ifdef NVAPI_WRAPPER_DEFINED___out_ecount_full_opt
  #undef __out_ecount_full_opt
  #undef NVAPI_WRAPPER_DEFINED___out_ecount_full_opt
#endif

#ifdef NVAPI_WRAPPER_DEFINED___out_ecount_opt
  #undef __out_ecount_opt
  #undef NVAPI_WRAPPER_DEFINED___out_ecount_opt
#endif

#ifdef NVAPI_WRAPPER_DEFINED___out_opt
  #undef __out_opt
  #undef NVAPI_WRAPPER_DEFINED___out_opt
#endif
