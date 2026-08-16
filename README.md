# cstub

A tool that generates stub functions for a C header.

## What it does

You mark functions in a header as internal, under a section comment.
cstub reads the header, finds those functions, and checks if they already exist in the matching .c file.
If a function is missing, cstub writes a stub for it into the .c file.

## Header format

```c
// SECTION: math

internal float add(float a, float b);
internal float sub(float a, float b);
```

## Usage

```
cstub path/to/header.h
```

This looks for path/to/header.c.
If the .c file does not exist, it will be created.
If a function from the header is missing in the .c file, a stub like this gets appended:

```c
internal float
add(float a, float b)
{
  NotImplemented;
}
```

## Example

```
cstub src/math.h
```

Reads src/math.h, writes missing stubs into src/math.c.
