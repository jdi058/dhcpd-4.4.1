#!/bin/sh

PATH=":${PATH}"
export PATH

status=0
if [ -n "" -a -f Atffile ]
then
	echo "S:unit:`date`"
        echo "T:unit:1:A"
	echo "I: unit tests"
	atf-run > atf.out
	status=$?

        # | cat is there to force non-fancy output
	atf-report < atf.out | cat

	if [ $status -eq 0 ]
	then
		rm -f atf.out
		echo R:PASS
	else
		echo R:FAIL
	fi
	echo "E:unit:`date`"
fi
exit $status
