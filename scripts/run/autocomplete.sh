#!/bin/bash

# Given a prefix, returns a sequence of matching files / directories.
# The variable "mode" must be defined as "directory" or "file" or "executable".
get_matching_files() {
  prefix="$1"
  
  dir=$(dirname "$prefix")/
  name=$(basename "$prefix")
  if [ "$dir" == "./" ]; then
    dir=""
  fi

  isdir=false
  case "$prefix" in */)
    isdir=true
  esac

  if $isdir; then
    # Match this directory (could be the desired output itself)
    output="${dir}${name}/"
    # Match files in sub-directories: dir/name*/*
    for word in $(compgen -G "${dir}${name}/*"); do
      if [ -d $word ] ; then
        output="${output} ${word}/"
      elif [ "$mode" == "executable" ] && [ -f "$word" ] && [ -r "$word" ] && [ -x "$word" ]; then
        output="${output} ${word}"
      elif [ "$mode" == "file" ] && [ -f "$word" ] && [ -r "$word" ]; then
        output="${output} ${word}"
      fi
    done
  else
    # Match directories: dir/name*/
    for word in $(compgen -G "${dir}${name}*/"); do
      output="${output} $word"
    done
    # Match files: dir/name*
    for word in $(compgen -G "${dir}${name}*"); do
      if [ "$mode" == "executable" ] && [ -f "$word" ] && [ -r "$word" ] && [ -x "$word" ]; then
        output="${output} ${word}"
      elif [ "$mode" == "file" ] && [ -f "$word" ] && [ -r "$word" ]; then
        output="${output} ${word}"
      fi
    done
  fi

  echo $output
}

# Extract options from Mallob's src/ directory. 
# If variable "keyword" is set, only extracts options whose definition line
# contains this keyword (via grep -E).
# If variable "default" is set, only extracts options whose default
# exactly matches this string.
filter_options() {
  if [ -z $keyword ]; then keyword='.'; fi
  if [ -z $default ]; then
    cat $MALLOB_BASE_DIR/src/optionslist.hpp $MALLOB_BASE_DIR/src/app/*/options.hpp|grep -E "$keyword" \
    |grep -oP 'OPT_[A-Z]+\([A-Za-z]+,( )*".*?",( )*".*?"' \
    | grep -oP '".*?"' | sed 's/"//g'|tr ' ' '\n'|grep -vE "^$"
  else
    cat $MALLOB_BASE_DIR/src/optionslist.hpp $MALLOB_BASE_DIR/src/app/*/options.hpp|grep -E "$keyword" \
    |grep -oP 'OPT_[A-Z]+\([A-Za-z]+,( )*".*?",( )*".*?",( )*'$default',' \
    | grep -oP '".*?"' | sed 's/"//g'|tr ' ' '\n'|grep -vE "^$"
  fi
}

# Script called for every attempt to autocomplete an argument.
_script() {

  local cur
  COMPREPLY=()
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[$((COMP_CWORD-1))]}"
  prevprev="${COMP_WORDS[$((COMP_CWORD-2))]}"
  #echo ":: $prevprev , $prev , $cur ::"

  matched=false
  
  # Try matching the value of a program option
  if [[ "$prev" =~ ^- ]] || ( [[ "$prevprev" =~ ^- ]] && [[ "$prev" == "=" ]] ); then

    # C list of commands with some autocompletion properties
    autocomplopts_dir=$(keyword="\[\[AUTOCOMPLETE_DIRECTORY\]\]" filter_options)
    autocomplopts_file=$(keyword="\[\[AUTOCOMPLETE_FILE\]\]" filter_options)
    autocomplopts_exec=$(keyword="\[\[AUTOCOMPLETE_EXECUTABLE\]\]" filter_options)  
    
    # Try to match one of these commands
    for opt in "DIR!" $autocomplopts_dir "EXEC!" $autocomplopts_exec "FILE!" $autocomplopts_file; do

      if [ $opt == "DIR!" ]; then mode="directory"; continue; fi
      if [ $opt == "EXEC!" ]; then mode="executable"; continue; fi
      if [ $opt == "FILE!" ]; then mode="file"; continue; fi 

      # Case (-opt , prefix)
      if ! $matched && [ "$prev" == "-$opt" ]; then
        case "$cur" in =*)
          cur="${cur:1}"
          COMPREPLY=( $(compgen -W "$(mode=$mode get_matching_files ${cur})" -- ${cur}) )
          matched=true
        esac
      fi

      # Case (-opt , = , prefix)
      if ! $matched && [ "$prevprev" == "-$opt" ] && [ "$prev" == "=" ]; then
          COMPREPLY=( $(compgen -W "$(mode=$mode get_matching_files ${cur})" -- ${cur}) )
          matched=true
      fi

      if $matched; then break; fi
    done
  fi

  # Try matching an option
  if ! $matched; then
    # BOOL options which are enabled by default: suggest as "-boolopt=0"
    enabled_boolopts=$(default=true keyword=OPT_BOOL filter_options|awk '{print "-"$1"=0"}')
    # BOOL options which are disabled by default: suggest as "-boolopt"
    disabled_boolopts=$(default=false keyword=OPT_BOOL filter_options|awk '{print "-"$1}')
    # All other options: suggest as "-opt="
    other_opts=$(keyword="OPT_(INT|FLOAT|STRING)" filter_options|awk '{print "-"$1"="}')

    COMPREPLY=( $(compgen -W "${enabled_boolopts} ${disabled_boolopts} ${other_opts}" -- ${cur}) )
  fi

  return 0
}

# Register autocompletion
export MALLOB_BASE_DIR=$(pwd)
exec="mallob"
echo "Exporting autocompletion for \"$exec\" and \"build/$exec\" with MALLOB_BASE_DIR=$MALLOB_BASE_DIR"
complete -o nospace -F _script $exec
complete -o nospace -F _script build/$exec
