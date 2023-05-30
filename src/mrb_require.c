/*
** require.c - require
**
** See Copyright Notice in mruby.h
*/

#include "mruby.h"
#include "mruby/data.h"
#include "mruby/string.h"
#include "mruby/dump.h"
#include "mruby/proc.h"
#include "mruby/compile.h"
#include "mruby/variable.h"
#include "mruby/array.h"
#include "mruby/numeric.h"
#include "mruby/irep.h"
#include "mruby/opcode.h"
#include "mruby/class.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#if defined(_MSC_VER) || defined(__MINGW32__)
#ifndef PATH_MAX
# define PATH_MAX MAX_PATH
#endif
#define strdup(x) _strdup(x)
#else
#include <sys/param.h>
#include <unistd.h>
#include <libgen.h>
#include <dlfcn.h>
#endif

#ifndef RSTRING_CSTR
static const char*
mrb_string_cstr(mrb_state *mrb, mrb_value s)
{
  return mrb_string_value_cstr(mrb, &s);
}
#define RSTRING_CSTR(mrb,s)  mrb_string_cstr(mrb, s)
#endif

/* We can't use MRUBY_RELEASE_NO to determine if byte code implementation is old */
#ifdef MKOP_A
#define USE_MRUBY_OLD_BYTE_CODE
#endif

#ifndef MRB_PROC_TARGET_CLASS
# define MRB_PROC_TARGET_CLASS(p, c) p->target_class = c
#endif

#ifdef _WIN32
#include <windows.h>
#define dlopen(x,y) (void*)LoadLibrary(x)
#define dlsym(x,y) (void*)GetProcAddress((HMODULE)x,y)
#define dlclose(x) FreeLibrary((HMODULE)x)
const char* dlerror() {
  DWORD err = (int) GetLastError();
  static char buf[256];
  if (err == 0) return NULL;
  FormatMessage(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    err,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    buf,
    sizeof buf,
    NULL);
  return buf;
}

char*
realpath(const char *path, char *resolved_path) {
  if (!resolved_path)
    resolved_path = malloc(PATH_MAX + 1);
  if (!resolved_path) return NULL;
  GetFullPathNameA(path, PATH_MAX, resolved_path, NULL);
  return resolved_path;
}
#else
#include <dlfcn.h>
#endif

#if defined(_WIN32)
# define ENV_SEP ';'
#else
# define ENV_SEP ':'
#endif

#define E_LOAD_ERROR (mrb_class_get(mrb, "LoadError"))

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#if 0
# include <stdarg.h>
# define debug(s,...) printf("%s:%d " s, __FILE__, __LINE__,__VA_ARGS__)
#else
# define debug(...) ((void)0)
#endif

static void
mrb_load_fail(mrb_state *mrb, mrb_value path, const char *err)
{
  mrb_value mesg, exc;

  mesg = mrb_str_new_cstr(mrb, err);
  mrb_str_cat_lit(mrb, mesg, " -- ");
  mrb_str_cat_str(mrb, mesg, path);
  exc = mrb_funcall(mrb, mrb_obj_value(E_LOAD_ERROR), "new", 1, mesg);
  mrb_iv_set(mrb, exc, mrb_intern_lit(mrb, "path"), path);

  mrb_exc_raise(mrb, exc);
}

static mrb_value
get_loaded_features(mrb_state *mrb, mrb_bool replace_new)
{
  mrb_value ary = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\""));
  ary = mrb_check_array_type(mrb, ary);

  if (mrb_nil_p(ary) && replace_new) {
    ary = mrb_ary_new(mrb);
    mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$\""), ary);
  }

  return ary;
}

static mrb_value
envpath_to_mrb_ary(mrb_state *mrb, const char *name)
{
  int i;
  int envlen;
  mrb_value ary = mrb_ary_new(mrb);

  char *env= getenv(name);
  if (env == NULL) {
    return ary;
  }

  envlen = strlen(env);
  for (i = 0; i < envlen; i++) {
    char *ptr = env + i;
    char *end = strchr(ptr, ENV_SEP);
    int len;
    if (end == NULL) {
      end = env + envlen;
    }
    len = end - ptr;
    mrb_ary_push(mrb, ary, mrb_str_new(mrb, ptr, len));
    i += len;
  }

  return ary;
}


static mrb_value
find_file_check(mrb_state *mrb, mrb_value path, mrb_value fname, mrb_value ext)
{
  FILE *fp;
  char fpath[MAXPATHLEN];
  mrb_value filepath = mrb_str_dup(mrb, path);
#ifdef _WIN32
  if (RSTRING_PTR(fname)[1] == ':') {
#else
  if (RSTRING_PTR(fname)[0] == '/') {
#endif
    /* when absolute path */
    mrb_funcall(mrb, filepath, "replace", 1, fname);
  } else {
    mrb_str_cat2(mrb, filepath, "/");
    mrb_str_buf_append(mrb, filepath, fname);
  }

  if (!mrb_string_p(filepath)) {
    return mrb_nil_value();
  }
  if (mrb_string_p(ext)) {
    mrb_str_buf_append(mrb, filepath, ext);
  }
  debug("filepath: %s\n", RSTRING_PTR(filepath));

  if (realpath(RSTRING_CSTR(mrb, filepath), fpath) == NULL) {
    return mrb_nil_value();
  }
  debug("fpath: %s\n", fpath);

#if defined(S_ISDIR)
  {
    struct stat st;
    if (stat(fpath, &st) || S_ISDIR(st.st_mode)) {
      return mrb_nil_value();
    }
  }
#endif

  fp = fopen(fpath, "r");
  if (fp == NULL) {
    return mrb_nil_value();
  }
  fclose(fp);

  return mrb_str_new_cstr(mrb, fpath);
}

static mrb_value
find_file(mrb_state *mrb, mrb_value filename, int comp)
{
  const char *ext, *ptr, *tmp;
  mrb_value exts;
  int i, j;

  const char *fname = RSTRING_CSTR(mrb, filename);
  mrb_value filepath = mrb_nil_value();
  mrb_value load_path = mrb_obj_dup(mrb, mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$:")));
  load_path = mrb_check_array_type(mrb, load_path);

  if(mrb_nil_p(load_path)) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "invalid $:");
    return mrb_undef_value();
  }

  tmp = ptr = fname;
  while (tmp) {
    if ((tmp = strchr(ptr, '/')) || (tmp = strchr(ptr, '\\'))) {
      ptr = tmp + 1;
    }
  }

  ext = strrchr(ptr, '.');
  exts = mrb_ary_new(mrb);
  if (ext == NULL && comp) {
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".rb"));
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".mrb"));
    mrb_ary_push(mrb, exts, mrb_str_new_cstr(mrb, ".so"));
  } else {
    mrb_ary_push(mrb, exts, mrb_nil_value());
  }

  /* when a filename start with '.', $: = ['.'] */
  if (*fname == '.') {
    load_path = mrb_ary_new(mrb);
    mrb_ary_push(mrb, load_path, mrb_str_new_cstr(mrb, "."));
  }

  for (i = 0; i < RARRAY_LEN(load_path); i++) {
    for (j = 0; j < RARRAY_LEN(exts); j++) {
      filepath = find_file_check(
        mrb,
        mrb_ary_entry(load_path, i),
        filename,
        mrb_ary_entry(exts, j));
      if (!mrb_nil_p(filepath)) {
        return filepath;
      }
    }
  }

  mrb_load_fail(mrb, filename, "cannot load such file");
  return mrb_nil_value();
}

#ifdef USE_MRUBY_OLD_BYTE_CODE
static void
replace_stop_with_return(mrb_state *mrb, mrb_irep *irep)
{
  if (irep->iseq[irep->ilen - 1] == MKOP_A(OP_STOP, 0)) {
    if (irep->flags == MRB_ISEQ_NO_FREE) {
      mrb_code* iseq = (mrb_code *)mrb_malloc(mrb, (irep->ilen + 1) * sizeof(mrb_code));
      memcpy(iseq, irep->iseq, irep->ilen * sizeof(mrb_code));
      irep->iseq = iseq;
      irep->flags &= ~MRB_ISEQ_NO_FREE;
    } else {
      irep->iseq = (mrb_code *)mrb_realloc(mrb, irep->iseq, (irep->ilen + 1) * sizeof(mrb_code));
    }
    irep->iseq[irep->ilen - 1] = MKOP_A(OP_LOADNIL, 0);
    irep->iseq[irep->ilen] = MKOP_AB(OP_RETURN, 0, OP_R_NORMAL);
    irep->ilen++;
  }
}
#endif

static mrb_value
load_mrb_file(mrb_state *mrb, mrb_value filepath, struct RClass *wrap)
{
  const char *fpath = RSTRING_CSTR(mrb, filepath);
  int ai;
  FILE *fp;
  mrb_irep *irep;

  fp = fopen(fpath, "rb");
  if (fp == NULL) {
    mrb_load_fail(
      mrb,
      mrb_str_new_cstr(mrb, fpath),
      "cannot load such file"
    );
    return mrb_nil_value(); // not reached
  }

  ai = mrb_gc_arena_save(mrb);

  irep = mrb_read_irep_file(mrb, fp);
  fclose(fp);

  mrb_gc_arena_restore(mrb, ai);

  if (irep) {
    struct RProc *proc;
    /*
    size_t i;
    for (i = sirep; i < mrb->irep_len; i++) {
      mrb->irep[i]->filename = mrb_string_value_ptr(mrb, filepath);
    }
    */

#ifdef USE_MRUBY_OLD_BYTE_CODE
    replace_stop_with_return(mrb, irep);
#endif
    proc = mrb_proc_new(mrb, irep);
    MRB_PROC_SET_TARGET_CLASS(proc, wrap);
    proc->flags |= MRB_PROC_SCOPE;
    proc->c = mrb->proc_class;

    return mrb_obj_value(proc);
  } else if (mrb->exc) {
    // fail to load
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  }

  return mrb_nil_value();
}

#if MRUBY_RELEASE_NO >= 30100
static void
activate_gem(mrb_state *mrb, void (*geminit)(mrb_state *mrb))
{
  geminit(mrb);
}
#else
static mrb_value
activate_gem_body2(mrb_state *mrb, mrb_value ud)
{
  void (*geminit)(mrb_state *) = (void (*)(mrb_state *))mrb_cptr(ud);
  geminit(mrb);
  return mrb_nil_value();
}

static mrb_value
activate_gem_body1(mrb_state *mrb, mrb_value ud)
{
  struct RProc *proc = mrb_proc_new_cfunc(mrb, activate_gem_body2);
  mrb_yield_with_class(mrb, mrb_obj_value(proc), 0, NULL, ud, mrb->object_class);
  return mrb_nil_value();
}

static void
activate_gem(mrb_state *mrb, void (*geminit)(mrb_state *mrb))
{
  if (mrb->c->ci == mrb->c->cibase) {
    geminit(mrb);
  } else {
    struct RProc *proc = mrb_proc_new_cfunc(mrb, activate_gem_body1);
    int cioff = mrb->c->ci - mrb->c->cibase;
    mrb_yield_with_class(mrb, mrb_obj_value(proc), 0, NULL, mrb_cptr_value(mrb, geminit), mrb->object_class);
    mrb->c->ci = mrb->c->cibase + cioff;
  }
}
#endif

static mrb_value
load_so_file(mrb_state *mrb, mrb_value filepath)
{
  char entry[PATH_MAX] = {0}, *ptr, *top, *tmp;
  typedef void (*fn_mrb_gem_init)(mrb_state *mrb);
  fn_mrb_gem_init fn;
  void * handle = dlopen(RSTRING_CSTR(mrb, filepath), RTLD_LAZY|RTLD_GLOBAL);
  if (!handle) {
    mrb_raise(mrb, E_RUNTIME_ERROR, dlerror());
  }
  dlerror(); // clear last error

  tmp = top = ptr = strdup(RSTRING_CSTR(mrb, filepath));
  while (tmp) {
    if ((tmp = strchr(ptr, '/')) || (tmp = strchr(ptr, '\\'))) {
      ptr = tmp + 1;
    }
  }

  tmp = strrchr(ptr, '.');
  if (tmp) *tmp = 0;
  tmp = ptr;
  while (*tmp) {
    if (*tmp == '-') *tmp = '_';
    tmp++;
  }
  snprintf(entry, sizeof(entry)-1, "GENERATED_TMP_mrb_%s_gem_init", ptr);
  fn = (fn_mrb_gem_init) dlsym(handle, entry);
  free(top);
  if (!fn) {
      mrb_load_fail(mrb, filepath, "cannot load such file");
  }

  if (fn != NULL) {
    mrb->c->ci->mid = 0;
    int ai = mrb_gc_arena_save(mrb);
    activate_gem(mrb, fn);
    mrb_gc_arena_restore(mrb, ai);
    if (mrb->exc) {
      mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
    }
  }
  dlerror(); // clear last error

  return mrb_true_value();
}

static void
unload_so_file(mrb_state *mrb, mrb_value filepath)
{
  char entry[PATH_MAX] = {0}, *ptr, *top, *tmp;
  typedef void (*fn_mrb_gem_final)(mrb_state *mrb);
  fn_mrb_gem_final fn;
  void * handle = dlopen(RSTRING_CSTR(mrb, filepath), RTLD_LAZY|RTLD_GLOBAL);
  if (!handle) {
    return;
  }

  tmp = top = ptr = strdup(RSTRING_CSTR(mrb, filepath));
  while (tmp) {
    if ((tmp = strchr(ptr, '/')) || (tmp = strchr(ptr, '\\'))) {
      ptr = tmp + 1;
    }
  }

  tmp = strrchr(ptr, '.');
  if (tmp) *tmp = 0;
  tmp = ptr;
  while (*tmp) {
    if (*tmp == '-') *tmp = '_';
    tmp++;
  }
  snprintf(entry, sizeof(entry)-1, "GENERATED_TMP_mrb_%s_gem_final", ptr);

  fn = (fn_mrb_gem_final) dlsym(handle, entry);
  free(top);
  if (fn == NULL) {
    return;
  }

  fn(mrb);
}

static mrb_value
load_rb_file(mrb_state *mrb, mrb_value filepath, struct RClass *wrap)
{
  FILE *fp;
  const char *fpath = RSTRING_CSTR(mrb, filepath);
  mrbc_context *mrbc_ctx;
  int ai = mrb_gc_arena_save(mrb);
  mrb_value proc;

  fp = fopen((const char*)fpath, "r");
  if (fp == NULL) {
    mrb_load_fail(mrb, filepath, "cannot load such file");
    return mrb_nil_value(); // not reached
  }

  mrbc_ctx = mrbc_context_new(mrb);
  mrbc_ctx->capture_errors = TRUE;
  mrbc_ctx->no_exec = TRUE;

  mrbc_filename(mrb, mrbc_ctx, fpath);
  proc = mrb_load_file_cxt(mrb, fp, mrbc_ctx);
  fclose(fp);

  if (mrb->exc) {
    mrb_gc_arena_restore(mrb, ai);
    mrbc_context_free(mrb, mrbc_ctx);
    mrb_exc_raise(mrb, mrb_obj_value(mrb->exc));
  } else if (mrb_undef_p(proc)) {
    mrb_gc_arena_restore(mrb, ai);
    mrbc_context_free(mrb, mrbc_ctx);
    mrb_raise(mrb, E_RUNTIME_ERROR, "parser error (maybe out of memory)");
  }

  mrb_gc_arena_restore(mrb, ai);
  mrb_gc_protect(mrb, proc);
  mrbc_context_free(mrb, mrbc_ctx);

  MRB_PROC_SET_TARGET_CLASS(mrb_proc_ptr(proc), wrap);
  mrb_proc_ptr(proc)->flags |= MRB_PROC_SCOPE;
  mrb_proc_ptr(proc)->c = mrb->proc_class;

  return proc;
}

static mrb_value
load_file(mrb_state *mrb, mrb_value filepath, struct RClass *wrap)
{
  char *ext = strrchr(RSTRING_CSTR(mrb, filepath), '.');

  if (!ext || strcmp(ext, ".rb") == 0) {
    return load_rb_file(mrb, filepath, wrap);
  } else if (strcmp(ext, ".mrb") == 0) {
    return load_mrb_file(mrb, filepath, wrap);
  } else if (strcmp(ext, ".so") == 0 ||
             strcmp(ext, ".dll") == 0 ||
             strcmp(ext, ".dylib") == 0) {
    return load_so_file(mrb, filepath);
  } else {
    return load_rb_file(mrb, filepath, wrap);
  }
}

static int
loaded_files_check(mrb_state *mrb, mrb_value filepath)
{
  mrb_value loading_files;
  mrb_value loaded_files = get_loaded_features(mrb, TRUE);
  int i;
  for (i = 0; i < RARRAY_LEN(loaded_files); i++) {
    mrb_value e = mrb_ary_entry(loaded_files, i);
    if (mrb_string_p(e) && mrb_str_cmp(mrb, e, filepath) == 0) {
      return 0;
    }
  }

  loading_files = mrb_gv_get(mrb, mrb_intern_cstr(mrb, "$\"_"));
  if (mrb_nil_p(loading_files)) {
    return 1;
  }
  for (i = 0; i < RARRAY_LEN(loading_files); i++) {
    if (mrb_str_cmp(
        mrb,
        mrb_ary_entry(loading_files, i),
        filepath) == 0) {
      return 0;
    }
  }

  return 1;
}

static mrb_value
require_load_library(mrb_state *mrb, mrb_value self)
{
  mrb_value filename, wrap, lib;
  mrb_bool for_require;

  mrb_get_args(mrb, "obo", &filename, &for_require, &wrap);
  if (mrb_type(filename) != MRB_TT_STRING) {
    mrb_raisef(mrb, E_TYPE_ERROR, "can't convert %S into String", filename);
    return mrb_nil_value();
  }

  filename = find_file(mrb, filename, (for_require ? 1 : 0));
  if (for_require && !loaded_files_check(mrb, filename)) {
    return mrb_false_value();
  }
  lib = load_file(mrb, filename, (mrb_type(wrap) == MRB_TT_MODULE ? mrb_class_ptr(wrap) : mrb->object_class));

  return mrb_assoc_new(mrb, lib, filename);
}

static mrb_value
mrb_init_load_path(mrb_state *mrb)
{
  char *env;
  mrb_value ary = envpath_to_mrb_ary(mrb, "MRBLIB");

  env = getenv("MRBGEMS_ROOT");
  if (env)
    mrb_ary_push(mrb, ary, mrb_str_new_cstr(mrb, env));
#ifdef MRBGEMS_ROOT
  else
    mrb_ary_push(mrb, ary, mrb_str_new_cstr(mrb, MRBGEMS_ROOT));
#endif

  return ary;
}

static mrb_value
mrb_load_error_path(mrb_state *mrb, mrb_value self)
{
  return mrb_iv_get(mrb, self, mrb_intern_lit(mrb, "path"));
}

#if MRUBY_RELEASE_NO >= 30000
# define CI_STACK(ci) ((ci)->stack)
#else
# define CI_STACK(ci) ((ci)[1].stackent)
#endif

static void
replace_loader_object(mrb_state *mrb)
{
  const mrb_callinfo *ci = mrb->c->ci - 1;
  if (ci < mrb->c->cibase || !ci->proc || MRB_PROC_CFUNC_P(ci->proc) || ci->proc->body.irep->nlocals < 4) {
    mrb_raise(mrb, E_RUNTIME_ERROR, "wrong caller");
  }

  CI_STACK(ci)[1] = mrb_obj_value(mrb_proc_new_cfunc(mrb, require_load_library));
}

static mrb_value
require_initialize_epilogue(mrb_state *mrb, mrb_value self)
{
  int ai = mrb_gc_arena_save(mrb);
  char *env;

  mrb_undef_method(mrb, mrb->kernel_module, "__require_initialize_epilogue__");
  replace_loader_object(mrb);

  env = getenv("MRUBY_REQUIRE");
  if (env != NULL) {
    mrb_sym mid = mrb_intern_lit(mrb, "require");
    int i, envlen;
    envlen = strlen(env);
    for (i = 0; i < envlen; i++) {
      char *ptr = env + i;
      char *end = strchr(ptr, ',');
      int len;
      mrb_value filename;
      if (end == NULL) {
        end = env + envlen;
      }
      len = end - ptr;

      filename = mrb_str_new(mrb, ptr, len);
      mrb_funcall_with_block(mrb, mrb_top_self(mrb), mid, 1, &filename, mrb_nil_value());
      mrb_gc_arena_restore(mrb, ai);
      i += len;
    }
  }

  return mrb_nil_value();
}

void
mrb_mruby_require_gem_init(mrb_state* mrb)
{
  struct RClass *krn;
  struct RClass *load_error;
  krn = mrb->kernel_module;

  mrb_define_method(mrb, krn, "__require_initialize_epilogue__", require_initialize_epilogue, MRB_ARGS_NONE());

  load_error = mrb_define_class(mrb, "LoadError", E_SCRIPT_ERROR);
  mrb_define_method(mrb, load_error, "path", mrb_load_error_path, MRB_ARGS_NONE());

  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$:"), mrb_init_load_path(mrb));
  mrb_gv_set(mrb, mrb_intern_cstr(mrb, "$\""), mrb_ary_new(mrb));
}

void
mrb_mruby_require_gem_final(mrb_state* mrb)
{
  mrb_value loaded_files = get_loaded_features(mrb, FALSE);
  if (!mrb_nil_p(loaded_files)) {
    int i;
    for (i = 0; i < RARRAY_LEN(loaded_files); i++) {
      mrb_value f = mrb_ary_entry(loaded_files, i);
      if (mrb_string_p(f)) {
        const char* ext = strrchr(RSTRING_CSTR(mrb, f), '.');
        if (ext && strcmp(ext, ".so") == 0) {
          unload_so_file(mrb, f);
        }
      }
    }
  }
}

/* vim:set et ts=2 sts=2 sw=2 tw=0: */
