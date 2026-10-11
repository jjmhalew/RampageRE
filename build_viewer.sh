#!/bin/sh
# build_viewer.sh - the old name of build.sh, kept so that older instructions and scripts still work: the same arguments.
exec sh "$(dirname "$0")/build.sh" "$@"
