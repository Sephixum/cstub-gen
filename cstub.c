#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MemoryZeroStruct(S)   memset(S, 0, sizeof(*(S)))
#define MallocArray(T, count) malloc(sizeof(T) * (count))
#define MallocRelease(ptr)    free(ptr)

#define KB(x) ((x) << 10)
#define MB(x) ((x) << 20)
#define GB(x) ((x) << 30)

#define PushArray(arena, T, count) arena_push(arena, sizeof(T) * (count), AlignOf(T))
#define PushStruct(arena, T)       PushArray(arena, T, 1)

#if defined(_MSC_VER) || defined(__clang__)
# define AlignOf(T) __alignof(T)
#elif defined(__GNUC__) || defined(__GNUG__)
# define AlignOf(T) __alignof__(T)
#else
# error AlginOf operator is not set for this compiler
#endif

#define DeferScope(begin_op, end_op) for (int _i_ = ((begin_op), 0); _i_ < 1; (end_op), ++_i_)
#define NoOp        (void)0
#define zero_struct {0}


typedef struct MemoryArena MemoryArena;
struct MemoryArena
{
  char*     data;
  uintptr_t cursor;
  size_t    capacity;
};

typedef struct TempArena TempArena;
struct TempArena
{
  MemoryArena* arena;
  uintptr_t    position;
};

static MemoryArena*
arena_alloc(size_t size)
{
  MemoryArena* result = 0;
  char* region = MallocArray(char, size);
  assert(region != 0);
  assert(size > sizeof(MemoryArena));
  result           = (MemoryArena*)region;
  result->data     = region;
  result->cursor   = (uintptr_t)(region + sizeof(MemoryArena));
  result->capacity = size - sizeof(MemoryArena);
  return(result);
}

static void
arena_release(MemoryArena* arena)
{
  MallocRelease(arena);
}

static void*
arena_push(MemoryArena* arena, size_t size, size_t align)
{
  uintptr_t cursor      = arena->cursor;
  uintptr_t mask        = (uintptr_t)align - 1;
  uintptr_t aligned     = (cursor + mask) & ~mask;
  uintptr_t end         = aligned + size;
  uintptr_t arena_begin = (uintptr_t)arena + sizeof(MemoryArena);
  uintptr_t arena_end   = arena_begin + arena->capacity;
  if (end > arena_end)
  {
    return(0);
  }
  arena->cursor = end;
  return((void*)aligned);
}

static TempArena
temp_begin(MemoryArena* arena)
{
  TempArena result = zero_struct;
  result.arena = arena;
  result.position = arena->cursor;
  return(result);
}

static void
temp_end(TempArena temp)
{
  temp.arena->cursor = temp.position;
}


typedef struct String_Slice String_Slice;
struct String_Slice
{
  const char* data;
  size_t      size;
};
#define str_slice_varg(S) (int)(S).size, (S).data
#define str_lit(S)        (String_Slice){S, sizeof(S) - 1}
static int
str_match(String_Slice a, String_Slice b)
{
  return(a.size == b.size && memcmp(a.data, b.data, a.size) == 0);
}

typedef struct Header_File Header_File;
struct Header_File
{
  char*  data;
  size_t size;
};

// NOTE(sep): Speculative allocation with commit/rollback. We don't know up
// front whether the read will succeed, so the buffer is pushed as a "stack"
// allocation via TempArena; on failure we roll the cursor back (temp_end),
// on success we simply never call temp_end and the allocation is promoted
// to a permanent one that lives in the arena for the rest of the program.
static Header_File
header_file_alloc(MemoryArena* arena, const char* path)
{
  Header_File result = zero_struct;
  FILE* file = fopen(path, "rb");
  if (!file)
  {
    return(result);
  }
  if (fseek(file, 0, SEEK_END) != 0)
  {
    fclose(file);
    return(result);
  }
  long file_size = ftell(file);
  if (file_size < 0)
  {
    fclose(file);
    return(result);
  }
  rewind(file);
  TempArena temp = temp_begin(arena);
  result.size = (size_t)file_size;
  result.data = PushArray(arena, char, result.size + 1);
  if (!result.data)
  {
    fclose(file);
    temp_end(temp);
    result.size = 0;
    return(result);
  }
  if (fread(result.data, 1, result.size, file) != result.size)
  {
    fclose(file);
    temp_end(temp);
    result.data = 0;
    result.size = 0;
    return(result);
  }
  result.data[result.size] = 0;
  fclose(file);
  return(result);
}


typedef struct Cursor Cursor;
struct Cursor
{
  const char* data;
  size_t      size;
  size_t      at;
};

static int
cursor_at_end(Cursor* cursor)
{
  return(cursor->at >= cursor->size);
}

static char
cursor_peek(Cursor* cursor)
{
  if (cursor_at_end(cursor))
  {
    return(0);
  }
  return(cursor->data[cursor->at]);
}

static char
cursor_peek_next(Cursor* cursor)
{
  if (cursor->at + 1 >= cursor->size)
  {
    return(0);
  }
  return(cursor->data[cursor->at + 1]);
}

static char
cursor_advance(Cursor* cursor)
{
  if (cursor_at_end(cursor))
  {
    return(0);
  }
  return(cursor->data[cursor->at++]);
}

static String_Slice
cursor_slice(Cursor* cursor, size_t begin, size_t end)
{
  String_Slice result;
  result.data = cursor->data + begin;
  result.size = end - begin;
  return(result);
}

static String_Slice
cursor_peek_slice(Cursor* cursor, size_t size)
{
  String_Slice result = zero_struct;
  if (cursor->at + size <= cursor->size)
  {
    result.data = cursor->data + cursor->at;
    result.size = size;
  }
  return(result);
}

static int
cursor_match_string(Cursor* cursor, String_Slice string)
{
  String_Slice current = cursor_peek_slice(cursor, string.size);
  if (!current.data)
  {
    return(0);
  }
  if (!str_match(current, string))
  {
    return(0);
  }
  cursor->at += string.size;
  return(1);
}

static int
cursor_match_char(Cursor* cursor, char c)
{
  if (cursor_peek(cursor) != c)
  {
    return(0);
  }
  cursor->at++;
  return(1);
}

static void
cursor_skip_horizontal_whitespace(Cursor* cursor)
{
  for (;;)
  {
    char c = cursor_peek(cursor);
    if (c == ' ' || c == '\t' || c == '\r')
    {
      cursor_advance(cursor);
    }
    else
    {
      break;
    }
  }
}

static void
cursor_skip_whitespace(Cursor* cursor)
{
  while (!cursor_at_end(cursor))
  {
    if (!isspace((unsigned char)cursor_peek(cursor)))
    {
      break;
    }
    cursor_advance(cursor);
  }
}

static int
cursor_at_identifier_start(Cursor* cursor)
{
  char c = cursor_peek(cursor);
  return(isalpha((unsigned char)c) || c == '_');
}

static int
cursor_at_identifier_char(Cursor* cursor)
{
  char c = cursor_peek(cursor);
  return(isalnum((unsigned char)c) || c == '_');
}

static String_Slice
cursor_read_identifier(Cursor* cursor)
{
  String_Slice result = zero_struct;
  if (!cursor_at_identifier_start(cursor))
  {
    return(result);
  }
  size_t begin = cursor->at;
  cursor_advance(cursor);
  while (cursor_at_identifier_char(cursor))
  {
    cursor_advance(cursor);
  }
  result.data = cursor->data + begin;
  result.size = cursor->at - begin;
  return(result);
}

static int
cursor_at_line_comment(Cursor* cursor)
{
  return(cursor_peek(cursor) == '/' && cursor_peek_next(cursor) == '/');
}

static int
cursor_at_block_comment(Cursor* cursor)
{
  return(cursor_peek(cursor) == '/' && cursor_peek_next(cursor) == '*');
}

static void
cursor_skip_line_comment(Cursor* cursor)
{
  if (!cursor_at_line_comment(cursor))
  {
    return;
  }
  cursor_advance(cursor);
  cursor_advance(cursor);
  while (!cursor_at_end(cursor))
  {
    if (cursor_advance(cursor) == '\n')
    {
      break;
    }
  }
}

static void
cursor_skip_block_comment(Cursor* cursor)
{
  if (!cursor_at_block_comment(cursor))
  {
    return;
  }
  cursor_advance(cursor);
  cursor_advance(cursor);
  while (!cursor_at_end(cursor))
  {
    if (cursor_peek(cursor) == '*' && cursor_peek_next(cursor) == '/')
    {
      cursor_advance(cursor);
      cursor_advance(cursor);
      break;
    }
    cursor_advance(cursor);
  }
}

static void
cursor_skip_space_and_comments(Cursor* cursor)
{
  for (;;)
  {
    cursor_skip_whitespace(cursor);
    if (cursor_at_line_comment(cursor))
    {
      cursor_skip_line_comment(cursor);
      continue;
    }
    if (cursor_at_block_comment(cursor))
    {
      cursor_skip_block_comment(cursor);
      continue;
    }
    break;
  }
}

static void
cursor_skip_to_line_end(Cursor* cursor)
{
  while (!cursor_at_end(cursor))
  {
    if (cursor_advance(cursor) == '\n')
    {
      break;
    }
  }
}

static String_Slice
cursor_read_line(Cursor* cursor)
{
  size_t begin = cursor->at;
  while (!cursor_at_end(cursor))
  {
    if (cursor_peek(cursor) == '\n')
    {
      break;
    }
    cursor_advance(cursor);
  }
  size_t end = cursor->at;
  if (end > begin && cursor->data[end - 1] == '\r')
  {
    end--;
  }
  if (!cursor_at_end(cursor))
  {
    cursor_advance(cursor);
  }
  return(cursor_slice(cursor, begin, end));
}

static int
cursor_at_section(Cursor* cursor)
{
  size_t position = cursor->at;
  cursor_skip_horizontal_whitespace(cursor);
  int result = cursor_match_string(cursor, str_lit("// SECTION:"));
  cursor->at = position;
  return(result);
}

static String_Slice
cursor_read_section_name(Cursor* cursor)
{
  cursor_skip_horizontal_whitespace(cursor);
  if (!cursor_match_string(cursor, str_lit("// SECTION:")))
  {
    return((String_Slice){0});
  }
  cursor_skip_horizontal_whitespace(cursor);
  return(cursor_read_line(cursor));
}


typedef struct FunctionDecl FunctionDecl;
struct FunctionDecl
{
  FunctionDecl* next;

  String_Slice decl;
  String_Slice name;
};

typedef struct Section Section;
struct Section
{
  Section* next;

  FunctionDecl* first_function;
  FunctionDecl* last_function;

  String_Slice name;

  size_t begin;
  size_t end;
};

static FunctionDecl*
parse_function_decl(MemoryArena* arena, Cursor* cursor)
{
  size_t line_begin = cursor->at;
  cursor_skip_horizontal_whitespace(cursor);
  if (!cursor_match_string(cursor, str_lit("internal")))
  {
    cursor->at = line_begin;
    return(0);
  }
  if (cursor_at_identifier_char(cursor))
  {
    cursor->at = line_begin;
    return(0);
  }
  cursor_skip_horizontal_whitespace(cursor);
  String_Slice return_type = cursor_read_identifier(cursor);
  if (!return_type.data)
  {
    cursor->at = line_begin;
    return(0);
  }
  cursor_skip_horizontal_whitespace(cursor);
  String_Slice name = cursor_read_identifier(cursor);
  if (!name.data)
  {
    cursor->at = line_begin;
    return(0);
  }
  cursor_skip_horizontal_whitespace(cursor);
  if (cursor_peek(cursor) != '(')
  {
    cursor->at = line_begin;
    return(0);
  }
  while (!cursor_at_end(cursor))
  {
    char c = cursor_advance(cursor);
    if (c == ';')
    {
      FunctionDecl* result = PushStruct(arena, FunctionDecl);
      assert(result != 0);
      result->decl = cursor_slice(cursor, line_begin, cursor->at);
      result->name = name;
      return(result);
    }
    if (c == '\n')
    {
      cursor->at = line_begin;
      return(0);
    }
  }
  cursor->at = line_begin;
  return(0);
}

static Section*
parse_sections(MemoryArena* arena, Header_File* file)
{
  Section* first = 0;
  Section* last = 0;
  Cursor cursor = {file->data, file->size, 0};
  while (!cursor_at_end(&cursor))
  {
    if (!cursor_at_section(&cursor))
    {
      cursor_skip_to_line_end(&cursor);
      continue;
    }
    size_t section_begin = cursor.at;
    String_Slice name = cursor_read_section_name(&cursor);
    if (!name.data)
    {
      cursor_skip_to_line_end(&cursor);
      continue;
    }
    Section* section = PushStruct(arena, Section);
    assert(section != 0);
    section->name = name;
    section->begin = section_begin;
    while (!cursor_at_end(&cursor))
    {
      size_t line_begin = cursor.at;
      if (cursor_at_section(&cursor))
      {
        cursor.at = line_begin;
        break;
      }
      FunctionDecl* function = parse_function_decl(arena, &cursor);
      if (function)
      {
        if (section->last_function)
        {
          section->last_function->next = function;
        }
        else
        {
          section->first_function = function;
        }
        section->last_function = function;
        continue;
      }
      cursor.at = line_begin;
      cursor_skip_to_line_end(&cursor);
    }
    section->end = cursor.at;
    if (last)
    {
      last->next = section;
    }
    else
    {
      first = section;
    }
    last = section;
  }
  return(first);
}

static void
cursor_skip_string(Cursor* cursor, char quote)
{
  if (cursor_peek(cursor) != quote)
  {
    return;
  }
  cursor_advance(cursor);
  while (!cursor_at_end(cursor))
  {
    char c = cursor_advance(cursor);
    if (c == '\\')
    {
      if (!cursor_at_end(cursor))
      {
        cursor_advance(cursor);
      }
    }
    else if (c == quote)
    {
      break;
    }
  }
}

static int
cursor_find_function_definition(Cursor* cursor, String_Slice function_name)
{
  while (!cursor_at_end(cursor))
  {
    if (cursor_at_line_comment(cursor))
    {
      cursor_skip_line_comment(cursor);
      continue;
    }
    if (cursor_at_block_comment(cursor))
    {
      cursor_skip_block_comment(cursor);
      continue;
    }
    if (cursor_peek(cursor) == '"' || cursor_peek(cursor) == '\'')
    {
      cursor_skip_string(cursor, cursor_peek(cursor));
      continue;
    }
    if (!cursor_at_identifier_start(cursor))
    {
      cursor_advance(cursor);
      continue;
    }
    String_Slice identifier = cursor_read_identifier(cursor);
    if (!str_match(identifier, function_name))
    {
      continue;
    }
    cursor_skip_space_and_comments(cursor);
    if (cursor_peek(cursor) != '(')
    {
      continue;
    }
    cursor_advance(cursor);
    int depth = 1;
    while (!cursor_at_end(cursor) && depth > 0)
    {
      if (cursor_at_line_comment(cursor))
      {
        cursor_skip_line_comment(cursor);
        continue;
      }
      if (cursor_at_block_comment(cursor))
      {
        cursor_skip_block_comment(cursor);
        continue;
      }
      if (cursor_peek(cursor) == '"' || cursor_peek(cursor) == '\'')
      {
        cursor_skip_string(cursor, cursor_peek(cursor));
        continue;
      }
      char c = cursor_advance(cursor);
      if (c == '(')
      {
        depth++;
      }
      else if (c == ')')
      {
        depth--;
      }
    }
    if (depth != 0)
    {
      return(0);
    }
    cursor_skip_space_and_comments(cursor);
    if (cursor_peek(cursor) == '{')
    {
      return(1);
    }
  }
  return(0);
}

static int
source_has_function(Header_File* source, String_Slice name)
{
  Cursor cursor = {source->data, source->size, 0};
  return(cursor_find_function_definition(&cursor, name));
}

static void
write_function_stub(FILE* file, FunctionDecl* function)
{
  String_Slice decl = function->decl;
  size_t name_offset = (size_t)(function->name.data - decl.data);
  String_Slice before_name = {decl.data, name_offset};
  size_t after_name_offset = name_offset + function->name.size;
  String_Slice after_name = {decl.data + after_name_offset, decl.size - after_name_offset};
  while (before_name.size > 0)
  {
    char c = before_name.data[before_name.size - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      before_name.size--;
    }
    else
    {
      break;
    }
  }
  while (before_name.size > 0)
  {
    char c = before_name.data[0];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      before_name.data++;
      before_name.size--;
    }
    else
    {
      break;
    }
  }
  String_Slice internal = str_lit("internal");
  if (before_name.size >= internal.size)
  {
    String_Slice prefix = {before_name.data, internal.size};
    if (str_match(prefix, internal))
    {
      before_name.data += internal.size;
      before_name.size -= internal.size;
    }
  }
  while (before_name.size > 0)
  {
    char c = before_name.data[0];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      before_name.data++;
      before_name.size--;
    }
    else
    {
      break;
    }
  }
  while (after_name.size > 0)
  {
    char c = after_name.data[after_name.size - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
    {
      after_name.size--;
    }
    else
    {
      break;
    }
  }
  if (after_name.size > 0 && after_name.data[after_name.size - 1] == ';')
  {
    after_name.size--;
  }
  fprintf(file, "internal %.*s\n" "%.*s%.*s\n" "{\n" "  NotImplemented;\n" "}\n",
          str_slice_varg(before_name), str_slice_varg(function->name), str_slice_varg(after_name));
}

static int
section_has_missing_functions(Section* section, Header_File* source)
{
  for (FunctionDecl* function = section->first_function; function; function = function->next)
  {
    if (!source_has_function(source, function->name))
    {
      return(1);
    }
  }
  return(0);
}

static void
generate_missing_functions(FILE* file, Section* sections, Header_File* source)
{
  int wrote_header = 0;
  for (Section* section = sections; section; section = section->next)
  {
    if (!section_has_missing_functions(section, source))
    {
      continue;
    }
    if (!wrote_header)
    {
      fprintf(file, "\n\n");
      wrote_header = 1;
    }
    fprintf(file, "// SECTION: %.*s\n" "\n", str_slice_varg(section->name));
    for (FunctionDecl* function = section->first_function; function; function = function->next)
    {
      if (!source_has_function(source, function->name))
      {
        write_function_stub(file, function);
        fprintf(file, "\n");
        printf("GENERATED: %.*s\n", str_slice_varg(function->name));
      }
    }
  }
}

// NOTE(sep): Same commit/rollback pattern as header_file_alloc — the buffer
// is over-allocated (strlen + 3, room for a trailing ".c\0"), used as scratch
// while we splice the extension, then kept permanently on success.
static char*
make_source_path(MemoryArena* arena, const char* header_path)
{
  TempArena temp = temp_begin(arena);
  size_t size = strlen(header_path);
  char* result = PushArray(arena, char, size + 3);
  if (!result)
  {
    temp_end(temp);
    return(0);
  }
  memcpy(result, header_path, size + 1);
  char* slash1 = strrchr(result, '/');
  char* slash2 = strrchr(result, '\\');
  char* slash = slash1;
  if (slash2 && (!slash1 || slash2 > slash1))
  {
    slash = slash2;
  }
  char* dot = strrchr(result, '.');
  if (!dot || (slash && dot < slash))
  {
    memcpy(result + size, ".c", 3);
  }
  else
  {
    dot[0] = '.';
    dot[1] = 'c';
    dot[2] = 0;
  }
  return(result);
}

int
main(int argc, char** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: cstub <header>\n");
    return(1);
  }
  MemoryArena* arena = arena_alloc(MB(4));
  Header_File header = header_file_alloc(arena, argv[1]);
  if (!header.data)
  {
    fprintf(stderr, "could not read '%s'\n", argv[1]);
    arena_release(arena);
    return(1);
  }
  Section* sections = parse_sections(arena, &header);
  char* source_path = make_source_path(arena, argv[1]);
  if (!source_path)
  {
    fprintf(stderr, "could not construct source path\n");
    arena_release(arena);
    return(1);
  }
  Header_File source = header_file_alloc(arena, source_path);
  if (!source.data)
  {
    source.data = PushStruct(arena, char);
    if (!source.data)
    {
      fprintf(stderr, "could not allocate empty source buffer\n");
      arena_release(arena);
      return(1);
    }
    source.data[0] = 0;
    source.size = 0;
    printf("'%s' does not exist yet, it will be created\n", source_path);
  }
  printf("Header: %s\n", argv[1]);
  printf("Source: %s\n\n", source_path);
  FILE* output = fopen(source_path, "ab");
  if (!output)
  {
    fprintf(stderr, "could not open '%s' for writing\n", source_path);
    arena_release(arena);
    return(1);
  }
  generate_missing_functions(output, sections, &source);
  fclose(output);
  arena_release(arena);
  return(0);
}
