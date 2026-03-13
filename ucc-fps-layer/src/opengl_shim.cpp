// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fps_reporter.hpp"

#include <dlfcn.h>

#include <cstring>
#include <mutex>

#if defined(__GNUC__) || defined(__clang__)
#define UCC_GL_EXPORT __attribute__((visibility("default")))
#else
#define UCC_GL_EXPORT
#endif

extern "C" {
UCC_GL_EXPORT void glXSwapBuffers( void *dpy, unsigned long drawable );
UCC_GL_EXPORT long long glXSwapBuffersMscOML( void *dpy,
                                              unsigned long drawable,
                                              long long targetMsc,
                                              long long divisor,
                                              long long remainder );
UCC_GL_EXPORT void *glXGetProcAddress( const unsigned char *procName );
UCC_GL_EXPORT void *glXGetProcAddressARB( const unsigned char *procName );
UCC_GL_EXPORT unsigned int eglSwapBuffers( void *display, void *surface );
UCC_GL_EXPORT void *eglGetProcAddress( const char *procName );
}

namespace {

using glXSwapBuffers_t = void (*)( void *, unsigned long );
using glXSwapBuffersMscOML_t = long long (*)( void *, unsigned long, long long, long long, long long );
using glXGetProcAddress_t = void *( * )( const unsigned char * );
using eglSwapBuffers_t = unsigned int (*)( void *, void * );
using eglGetProcAddress_t = void *( * )( const char * );
using dlsym_t = void *( * )( void *, const char * );

template < typename Fn >
void *fn_to_ptr( Fn fn )
{
  union
  {
    Fn fn;
    void *ptr;
  } cast{};
  cast.fn = fn;
  return cast.ptr;
}

dlsym_t resolve_real_dlsym()
{
  static dlsym_t realDlsym = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
#if defined(__GLIBC__)
    realDlsym = reinterpret_cast<dlsym_t>( ::dlvsym( RTLD_NEXT, "dlsym", "GLIBC_2.2.5" ) );
    if ( !realDlsym )
      realDlsym = reinterpret_cast<dlsym_t>( ::dlvsym( RTLD_DEFAULT, "dlsym", "GLIBC_2.2.5" ) );
#endif
  } );
  return realDlsym;
}

void *real_lookup( const char *symbol )
{
  dlsym_t realDlsym = resolve_real_dlsym();
  return realDlsym ? realDlsym( RTLD_NEXT, symbol ) : nullptr;
}

void *find_hook_ptr( const char *name );

glXSwapBuffers_t real_glx_swap_buffers()
{
  static glXSwapBuffers_t realFn = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
    realFn = reinterpret_cast<glXSwapBuffers_t>( real_lookup( "glXSwapBuffers" ) );
  } );
  return realFn;
}

glXSwapBuffersMscOML_t real_glx_swap_buffers_msc_oml()
{
  static glXSwapBuffersMscOML_t realFn = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
    realFn = reinterpret_cast<glXSwapBuffersMscOML_t>( real_lookup( "glXSwapBuffersMscOML" ) );
  } );
  return realFn;
}

glXGetProcAddress_t real_glx_get_proc_address()
{
  static glXGetProcAddress_t realFn = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
    realFn = reinterpret_cast<glXGetProcAddress_t>( real_lookup( "glXGetProcAddress" ) );
    if ( !realFn )
      realFn = reinterpret_cast<glXGetProcAddress_t>( real_lookup( "glXGetProcAddressARB" ) );
  } );
  return realFn;
}

eglSwapBuffers_t real_egl_swap_buffers()
{
  static eglSwapBuffers_t realFn = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
    realFn = reinterpret_cast<eglSwapBuffers_t>( real_lookup( "eglSwapBuffers" ) );
  } );
  return realFn;
}

eglGetProcAddress_t real_egl_get_proc_address()
{
  static eglGetProcAddress_t realFn = nullptr;
  static std::once_flag once;
  std::call_once( once, []() {
    realFn = reinterpret_cast<eglGetProcAddress_t>( real_lookup( "eglGetProcAddress" ) );
  } );
  return realFn;
}

void *find_hook_ptr( const char *name )
{
  if ( !name )
    return nullptr;
  if ( std::strcmp( name, "glXSwapBuffers" ) == 0 )
    return fn_to_ptr( reinterpret_cast<glXSwapBuffers_t>( &glXSwapBuffers ) );
  if ( std::strcmp( name, "glXSwapBuffersMscOML" ) == 0 )
    return fn_to_ptr( reinterpret_cast<glXSwapBuffersMscOML_t>( &glXSwapBuffersMscOML ) );
  if ( std::strcmp( name, "glXGetProcAddress" ) == 0 )
    return fn_to_ptr( reinterpret_cast<glXGetProcAddress_t>( &glXGetProcAddress ) );
  if ( std::strcmp( name, "glXGetProcAddressARB" ) == 0 )
    return fn_to_ptr( reinterpret_cast<glXGetProcAddress_t>( &glXGetProcAddressARB ) );
  if ( std::strcmp( name, "eglSwapBuffers" ) == 0 )
    return fn_to_ptr( reinterpret_cast<eglSwapBuffers_t>( &eglSwapBuffers ) );
  if ( std::strcmp( name, "eglGetProcAddress" ) == 0 )
    return fn_to_ptr( reinterpret_cast<eglGetProcAddress_t>( &eglGetProcAddress ) );
  return nullptr;
}

}  // namespace

extern "C" {

UCC_GL_EXPORT void glXSwapBuffers( void *dpy, unsigned long drawable )
{
  uccfps::record_frame();
  if ( auto realFn = real_glx_swap_buffers() )
    realFn( dpy, drawable );
}

UCC_GL_EXPORT long long glXSwapBuffersMscOML( void *dpy,
                                              unsigned long drawable,
                                              long long targetMsc,
                                              long long divisor,
                                              long long remainder )
{
  uccfps::record_frame();
  if ( auto realFn = real_glx_swap_buffers_msc_oml() )
    return realFn( dpy, drawable, targetMsc, divisor, remainder );
  return -1;
}

UCC_GL_EXPORT void *glXGetProcAddress( const unsigned char *procName )
{
  void *realFn = nullptr;
  if ( auto realGetProc = real_glx_get_proc_address() )
    realFn = realGetProc( procName );

  void *hookFn = find_hook_ptr( reinterpret_cast<const char *>( procName ) );
  return hookFn && realFn ? hookFn : realFn;
}

UCC_GL_EXPORT void *glXGetProcAddressARB( const unsigned char *procName )
{
  return glXGetProcAddress( procName );
}

UCC_GL_EXPORT unsigned int eglSwapBuffers( void *display, void *surface )
{
  uccfps::record_frame();
  if ( auto realFn = real_egl_swap_buffers() )
    return realFn( display, surface );
  return 0U;
}

UCC_GL_EXPORT void *eglGetProcAddress( const char *procName )
{
  void *realFn = nullptr;
  if ( auto realGetProc = real_egl_get_proc_address() )
    realFn = realGetProc( procName );

  void *hookFn = find_hook_ptr( procName );
  return hookFn && realFn ? hookFn : realFn;
}

UCC_GL_EXPORT void *dlsym( void *handle, const char *name )
{
  void *realFn = nullptr;
  if ( auto realDlsym = resolve_real_dlsym() )
    realFn = realDlsym( handle, name );

  void *hookFn = find_hook_ptr( name );
  return hookFn && realFn ? hookFn : realFn;
}

}  // extern "C"