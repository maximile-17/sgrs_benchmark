#!/bin/bash

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
SGRS=/home/mxx/ddt_direct/copy_dma/sgrs 
HELPMSG="orte_base_help_aggregate 0"

scp /home/mxx/ddt_direct/copy_dma/sgrs suse108:/home/mxx/ddt_direct/copy_dma/
#$OMPI_PATH/bin/mpirun -np 2 $BIND $1

#
#$OMPI_PATH/bin/mpirun -np 2 -hostfile $HOSTFILE --mca $IB_SUPPROT \
#                                          --mca $IB_HCA\
#                                          --mca $RECEIVE_QUEUES\
#                                        $NODE $BIND $ROOT $SGRS -E 1 
#for block_size in 32 64 128 256 512 1024 $[1024*2] $[1024*4] $[1024*8] $[1024*16] $[1024*32] $[1024*64] $[1024*128] $[1024*256] $[1024*512] $[1024*1024]
for block_num in 1 2 4 8 16 24 30
do
for block_size in 4 8 16 32 64 128 256 512 1024 $[1024*2] $[1024*4] $[1024*8] $[1024*16] $[1024*32] $[1024*64] $[1024*128]
do
$OMPI_PATH/bin/mpirun -np 2 -hostfile $HOSTFILE --mca $HELPMSG $BIND $NODE $ROOT $SGRS -E 1 -b $block_size -n $block_num -s $[1024*1024*45] -W 1000 -N 10000
done
done
