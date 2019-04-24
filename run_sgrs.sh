#!/usr/bin/bash

#This script is used to run verbs through Openmpi
#environment in fnet00 & fnet01 --2019.04.01
set -euxo pipefail
OMPI_PATH=/home/mxx/opt/openmpi-4.0.0
HOSTFILE=/home/mxx/HOSTS/ompi_hosts
IB_SUPPROT="btl openib,self,vader"
IB_HCA="btl_openib_if_include mlx5_0"
RECEIVE_QUEUES="btl_openib_receive_queues P,128,256,192,128:S,2048,256,128,32:S,12288,256,128,32:S,65536,256,128,32"
VERBOSE="btl_base_verbose 50"
BIND="--report-bindings"
EXC1="coll ^hcoll"
EXC2="osc ^ucx"
EXC3="pml ^ucx"
ROOT="--allow-run-as-root"
NODE="--map-by node"
SGRS=/home/mxx/ddt_direct/verbs_mxx/sgrs

scp /home/mxx/ddt_direct/verbs_mxx/sgrs suse108:/home/mxx/ddt_direct/verbs_mxx/

$OMPI_PATH/bin/mpirun -np 2 -hostfile $HOSTFILE $NODE $BIND $ROOT $SGRS -E 1
#
#$OMPI_PATH/bin/mpirun -np 2 -hostfile $HOSTFILE --mca $IB_SUPPROT \
#                                          --mca $IB_HCA\
#                                          --mca $RECEIVE_QUEUES\
#                                          --mca $VERBOSE\
#                                        $BIND $ROOT $1
