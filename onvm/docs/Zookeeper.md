openNetVM ZooKeeper Datastore
==

When running in distributed mode, openNetVM uses a [ZooKeeper](http://zookeeper.apache.org/) datastore to track and synchronize global state of the system. In general, ZooKeeper is used to track the following:

  - MAC addresses of running ONVM managers
  - Which services are running on which managers
  - Statistics for each NF

Cluster Setup
--

I have the following cluster set up:

 - A constant number of running ZooKeeper nodes that are voting members of the ensemble. This number should be chosen to provide the reliability guarantees required by the application.
 - ONVM managers connect to the cluster as nonvoting [observers](https://zookeeper.apache.org/doc/trunk/zookeeperObservers.html) (which allows scaling without hurting write performance).

The ONVM manager connects to an instance of ZooKeeper running on the local machine. This means that all reads are served over a local socket and do not need to happen over the network. The script found in `scripts/zookeeper-install.sh` will [dynamically configure](https://zookeeper.apache.org/doc/trunk/zookeeperReconfig.html) the cluster for the new ZooKeeper instance. That script will require the following environment variables to be set:

  - `ONVM_ZK_INIT`: The IP address of one of the static ZooKeeper nodes. This is the known entry point to the cluster.
  - `ONVM_ZK_IP`: The IP address of the interface this node will use to communicate with ZooKeeper. This tells the rest of the ensemble how to reach the new node.
  - `ONVM_DISTRIBUTED`: Set this variable to anything so that ONVM can run in distributed mode.

Datastore Structure
--

The ZooKeeper datastore is similar to a hierarchical filesystem. We store data in the following schema:

  - Running managers. These nodes have the format `/managers/<manager id>` and are ephemeral (will get automatically deleted when the manager exits). The manager ID comes from the ZooKeeper connection. The data of this node contains the MAC address where this manager can be found.
  - Service to manager mapping. These nodes have the format `/services/<service id>/<manager id>` and are ephemeral. The data of this node contains the number of service instances running on that host.
  - NF stat nodes. These nodes have the format `/nf/<service id>/nf<increasing id>` and are ephemeral. These nodes are created with flag `ZOO_SEQUENTIAL` (so they have an increasing number appended to the end). The data of these nodes contains a string representation of the json stats dict computed by the manager.

Performance Implications
--

For more information on how ZooKeeper works, see the [description wiki page](https://cwiki.apache.org/confluence/display/ZOOKEEPER/ProjectDescription) or the [original paper](https://www.usenix.org/legacy/event/atc10/tech/full_papers/Hunt.pdf).

Each host running ZooKeeper keeps an in-memory representation of the entire tree. This means that read requests can be satisfied locally without resorting to network operations. For writes, the requests are agreed upon by other services before they are committed to the datastore. Therefore, we want to keep writes to a minimum, especially when on the critical path.
