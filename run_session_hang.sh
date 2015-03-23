#!/bin/bash

#bug 454
export test_name=run_session_hang
$test_dir/configure_maxscale.sh

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15

$test_dir/session_hang/run_setmix.sh &
perl $test_dir/session_hang/simpletest.pl

sleep 15


echo "show databases;" mysql -u$repl_user -p$repl_password -h$maxscale_IP -p 4006
res=$?
$test_dir/copy_logs.sh run_session_hang

exit $res
