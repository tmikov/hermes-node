#!/usr/bin/env bash

IFS=$'\n'
set -f

# Directories to format - modify this list to change which directories are processed
FORMAT_DIRS=(lib tools include unittests)

all=0
force=0
interactive=0
verbose=0
check=0

# Process multiple options
while [ "$#" -gt 0 ]; do
  case "$1" in
    -a)
      all=1
      shift
      ;;
    -f)
      force=1
      shift
      ;;
    -i)
      interactive=1
      shift
      ;;
    -v)
      verbose=1
      shift
      ;;
    --check)
      check=1
      shift
      ;;
    *)
      echo "Usage: $0 [-a] [-f] [-i] [-v] [--check]" >&2
      echo "  -a:      Format all files" >&2
      echo "  -f:      Force format files in last commit without prompting" >&2
      echo "  -i:      Interactive mode, show diff and ask before applying changes" >&2
      echo "  -v:      Verbose mode, show additional information about which files are being processed" >&2
      echo "  --check: Dry-run over all files in FORMAT_DIRS; exit non-zero if any need formatting (no files modified)" >&2
      exit 1
      ;;
  esac
done

# Fall back to one in $PATH if not found.
clang_format="$(which clang-format)" || true

if [ ! -x "$clang_format" ]; then
  echo "ERROR: Must have clang-format in PATH"
  exit 1
fi

# Determine which diff command to use
if [ -t 1 ] && diff --color=always /dev/null /dev/null >/dev/null 2>&1; then
  # Terminal supports color and GNU diff with color is available
  diff_cmd=(diff --color=always -u)
elif which colordiff >/dev/null 2>&1; then
  # Use colordiff as alternative
  diff_cmd=(colordiff -u)
else
  # Fallback to plain diff
  diff_cmd=(diff -u)
fi

THIS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$THIS_DIR"/../ || exit 1

# --check mode: dry-run clang-format over all FORMAT_DIRS files. Exits 0 if
# clean, non-zero (and prints offending files) if any would be reformatted.
# Does not modify any files. Used by CI.
if (( check )); then
  check_files=(
    $(find "${FORMAT_DIRS[@]}" \
        -name \*.h -print -o -name \*.c -print -o -name \*.cpp -print)
  )
  if [ ${#check_files[@]} -eq 0 ]; then
    echo "No files found in FORMAT_DIRS to check."
    exit 0
  fi
  if (( verbose )); then
    echo "Checking ${#check_files[@]} files..."
  fi
  if "$clang_format" --dry-run --Werror -style=file "${check_files[@]}"; then
    echo "All files are properly formatted."
    exit 0
  else
    echo "" >&2
    echo "Formatting issues detected. Run './utils/format.sh -a' to fix." >&2
    exit 1
  fi
fi

# Function to check if a file is in one of the allowed directories
is_in_format_dirs() {
  local file="$1"
  for dir in "${FORMAT_DIRS[@]}"; do
    # Check if the file starts with the directory path
    if [[ "$file" == "$dir/"* ]] || [[ "$file" == "$dir" ]]; then
      return 0
    fi
  done
  return 1
}

have_changes=0       # We have uncommitted changes
authored_previous=0  # We authored the most recent commit on this branch

if [[ $(git status --porcelain 2>/dev/null) ]]; then
  have_changes=1
fi

if git log -1 --pretty=format:"%an <%ae>" | grep -q -e "<$USER@" -e "^$USER" -e "<$USER>" -e "$USER@"
then
  authored_previous=1
fi

files=()

if (( all )); then
  echo "Formatting all files..."
  files=(
    $(find "${FORMAT_DIRS[@]}" \
        -name \*.h -print -o -name \*.c -print -o -name \*.cpp -print)
  )
  if (( verbose )); then
    echo "Files to be processed:"
    printf '  %s\n' "${files[@]}"
  fi
elif (( have_changes )); then
  echo "Formatting modified and staged files..."
  # Get both unstaged changes (git diff) and staged changes (git diff --cached)
  modified_files=$(git diff --name-only --diff-filter=ACMR | grep -E '\.(h|c|cpp|inc|def|mm|m)$')
  staged_files=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(h|c|cpp|inc|def|mm|m)$')

  # Combine the lists and filter by allowed directories
  files=()
  for f in $modified_files $staged_files; do
    if is_in_format_dirs "$f"; then
      files+=("$f")
    elif (( verbose )); then
      echo "Skipping file outside format directories: $f"
    fi
  done

  if (( verbose )); then
    echo "Modified files to be processed:"
    printf '  %s\n' "${files[@]}"
  fi
elif (( authored_previous )) || (( force )); then
  all_last_commit_files=( $(git diff-tree --no-commit-id --name-only -r HEAD | grep -E '\.(h|c|cpp|inc|def|mm|m)$') )
  
  # Filter by allowed directories
  last_commit_files=()
  for f in "${all_last_commit_files[@]}"; do
    if is_in_format_dirs "$f"; then
      last_commit_files+=("$f")
    elif (( verbose )); then
      echo "Skipping file outside format directories: $f"
    fi
  done
  
  if [ ${#last_commit_files[@]} -gt 0 ]; then
    echo "There are no modified files, but you authored the last commit:"
    printf '  %s\n' "${last_commit_files[@]}"
    if (( !force )); then
      read -r -p "Format files in that commit? [y/N]" reply
      if [[ $reply != [Yy] ]]; then
        echo "Not doing anything."
        exit 0
      fi
    fi
    files=( "${last_commit_files[@]}" )
  else
    echo "No files from the last commit are in the format directories."
    exit 0
  fi
  if (( verbose )); then
    echo "Last commit files to be processed:"
    printf '  %s\n' "${files[@]}"
  fi
else
  echo "You have no modified files and you didn't recently commit on this branch."
  echo "To format all files: $0 -a"
  exit 1
fi

for f in "${files[@]}"; do
  if [ ! -f "$f" ]; then
    echo "File not found: $f (skipping)"
    continue
  fi

  if (( verbose )); then
    echo "Processing file: $f"
  fi

  # Create a temporary file for the formatted content
  temp_file=$(mktemp)
  "$clang_format" -style=file "$f" > "$temp_file"

  # Check if the file would be modified
  if ! diff -q "$f" "$temp_file" >/dev/null 2>&1; then
    echo "File would be reformatted: $f"

    # Always show the differences with color
    "${diff_cmd[@]}" "$f" "$temp_file" | sed "s|$temp_file|$f (formatted)|"

    if (( interactive )); then
      # Ask user whether to apply changes
      read -r -p "Apply these changes? [y/N] " reply
      if [[ $reply == [Yy] ]]; then
        cp "$temp_file" "$f"
        echo "Changes applied to $f"
      else
        echo "Changes not applied to $f"
      fi
    else
      # Apply changes without asking
      cp "$temp_file" "$f"
      echo "Reformatted: $f"
    fi
  else
    echo "No changes needed for $f"
  fi

  # Clean up the temporary file
  rm -f "$temp_file"
done

echo "Done"
