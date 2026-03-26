#!/bin/bash

set -e

function fetch_and_extract() {
    dirname="$1"
    filetolookfor="$2"
    url="$3"

    if [ -f "$filetolookfor" ]; then
        echo "[$dirname] Sources already present"
        return
    fi

    if [ -f "$dirname.zip" ]; then
        echo "[$dirname] ZIP already present"
        return
    fi

    echo "[$dirname] Fetching sources ..."
    curl -L -o "$dirname.zip" "$url"

    echo "[$dirname] Extracting sources ..."
    unzip $dirname.zip
    for d in $dirname-*/ ; do cd "$d"; mv . ../; cd ..; done
    rmdir $dirname-*/
}
