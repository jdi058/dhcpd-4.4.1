# Copyright (C) 2012-2014, 2016  Internet Systems Consortium, Inc. ("ISC")
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.



# Clean up after rrl tests.

rm -f dig.out* *mdig.out*
rm -f  */named.memstats */named.run */named.stats */log-* */session.key
rm -f ns3/bl*.db */*.jnl */*.core */*.pid
rm -f ns*/named.lock
rm -f broken.out
