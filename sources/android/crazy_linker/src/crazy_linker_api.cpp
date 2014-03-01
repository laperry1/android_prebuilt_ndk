// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implements the crazy linker C-based API exposed by <crazy_linker.h>

#include <crazy_linker.h>

#include <string.h>

#include "crazy_linker_error.h"
#include "crazy_linker_ashmem.h"
#include "crazy_linker_globals.h"
#include "crazy_linker_proc_maps.h"
#include "crazy_linker_search_path_list.h"
#include "crazy_linker_shared_library.h"
#include "crazy_linker_thread.h"
#include "crazy_linker_util.h"
#include "crazy_linker_library_view.h"
#include "crazy_linker_system.h"

using crazy::Globals;
using crazy::Error;
using crazy::SearchPathList;
using crazy::ScopedGlobalLock;
using crazy::LibraryView;

//
// crazy_context_t
//

struct crazy_context_t {
 public:
  crazy_context_t()
      : load_address(0),
        file_offset(0),
        error(),
        search_paths(),
        java_vm(NULL),
        minimum_jni_version(0) {
    ResetSearchPaths();
  }

  void ResetSearchPaths();

  size_t load_address;
  size_t file_offset;
  Error error;
  SearchPathList search_paths;
  void* java_vm;
  int minimum_jni_version;
};

void crazy_context_t::ResetSearchPaths() {
  search_paths.ResetFromEnv("LD_LIBRARY_PATH");
}

//
// API functions
//

extern "C" {

crazy_context_t* crazy_context_create(void) { return new crazy_context_t(); }

const char* crazy_context_get_error(crazy_context_t* context) {
  const char* error = context->error.c_str();
  return (error[0] != '\0') ? error : NULL;
}

// Clear error in a given context.
void crazy_context_clear_error(crazy_context_t* context) {
  context->error = "";
}

void crazy_context_set_load_address(crazy_context_t* context,
                                    size_t load_address) {
  context->load_address = load_address;
}

size_t crazy_context_get_load_address(crazy_context_t* context) {
  return context->load_address;
}

void crazy_context_set_file_offset(crazy_context_t* context,
                                   size_t file_offset) {
  context->file_offset = file_offset;
}

size_t crazy_context_get_file_offset(crazy_context_t* context) {
  return context->file_offset;
}

crazy_status_t crazy_context_add_search_path(crazy_context_t* context,
                                             const char* file_path) {
  context->search_paths.AddPaths(file_path);
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_context_add_search_path_for_address(
    crazy_context_t* context,
    void* address) {
  uintptr_t load_address;
  char path[512];
  char* p;

  if (crazy::FindElfBinaryForAddress(
          address, &load_address, path, sizeof(path)) &&
      (p = strrchr(path, '/')) != NULL && p[1]) {
    *p = '\0';
    return crazy_context_add_search_path(context, path);
  }

  context->error.Format("Could not find ELF binary at address @%p", address);
  return CRAZY_STATUS_FAILURE;
}

void crazy_context_reset_search_paths(crazy_context_t* context) {
  context->ResetSearchPaths();
}

void crazy_context_set_java_vm(crazy_context_t* context,
                               void* java_vm,
                               int minimum_jni_version) {
  context->java_vm = java_vm;
  context->minimum_jni_version = minimum_jni_version;
}

void crazy_context_get_java_vm(crazy_context_t* context,
                               void** java_vm,
                               int* minimum_jni_version) {
  *java_vm = context->java_vm;
  *minimum_jni_version = context->minimum_jni_version;
}

void crazy_context_destroy(crazy_context_t* context) { delete context; }

crazy_status_t crazy_library_open(crazy_library_t** library,
                                  const char* lib_name,
                                  crazy_context_t* context) {
  ScopedGlobalLock lock;
  LibraryView* wrap =
      crazy::Globals::GetLibraries()->LoadLibrary(lib_name,
                                                  RTLD_NOW,
                                                  context->load_address,
                                                  context->file_offset,
                                                  &context->search_paths,
                                                  &context->error);
  if (!wrap)
    return CRAZY_STATUS_FAILURE;

  if (context->java_vm != NULL && wrap->IsCrazy()) {
    crazy::SharedLibrary* lib = wrap->GetCrazy();
    if (!lib->SetJavaVM(
             context->java_vm, context->minimum_jni_version, &context->error)) {
      crazy::Globals::GetLibraries()->UnloadLibrary(wrap);
      return CRAZY_STATUS_FAILURE;
    }
  }

  *library = reinterpret_cast<crazy_library_t*>(wrap);
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_get_info(crazy_library_t* library,
                                      crazy_context_t* context,
                                      crazy_library_info_t* info) {
  if (!library) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);
  if (!wrap->GetInfo(&info->load_address,
                     &info->load_size,
                     &info->relro_start,
                     &info->relro_size,
                     &context->error)) {
    return CRAZY_STATUS_FAILURE;
  }

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_system_can_share_relro(void) {
  crazy::AshmemRegion region;
  if (!region.Allocate(PAGE_SIZE, NULL) ||
      !region.SetProtectionFlags(PROT_READ) ||
      !crazy::AshmemRegion::CheckFileDescriptorIsReadOnly(region.fd()))
    return CRAZY_STATUS_FAILURE;

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_create_shared_relro(crazy_library_t* library,
                                                 crazy_context_t* context,
                                                 size_t load_address,
                                                 size_t* relro_start,
                                                 size_t* relro_size,
                                                 int* relro_fd) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  if (!library || !wrap->IsCrazy()) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  crazy::SharedLibrary* lib = wrap->GetCrazy();
  if (!lib->CreateSharedRelro(
           load_address, relro_start, relro_size, relro_fd, &context->error))
    return CRAZY_STATUS_FAILURE;

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_use_shared_relro(crazy_library_t* library,
                                              crazy_context_t* context,
                                              size_t relro_start,
                                              size_t relro_size,
                                              int relro_fd) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  if (!library || !wrap->IsCrazy()) {
    context->error = "Invalid library file handle";
    return CRAZY_STATUS_FAILURE;
  }

  crazy::SharedLibrary* lib = wrap->GetCrazy();
  if (!lib->UseSharedRelro(relro_start, relro_size, relro_fd, &context->error))
    return CRAZY_STATUS_FAILURE;

  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_find_by_name(const char* library_name,
                                          crazy_library_t** library) {
  {
    ScopedGlobalLock lock;
    LibraryView* wrap =
        Globals::GetLibraries()->FindLibraryByName(library_name);
    if (!wrap)
      return CRAZY_STATUS_FAILURE;

    wrap->AddRef();
    *library = reinterpret_cast<crazy_library_t*>(wrap);
  }
  return CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_library_find_symbol(crazy_library_t* library,
                                         const char* symbol_name,
                                         void** symbol_address) {
  LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

  // TODO(digit): Handle NULL symbols properly.
  *symbol_address = wrap->LookupSymbol(symbol_name);
  return (*symbol_address == NULL) ? CRAZY_STATUS_FAILURE
                                   : CRAZY_STATUS_SUCCESS;
}

crazy_status_t crazy_linker_find_symbol(const char* symbol_name,
                                        void** symbol_address) {
  // TODO(digit): Implement this.
  return CRAZY_STATUS_FAILURE;
}

crazy_status_t crazy_library_find_from_address(void* address,
                                               crazy_library_t** library) {
  {
    ScopedGlobalLock lock;
    LibraryView* wrap = Globals::GetLibraries()->FindLibraryForAddress(address);
    if (!wrap)
      return CRAZY_STATUS_FAILURE;

    wrap->AddRef();

    *library = reinterpret_cast<crazy_library_t*>(wrap);
    return CRAZY_STATUS_SUCCESS;
  }
}

void crazy_library_close(crazy_library_t* library) {
  if (library) {
    ScopedGlobalLock lock;
    LibraryView* wrap = reinterpret_cast<LibraryView*>(library);

    Globals::GetLibraries()->UnloadLibrary(wrap);
  }
}

}  // extern "C"