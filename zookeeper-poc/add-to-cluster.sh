#! /bin/bash

# Add a new node to the specified ZooKeeper 3.5 cluster
# Example: ./add-to-cluster.sh ~/zookeeper 192.168.1.90:2791 192.168.1.137 37
# $1: Directory of zookeeper 3.5
# $2: host:port of an existing node
# $3: host of new server
# $4: new server id

ZK_DIR=$1
EXISTING=$2
NEW=$3
ID=$4
ME=$(whoami)

# Ensure the right directories exist
sudo -v
sudo mkdir -p /zookeeper/conf
sudo chown -R $ME /zookeeper
echo $ID > /zookeeper/myid
cp ./zoo_replicated1.cfg.dynamic.default /zookeeper/conf/zoo_replicated1.cfg.dynamic
echo "server.$ID=$NEW:2888:3888:observer;2791" >> /zookeeper/conf/zoo_replicated1.cfg.dynamic

# Set up ZK config file
cp ./zoo.cfg.default $ZK_DIR/conf/zoo.cfg

# Connect to an existing ZK host and add this new server to the cluster
cd $ZK_DIR/src/c
make
./cli_st $EXISTING cmd:"reconfig -add $ID=$NEW:2888:3888:observer;2791"
