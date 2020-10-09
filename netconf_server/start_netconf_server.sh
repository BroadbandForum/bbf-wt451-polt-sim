#!/bin/bash

START_DIR=`dirname $0`
cd $START_DIR
export SYSREPO_REPOSITORY_PATH=`pwd`/sysrepo
export LIBYANG_EXTENSIONS_PLUGINS_DIR=`pwd`/lib/libyang/extensions
export LIBYANG_USER_TYPES_PLUGINS_DIR=`pwd`/lib/libyang/user_types
export LD_LIBRARY_PATH=`pwd`/lib:$LD_LIBRARY_PATH

# Make sure Netopeer2 is started
/usr/local/bin/start_netopeer2_server.sh
sleep 2

NETCONF_PARMS=""
if [ "$1" = "gdb" ]; then
    GDB="gdb --args"
    shift
fi
if [ "$1" = "valgrind" ]; then
    GDB="valgrind"
    shift
fi
$GDB ./bcmolt_netconf_server $*
