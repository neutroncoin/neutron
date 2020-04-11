#!/bin/sh
#
# Copyright (c) 2017-2020 The Swipp Developers
# Copyright (c) 2020 The Neutron Developers
#
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

if [ $# -gt 0 ]; then
    FILE="$1"
    shift
    if [ -f "$FILE" ]; then
        INFO="$(head -n 1 "$FILE")"
    fi
else
    echo "Usage: $0 <filename>"
    exit 1
fi

if [ -e "$(which git)" ]; then
    git diff >/dev/null 2>/dev/null

    # get a string like "v0.6.0-66-g59887e8"
    DESC="$(git describe --tags 2>/dev/null)"

    # get a string like "2012-04-10 16:27:19 +0200"
    TIME="$(git log -n 1 --format="%ci")"
fi

if [ -n "$DESC" ]; then
    NEWINFO="#define BUILD_DESC \"$DESC\""
else
    NEWINFO="// No build information available"
fi

# only update build.h if necessary
if [ "$INFO" != "$NEWINFO" ]; then
    echo "$NEWINFO" >"$FILE"
    echo "#define BUILD_DATE \"$TIME\"" >>"$FILE"
fi
