#! /bin/bash
set -e

# A script to install and configure ZooKeeper
# For use with distributed OpenNetVM
# Expected to be run from within scripts/install.sh
# CONFIGURATION (via environment variable):
#  - Set $ONVM_ZK_DATA_DIR to where you want to store ZooKeeper data (defaults to /zookeeper)
#  - Make sure $ONVM_ZK_INIT has the host of an existing ZooKeeper node
#  - Set $ONVM_ZK_IP to this node's IP address to add to ZooKeeper

# Validate sudo access
sudo -v

# Ensure we're working relative to the onvm root directory
if [ $(basename $(pwd)) == "scripts" ]; then
    cd ..
fi

# Set state variables
start_dir=$(pwd)
me=$(whoami)
zk_data_dir="${ONVM_ZK_DATA_DIR:-/zookeeper}"

# Compile zookeeper
cd zookeeper
echo "Building Zookeeper server in $(pwd)"
sleep 1
ant bin-package

cd src/c
echo "Compiling and installing Zookeeper C Library in $(pwd)"
sleep 1
autoreconf -i
autoconf
./configure
make -j 8
sudo make install -j 8

# Compile Zookeeper Queue library
cd $start_dir/zookeeper/src/recipes/queue/src/c
autoreconf -if
./configure CFLAGS='-Wno-unused'
make -j 8
sudo make install -j 8

# Refresh sudo
sudo -v

# Create zookeeper data directory
cd $start_dir
sudo mkdir -p $zk_data_dir
sudo chown -R $me $zk_data_dir

# Get the ID for this machine and set up ZK config
hex_id=$(hostid)  # returns in hex
myid=$(printf "%d" 0x$hex_id)
echo "This machine's ID is: $myid"
echo $myid > $zk_data_dir/myid
cp ./scripts/zoo_replicated1.cfg.dynamic.default $zk_data_dir/conf/zoo_replicated1.cfg.dynamic
cp ./scripts/zoo.cfg.default $start_dir/zookeeper/conf/zoo.cfg

# Add this host to the cluster
# Using the IP of em1
echo "server.$myid=$ONVM_ZK_IP:2888:3888:observer;2791" >> $zk_data_dir/conf/zoo_replicated1.cfg.dynamic
$start_dir/zookeeper/src/c/cli_st $ONVM_ZK_INIT:2791 cmd:"reconfig -add $myid=$ONVM_ZK_IP:2888:3888:observer;2791"
