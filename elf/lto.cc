// This file handles the linker plugin to support LTO (Link-Time
// Optimization).
//
// LTO is a technique to do whole-program optimization to a program. Since
// a linker sees the whole program as opposed to a single compilation
// unit, it in theory can do some optimizations that cannot be done in the
// usual separate compilation model. For example, LTO should be able to
// inline functions that are defined in other compilation unit.
//
// In GCC and Clang, all you have to do to enable LTO is adding the
// `-flto` flag to the compiler and the linker command lines. If `-flto`
// is given, the compiler generates a file that contains not machine code
// but the compiler's IR (intermediate representation). In GCC, the output
// is an ELF file which wraps GCC's IR. In LLVM, it's not even an ELF file
// but just a raw LLVM IR file.
//
// Here is what we have to do if at least one input file is not a usual
// ELF file but an IR object file:
//
//  1. Read symbols both from usual ELF files and from IR object files and
//     resolve symbols as usual.
//
//  2. Pass all IR objects to the compiler backend. The compiler backend
//     compiles the IRs and returns a few big ELF object files as a
//     result.
//
//  3. Parse the returned ELF files and overwrite IR object symbols with
//     the returned ones, discarding IR object files.
//
//  4. Continue the rest of the linking process as usual.
//
// When gcc or clang inovkes ld, they pass `-plugin linker-plugin.so` to
// the linker. The given .so file provides a way to call the compiler
// backend.
//
// The linker plugin API is documented at
// https://gcc.gnu.org/wiki/whopr/driver, though the document is a bit
// outdated.
//
// Frankly, the linker plugin API is peculiar and is not very easy to use.
// For some reason, the API functions don't return the result of a
// function call as a return value but instead calls other function with
// the result as its argument to "return" the result.
//
// For example, the first thing you need to do after dlopen()'ing a linker
// plugin .so is to call `onload` function with a list of callback
// functions. `onload` calls callbacks to notify about the pointers to
// other functions the linker plugin provides. I don't know why `onload`
// can't just return a list of functions or why the linker plugin can't
// define not only `onload` but other functions, but that's how it works.
//
// Here is the steps to use the linker plugin:
//
//  1. dlopen() the linker plugin .so and call `onload` to obtain pointers
//     to other functions provided by the plugin.
//
//  2. Call `claim_file_hook` with an IR object file to read its symbol
//     table. `claim_file_hook` calls the `add_symbols` callback to
//     "return" a list of symbols.
//
//  3. `claim_file_hook` returns LDPT_OK only when the plugin wants to
//     handle a given file. Since we pass only IR object files to the
//     plugin in mold, it always returns LDPT_OK in our case.
//
//  4. Once we made a decision as to which object file to include into the
//     output file, we call `all_symbols_read_hook` to compile IR objects
//     into a few big ELF files. That function calls the `get_symbols`
//     callback to ask us about the symbol resolution results. (The
//     compiler backend needs to know whether an undefined symbol in an IR
//     object was resolved to a regular object file or a shared object to
//     do whole program optimization, for example.)
//
//  5. `all_symbols_read_hook` "returns" the result by calling the
//     `add_input_file` callback. The callback is called with a path to an
//     LTO'ed ELF file. We parse that ELF file and override symbols
//     defined by IR objects with the ELF file's ones.
//
//  6. Lastly, we call `cleanup_hook` to remove temporary files created by
//     the compiler backend.

#include "mold.h"
#include "../lto.h"

#include <cstdarg>
#include <dlfcn.h>
#include <fcntl.h>
#include <sstream>

#if 0
# define LOG std::cerr
#else
# define LOG std::ostringstream()
#endif

namespace mold::elf {

// Global variables
// We store LTO-related information to global variables,
// as the LTO plugin is not thread-safe by design anyway.

template <typename E> static Context<E> *gctx;
static int phase = 0;
static void *dlopen_handle;
static std::vector<PluginSymbol> plugin_symbols;
static ClaimFileHandler *claim_file_hook;
static AllSymbolsReadHandler *all_symbols_read_hook;
static CleanupHandler *cleanup_hook;

// Event handlers

static PluginStatus message(int level, const char *fmt, ...) {
  LOG << "message\n";
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  return LDPS_OK;
}

template <typename E>
static PluginStatus register_claim_file_hook(ClaimFileHandler fn) {
  LOG << "register_claim_file_hook\n";
  claim_file_hook = fn;
  return LDPS_OK;
}

template <typename E>
static PluginStatus
register_all_symbols_read_hook(AllSymbolsReadHandler fn) {
  LOG << "register_all_symbols_read_hook\n";
  all_symbols_read_hook = fn;
  return LDPS_OK;
}

template <typename E>
static PluginStatus register_cleanup_hook(CleanupHandler fn) {
  LOG << "register_cleanup_hook\n";
  cleanup_hook = fn;
  return LDPS_OK;
}

static PluginStatus
add_symbols(void *handle, int nsyms, const PluginSymbol *psyms) {
  LOG << "add_symbols: " << nsyms << "\n";
  assert(phase == 1);
  plugin_symbols = {psyms, psyms + nsyms};
  return LDPS_OK;
}

template <typename E>
static PluginStatus add_input_file(const char *path) {
  LOG << "add_input_file: " << path << "\n";

  Context<E> &ctx = *gctx<E>;
  static i64 file_priority = 100;

  MappedFile<Context<E>> *mf = MappedFile<Context<E>>::must_open(ctx, path);

  ObjectFile<E> *file = ObjectFile<E>::create(ctx, mf, "", false);
  ctx.obj_pool.push_back(std::unique_ptr<ObjectFile<E>>(file));
  ctx.objs.push_back(file);

  file->priority = file_priority++;
  file->is_alive = true;
  file->parse(ctx);
  file->resolve_symbols(ctx);
  return LDPS_OK;
}

static PluginStatus
get_input_file(const void *handle, struct PluginInputFile *file) {
  LOG << "get_input_file\n";
  return LDPS_OK;
}

template <typename E>
static PluginStatus release_input_file(const void *handle) {
  LOG << "release_input_file\n";
  return LDPS_OK;
}

static PluginStatus add_input_library(const char *path) {
  LOG << "add_input_library\n";
  return LDPS_OK;
}

static PluginStatus set_extra_library_path(const char *path) {
  LOG << "set_extra_library_path\n";
  return LDPS_OK;
}

template <typename E>
static PluginStatus get_view(const void *handle, const void **view) {
  LOG << "get_view\n";

  ObjectFile<E> &file = *(ObjectFile<E> *)handle;
  *view = (void *)file.mf->data;
  return LDPS_OK;
}

static PluginStatus
get_input_section_count(const void *handle, int *count) {
  LOG << "get_input_section_count\n";
  return LDPS_OK;
}

static PluginStatus
get_input_section_type(const PluginSection section, int *type) {
  LOG << "get_input_section_type\n";
  return LDPS_OK;
}

static PluginStatus
get_input_section_name(const PluginSection section,
                       char **section_name) {
  LOG << "get_input_section_name\n";
  return LDPS_OK;
}

static PluginStatus
get_input_section_contents(const PluginSection section,
                           const char **section_contents,
		           size_t *len) {
  LOG << "get_input_section_contents\n";
  return LDPS_OK;
}

static PluginStatus
update_section_order(const PluginSection *section_list,
		     int num_sections) {
  LOG << "update_section_order\n";
  return LDPS_OK;
}

static PluginStatus allow_section_ordering() {
  LOG << "allow_section_ordering\n";
  return LDPS_OK;
}

static PluginStatus
get_symbols_v1(const void *handle, int nsyms, PluginSymbol *psyms) {
  unreachable();
}

template <typename E>
static PluginStatus
get_symbols(const void *handle, int nsyms, PluginSymbol *psyms) {
  ObjectFile<E> &file = *(ObjectFile<E> *)handle;

  if (!file.is_alive) {
    for (int i = 0; i < nsyms; i++)
      psyms[i].resolution = LDPR_PREEMPTED_REG;
    return LDPS_NO_SYMS;
  }

  auto get_resolution = [&](PluginSymbol &psym, Symbol<E> &sym) -> int {
    if (!sym.file)
      return LDPR_UNDEF;
    if (sym.file == &file)
      return LDPR_PREVAILING_DEF;
    if (sym.file->is_dso())
      return LDPR_RESOLVED_DYN;
    if (((ObjectFile<E> *)sym.file)->is_lto_obj)
      return LDPR_RESOLVED_IR;
    return LDPR_RESOLVED_EXEC;
  };

  for (i64 i = 0; i < nsyms; i++) {
    PluginSymbol &psym = psyms[i];
    Symbol<E> &sym = *file.symbols[i + 1];
    psym.resolution = get_resolution(psym, sym);
  }
  return LDPS_OK;
}

template <typename E>
static PluginStatus
get_symbols_v2(const void *handle, int nsyms, PluginSymbol *psyms) {
  LOG << "get_symbols_v2\n";
  PluginStatus st = get_symbols<E>(handle, nsyms, psyms);
  return (st == LDPS_NO_SYMS) ? LDPS_OK : st;
}

template <typename E>
static PluginStatus
get_symbols_v3(const void *handle, int nsyms, PluginSymbol *psyms) {
  LOG << "get_symbols_v3\n";
  return get_symbols<E>(handle, nsyms, psyms);
}

static PluginStatus allow_unique_segment_for_sections() {
  LOG << "allow_unique_segment_for_sections\n";
  return LDPS_OK;
}

static PluginStatus
unique_segment_for_sections(const char *segment_name,
			    uint64_t flags,
			    uint64_t align,
			    const PluginSection *section_list,
			    int num_sections) {
  LOG << "unique_segment_for_sections\n";
  return LDPS_OK;
}

static PluginStatus
get_input_section_alignment(const PluginSection section,
                            int *addralign) {
  LOG << "get_input_section_alignment\n";
  return LDPS_OK;
}

static PluginStatus
get_input_section_size(const PluginSection section, uint64_t *size) {
  LOG << "get_input_section_size\n";
  return LDPS_OK;
}

template <typename E>
static PluginStatus
register_new_input_hook(NewInputHandler fn) {
  LOG << "register_new_input_hook\n";
  return LDPS_OK;
}

static PluginStatus
get_wrap_symbols(uint64_t *num_symbols, const char ***wrap_symbols) {
  LOG << "get_wrap_symbols\n";
  return LDPS_OK;
}

template <typename E>
static void load_plugin(Context<E> &ctx) {
  assert(phase == 0);
  phase = 1;
  gctx<E> = &ctx;

  dlopen_handle = dlopen(ctx.arg.plugin.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!dlopen_handle)
    Fatal(ctx) << "could not open plugin file: " << dlerror();

  OnloadFn *onload = (OnloadFn *)dlsym(dlopen_handle, "onload");
  if (!onload)
    Fatal(ctx) << "failed to load plugin " << ctx.arg.plugin << ": "
               << dlerror();

  auto save = [&](std::string_view str) {
    return save_string(ctx, std::string(str).c_str()).data();
  };

  std::vector<PluginTagValue> tv;
  tv.emplace_back(LDPT_MESSAGE, message);

  if (ctx.arg.shared)
    tv.emplace_back(LDPT_LINKER_OUTPUT, LDPO_DYN);
  else if (ctx.arg.pie)
    tv.emplace_back(LDPT_LINKER_OUTPUT, LDPO_PIE);
  else
    tv.emplace_back(LDPT_LINKER_OUTPUT, LDPO_EXEC);

  for (std::string_view opt : ctx.arg.plugin_opt)
    tv.emplace_back(LDPT_OPTION, save(opt));

  tv.emplace_back(LDPT_REGISTER_CLAIM_FILE_HOOK, register_claim_file_hook<E>);
  tv.emplace_back(LDPT_REGISTER_ALL_SYMBOLS_READ_HOOK,
                  register_all_symbols_read_hook<E>);
  tv.emplace_back(LDPT_REGISTER_CLEANUP_HOOK, register_cleanup_hook<E>);
  tv.emplace_back(LDPT_ADD_SYMBOLS, add_symbols);
  tv.emplace_back(LDPT_GET_SYMBOLS, get_symbols_v1);
  tv.emplace_back(LDPT_ADD_INPUT_FILE, add_input_file<E>);
  tv.emplace_back(LDPT_GET_INPUT_FILE, get_input_file);
  tv.emplace_back(LDPT_RELEASE_INPUT_FILE, release_input_file<E>);
  tv.emplace_back(LDPT_ADD_INPUT_LIBRARY, add_input_library);
  tv.emplace_back(LDPT_OUTPUT_NAME, save(ctx.arg.output));
  tv.emplace_back(LDPT_SET_EXTRA_LIBRARY_PATH, set_extra_library_path);
  tv.emplace_back(LDPT_GET_VIEW, get_view<E>);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_COUNT, get_input_section_count);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_TYPE, get_input_section_type);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_NAME, get_input_section_name);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_CONTENTS, get_input_section_contents);
  tv.emplace_back(LDPT_UPDATE_SECTION_ORDER, update_section_order);
  tv.emplace_back(LDPT_ALLOW_SECTION_ORDERING, allow_section_ordering);
  tv.emplace_back(LDPT_GET_SYMBOLS_V2, get_symbols_v2<E>);
  tv.emplace_back(LDPT_ALLOW_UNIQUE_SEGMENT_FOR_SECTIONS,
                  allow_unique_segment_for_sections);
  tv.emplace_back(LDPT_UNIQUE_SEGMENT_FOR_SECTIONS, unique_segment_for_sections);
  tv.emplace_back(LDPT_GET_SYMBOLS_V3, get_symbols_v3<E>);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_ALIGNMENT, get_input_section_alignment);
  tv.emplace_back(LDPT_GET_INPUT_SECTION_SIZE, get_input_section_size);
  tv.emplace_back(LDPT_REGISTER_NEW_INPUT_HOOK, register_new_input_hook<E>);
  tv.emplace_back(LDPT_GET_WRAP_SYMBOLS, get_wrap_symbols);
  tv.emplace_back(LDPT_NULL, 0);

  onload(tv.data());
}

template <typename E>
static ElfSym<E> to_elf_sym(PluginSymbol &psym) {
  ElfSym<E> esym = {};

  switch (psym.def) {
  case LDPK_DEF:
    esym.st_shndx = SHN_ABS;
    break;
  case LDPK_WEAKDEF:
    esym.st_shndx = SHN_ABS;
    esym.st_bind = STB_WEAK;
    break;
  case LDPK_UNDEF:
    esym.st_shndx = SHN_UNDEF;
    break;
  case LDPK_WEAKUNDEF:
    esym.st_shndx = SHN_UNDEF;
    esym.st_bind = STB_WEAK;
    break;
  case LDPK_COMMON:
    esym.st_shndx = SHN_COMMON;
    break;
  }

  switch (psym.symbol_type) {
  case LDST_UNKNOWN:
    break;
  case LDST_FUNCTION:
    esym.st_type = STT_FUNC;
    break;
  case LDST_VARIABLE:
    esym.st_type = STT_OBJECT;
    break;
  };

  switch (psym.visibility) {
  case LDPV_DEFAULT:
    break;
  case LDPV_PROTECTED:
    esym.st_visibility = STV_PROTECTED;
    break;
  case LDPV_INTERNAL:
    esym.st_visibility = STV_INTERNAL;
    break;
  case LDPV_HIDDEN:
    esym.st_visibility = STV_HIDDEN;
    break;
  }

  esym.st_size = psym.size;
  return esym;
}

template <typename E>
ObjectFile<E> *read_lto_object(Context<E> &ctx, MappedFile<Context<E>> *mf) {
  LOG << "read_lto_object: " << mf->name << "\n";

  if (ctx.arg.plugin.empty())
    Fatal(ctx) << mf->name << ": don't know how to handle this LTO object file"
               << " because no -plugin option was given";

  // dlopen the linker plugin file
  static std::once_flag flag;
  std::call_once(flag, [&] { load_plugin(ctx); });

  // Create mold's object instance
  ObjectFile<E> *obj = new ObjectFile<E>;
  obj->symbols.push_back(new Symbol<E>);
  obj->first_global = 1;
  obj->is_lto_obj = true;
  obj->mf = mf;

  // Create plugin's object instance
  PluginInputFile *file = new PluginInputFile;
  file->name = save_string(ctx, mf->parent ? mf->parent->name : mf->name).data();
  file->fd = open(file->name, O_RDONLY);
  if (file->fd == -1)
    Fatal(ctx) << "cannot open " << file->name << ": " << errno_string();
  file->offset = mf->get_offset();
  file->filesize = mf->size;
  file->handle = (void *)obj;

  LOG << "read_lto_symbols: "<< mf->name << "\n";

  // claim_file_hook() calls add_symbols() which initializes `plugin_symbols`
  int claimed = false;
  PluginStatus st = claim_file_hook(file, &claimed);
  assert(claimed);

  // Initialize object symbols
  std::vector<ElfSym<E>> *esyms = new std::vector<ElfSym<E>>(1);

  for (PluginSymbol &psym : plugin_symbols) {
    esyms->push_back(to_elf_sym<E>(psym));
    obj->symbols.push_back(get_symbol(ctx, save_string(ctx, psym.name)));
  }

  obj->elf_syms = *esyms;
  obj->sym_fragments.resize(esyms->size());
  obj->symvers.resize(esyms->size());
  plugin_symbols.clear();
  return obj;
}

// Entry point
template <typename E>
void do_lto(Context<E> &ctx) {
  Timer t(ctx, "do_lto");

  assert(phase == 1);
  phase = 2;

  // all_symbols_read_hook() calls add_input_file() and add_input_library()
  LOG << "all symbols read\n";
  all_symbols_read_hook();

  // Remove IR object files
  for (ObjectFile<E> *file : ctx.objs)
    if (file->is_lto_obj)
      file->is_alive = false;

  std::erase_if(ctx.objs, [](ObjectFile<E> *file) { return file->is_lto_obj; });
}

template <typename E>
void lto_cleanup(Context<E> &ctx) {
  Timer t(ctx, "lto_cleanup");

  if (cleanup_hook)
    cleanup_hook();
}

#define INSTANTIATE(E)                                                  \
  template ObjectFile<E> *                                              \
    read_lto_object(Context<E> &, MappedFile<Context<E>> *);            \
  template void do_lto(Context<E> &);                                   \
  template void lto_cleanup(Context<E> &)

INSTANTIATE(X86_64);
INSTANTIATE(I386);
INSTANTIATE(ARM64);
INSTANTIATE(RISCV64);

} // namespace mold::elf
