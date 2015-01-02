#!/bin/sh

rm -rf autom4te.cache
rm -f aclocal.m4 ltmain.sh

autoreconf -f -i -v --warnings=all || exit 1

if [ -z "$NOCONFIGURE" ]; then
	./configure "$@"
fi
