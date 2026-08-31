#!/usr/bin/env bash

OUTPUT_FILE="project_bundle.txt"
> "$OUTPUT_FILE"

# Find all relevant source files while skipping bin, build, and .git directories
find . \
  -type d \( -name "bin" -o -name "build" -o -name ".git" \) -prune -o \
  -type f \( \
    -name "*.c" -o \
    -name "*.h" -o \
    -name "*.md" -o \
    -name "*.sh" -o \
    -name "Makefile" -o \
    -name "VERSION" \
  \) -print | sort | while IFS= read -r file; do
    echo "================================================================================" >> "$OUTPUT_FILE"
    echo "FILE: $file" >> "$OUTPUT_FILE"
    echo "================================================================================" >> "$OUTPUT_FILE"
    cat "$file" >> "$OUTPUT_FILE"
    echo -e "\n" >> "$OUTPUT_FILE"
done

echo "Successfully bundled all source files into $OUTPUT_FILE"
