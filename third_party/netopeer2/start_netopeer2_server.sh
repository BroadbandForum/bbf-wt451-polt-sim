#!/bin/bash
# Parameters:
# - tool parameters
#set -x

fs_bin_dir=`dirname $0`
tool_name=$fs_bin_dir/netopeer2-server
sysrepo_dir=$fs_bin_dir/../sysrepo
lib_dir=$fs_bin_dir/../lib

export LD_LIBRARY_PATH=$lib_dir:$LD_LIBRARY_PATH
export SYSREPO_REPOSITORY_PATH=$sysrepo_dir
export LIBYANG_EXTENSIONS_PLUGINS_DIR=$lib_dir/libyang/extensions
export LIBYANG_USER_TYPES_PLUGINS_DIR=$lib_dir/libyang/user_types
# copy OLT certificate
cp /root/.ssh/polt.cer /certificates/polt.cer

if [ "$1" = "gdb" ]; then
    GDB="gdb --args"
    shift
fi
# cleanup
if ps -ef | grep netopeer2\-server | grep -v grep > /dev/null; then
    echo netopeer2-server is already running
    exit -1
fi
if [ "`whoami`" = "root" ]; then
    chown -R root:root $sysrepo_dir/*
fi
if ! ps -ef | grep netconf_server | grep -v grep > /dev/null; then
    echo Cleaning up stale state
    rm -fr /dev/shm/sr* $sysrepo_dir/sr_evpipe* /tmp/netopeer2-server.pid
fi
$GDB $tool_name $*
