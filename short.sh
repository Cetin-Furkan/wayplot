#!/usr/bin/env bash

OUTPUT_FILE="project_bundle.txt"
> "$OUTPUT_FILE"

# Collect strictly Makefile, main.c, and everything inside include/, src/, and shaders/
{
  #[ -f "Makefile" ] && echo "./Makefile"
  #[ -f "main.c" ] && echo "./main.c"
  for dir in tests; do
    [ -d "$dir" ] && find "$dir" -type f
  done
} | sort | while IFS= read -r file; do
  [ -f "$file" ] || continue
  echo "================================================================================" >> "$OUTPUT_FILE"
  echo "FILE: $file" >> "$OUTPUT_FILE"
  echo "================================================================================" >> "$OUTPUT_FILE"
  cat "$file" >> "$OUTPUT_FILE"
  echo -e "\n" >> "$OUTPUT_FILE"
done

echo "Successfully bundled core source files into $OUTPUT_FILE"