#!/bin/bash

mkdir -p .api/jobs.0/
rm -rf .api/jobs.*/*/
for d in .api/jobs.*; do
	for subdir in "in" "out"; do
		mkdir -p /tmp/mallob_api/$d/$subdir
		ln -s /tmp/mallob_api/$d/$subdir $d/$subdir
	done
done

