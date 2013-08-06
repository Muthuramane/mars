#!/bin/bash
# Copyright 2010-2013 Frank Liepold /  1&1 Internet AG
#
# Email: frank.liepold@1und1.de
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

#####################################################################

# starts an endless loop which creates and removes files in a given directory
# on remote host
# the pid of the started process will be returned in the variable named by $4
# the name of the started script will be returned in the variable named by $5
function lib_rw_write_and_delete_loop
{
    [ $# -eq 8 ] || lib_exit 1 "wrong number $# of arguments (args = $*)"
    local host=$1 target_file=$2 file_size_in_gb=$3 part_of_size_to_write=$4
    local varname_pid=$5 varname_script=$6 no_of_loops=$7 sleeptime=$8
    local bs=1024 
    local dd_count=$(($file_size_in_gb * 1024 * 1024 / $part_of_size_to_write))
    local dir="$(dirname $target_file)"
    local script=$lib_rw_write_and_delete_script
    lib_vmsg "  checking directory of $host:$target_file"
    dir="$(dirname $target_file)"
    lib_remote_idfile $host "test -d $dir " || \
                lib_exit 1 "directory $host:$dir not found"
    
    # this script will be started
    # after no_of_loops a sleep 1 is executed instead of the dd, because the
    # script should be explizitly killed
    # 
    # the stderror output of dd is filtered
    echo '#/bin/bash
no_of_loops='$no_of_loops'
sleeptime='$sleeptime'
count=1
while true; do
    if [ $no_of_loops -ne 0 -a $count -gt $no_of_loops ]; then
        sleep 1
        continue
    fi
    yes $(printf "%0.1024d" $count) | dd of='"$target_file"'.$count bs='"$bs"' count='"$dd_count"' conv=fsync status=noxfer 3>&2 2>&1 >&3 | grep -v records 3>&2 2>&1 >&3
    rm -f '"$target_file"'.*
    echo count=$count
    sleep $sleeptime
    let count+=1
done' >$script
    lib_start_script_remote_bg $host $script $varname_pid \
                                         $varname_script
    main_error_recovery_functions["lib_rw_stop_scripts"]+="$host $script "
}

function lib_rw_stop_scripts
{
    local host script rc write_count
    while [ $# -gt 0 ]; do
        host=$1
        script=$2
        shift 2
        lib_rw_stop_one_script $host $script "write_count"
        rc=$?
        if [ $rc -ne 0 ]; then
            main_error_recovery_functions["lib_rw_stop_scripts"]=
            lib_exit 1
        fi
    done
}

function lib_rw_stop_one_script
{
    local host=$1 script=$2 varname_write_count=$3
    local my_write_count grep_out rc
    lib_vmsg "  determine pid of script $script on $host"
    pid=$(lib_remote_idfile $host pgrep -f $script)
    rc=$?
    [ $rc -ne 0 ] && return $rc

    lib_vmsg "  stopping script $script (pid=$pid) on $host"
    lib_remote_idfile $host kill -9 $pid
    rc=$?
    [ $rc -ne 0 ] && return $rc

    grep_out=$(lib_remote_idfile $host \
                        "grep '^count=[0-9][0-9]*$' $script.out | tail -1")
    if [ -n "$grep_out" ]; then
        my_write_count=${grep_out#count=}
        lib_vmsg "  write_count: $my_write_count"
        eval $varname_write_count=$my_write_count
    fi
    lib_vmsg "  removing files $script* on $host"
    lib_remote_idfile $host "rm -f $script*"
}

function lib_rw_start_writing_data_device
{
    [ $# -eq 5 ] || lib_exit 1 "wrong number $# of arguments (args = $*)"
    local varname_pid=$1 varname_script=$2 no_of_loops=$3 sleeptime=$4
    local res=$5
    lib_rw_write_and_delete_loop ${main_host_list[0]} \
                 ${resource_mount_point_list[$res]}/$lib_rw_file_to_write \
                 $(lv_config_get_lv_size ${resource_name_list[0]}) 4 \
                 $varname_pid $varname_script $no_of_loops $sleeptime
}

function lib_rw_stop_writing_data_device
{
    local script=$1 varname_write_count=$2
    lib_rw_stop_one_script ${main_host_list[0]} $script $varname_write_count
}

function lib_rw_cksum
{
    local host=$1 dev=$2 varname_cksum=$3 my_cksum_out
    lib_vmsg "  calculating cksum for $dev on $host"
    my_cksum_out=($(lib_remote_idfile $host cksum $dev)) || lib_exit 1 
    lib_vmsg "  cksum = ${my_cksum_out[@]}"
    eval $varname_cksum='('${my_cksum_out[0]}' '${my_cksum_out[1]}')'
}

function lib_rw_compare_checksums
{
    [ $# -eq 6 ] || lib_exit 1 "wrong number $# of arguments (args = $*)"
    local primary_host=$1 secondary_host=$2 dev=$3 cmp_size=$4
    local varname_cksum_primary=$5 varname_cksum_secondary=$6
    local host role primary_cksum_out secondary_cksum_out cksum_out
    local cksum_dev=$dev
                                            
    for role in "primary" "secondary"; do
        eval host='$'${role}_host
        if [ $cmp_size -ne 0 ]; then
            local dummy_file=$main_mars_directory/dummy-$host
            lib_vmsg "  dumping $cmp_size G of $dev to $dummy_file"
            lib_remote_idfile $host \
                "dd if=$dev of=$dummy_file bs=1024 count=$(($cmp_size * 1024 * 1024))" || lib_exit 1
            lib_remote_idfile $host "ls -l $dummy_file"
            cksum_dev=$dummy_file
        fi
        lib_rw_cksum $host $cksum_dev "cksum_out"
        eval ${role}_cksum_out='"${cksum_out[*]}"'
        if [ $cmp_size -ne 0 ]; then
            lib_remote_idfile $host "rm -f $dummy_file" || lib_exit 1
        fi
    done
    if [ "$primary_cksum_out" != "$secondary_cksum_out" ]; then
        lib_exit 1 "cksum primary: '$primary_cksum_out' != '$secondary_cksum_out' = cksum secondary"
    fi
    if [ -n "$varname_cksum_primary" ]; then
        for role in "primary" "secondary"; do
            eval eval '$varname_cksum_'$role='\"\$${role}_cksum_out\"'
        done
    fi
}

## under construction! Not needed up to now
function lib_rw_debug
{
    printf '#!/bin/bash\nsleep 10\n' >/tmp/f1
    lib_start_script_remote_bg istore-test-bs7 /tmp/f1 gix gox
    echo gix=$gix gox=$gox

    printf '#!/bin/bash\nwuerg\nsleep 10\n' >/tmp/f1
    lib_start_script_remote_bg istore-test-bs7 /tmp/f1 hix hox
    echo hix=$hix hox=$hox
    
    printf '#!/bin/bash\nwuzzl' >/tmp/f2
    lib_start_script_remote_bg istore-test-bs7 /tmp/f2 hux hax
    echo hux=$hux hax=$hax
}

function lib_rw_mount_data_device
{
    local host=$1 dev=$2 mount_point=$3
    local res=${resource_name_list[0]}
    if ! mount_is_device_mounted $host $dev; then
        mount_mount $host $dev $mount_point ${resource_fs_type_list[$res]} || \
                                                                    lib_exit 1
    fi
}
