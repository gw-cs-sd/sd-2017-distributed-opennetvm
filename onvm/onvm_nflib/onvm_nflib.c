/*********************************************************************
 *                     openNetVM
 *              https://sdnfv.github.io
 *
 *   BSD LICENSE
 *
 *   Copyright(c)
 *            2015-2016 George Washington University
 *            2015-2016 University of California Riverside
 *            2016 Hewlett Packard Enterprise Development LP
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * The name of the author may not be used to endorse or promote
 *       products derived from this software without specific prior
 *       written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ********************************************************************/

/******************************************************************************

                                  onvm_nflib.c


                  File containing all functions of the NF API


******************************************************************************/


/***************************Standard C library********************************/


#include <getopt.h>
#include <signal.h>
#include <rte_lcore.h>
#include <rte_launch.h>


/*****************************Internal headers********************************/


#include "onvm_nflib.h"
#include "onvm_includes.h"
#include "onvm_sc_common.h"


/**********************************Macros*************************************/


// Number of packets to attempt to read from queue
#define PKT_READ_SIZE  ((uint16_t)32)

// Possible NF packet consuming modes
#define NF_MODE_UNKNOWN 0
#define NF_MODE_SINGLE 1
#define NF_MODE_RING 2

/******************************Global Variables*******************************/

// User supplied service ID
static uint16_t service_id = -1;

// User-given NF Client ID (defaults to manager assigned)
uint16_t initial_instance_id;

// True as long as the NF should keep processing packets
uint8_t keep_running;

// ring used for NF -> mgr messages (like startup & shutdown)
static struct rte_ring *mgr_msg_queue;

// Shared pool for all clients info
static struct rte_mempool *nf_info_mp;

// Shared pool for mgr <--> NF messages
static struct rte_mempool *nf_msg_pool;

// Shared data for default service chain
static struct onvm_service_chain *default_chain;

// Keeping track of the inital args (but only once), so we can use them again
static int first_init_flag = 1;
static int first_argc;
static char **first_argv;
static const char *first_nf_tag;

/***********************Internal Functions Prototypes*************************/


/*
 * Function that initialize a nf info data structure.
 *
 * Input  : the tag to name the NF
 * Output : the data structure initialized
 *
 */
static struct onvm_nf_info *
onvm_nflib_info_init(const char *tag);


/*
 * Function printing an explanation of command line instruction for a NF.
 *
 * Input : name of the executable containing the NF
 *
 */
static void
onvm_nflib_usage(const char *progname);


/*
 * Function that parses the global arguments common to all NFs.
 *
 * Input  : the number of arguments (following C standard library convention)
 *          an array of strings representing these arguments
 * Output : an error code
 *
 */
static int
onvm_nflib_parse_args(int argc, char *argv[]);


/*
* Signal handler to catch SIGINT.
*
* Input : int corresponding to the signal catched
*
*/
static void
onvm_nflib_handle_signal(int sig);

/*
 * Check if there are packets in this NF's RX Queue and process them
 */
static inline void
onvm_nflib_dequeue_packets(void **pkts, struct onvm_nf_info *info, pkt_handler handler) __attribute__((always_inline));

/*
 * Check if there is a message available for this NF and process it
 */
static inline void
onvm_nflib_dequeue_messages(struct onvm_nf_info *info) __attribute__((always_inline));

static inline void
onvm_nflib_scale(struct onvm_nf_info *info) __attribute__((always_inline));

static int
onvm_nflib_start_child(void *arg);

/*
 * Set this NF's status to not running and release memory
 *
 * Input: Info struct corresponding to this NF
 */
static void
onvm_nflib_cleanup(struct onvm_nf_info *info);

/************************************API**************************************/


int
onvm_nflib_init(int argc, char *argv[], const char *nf_tag, struct onvm_nf_info **nf_info_p) {
        const struct rte_memzone *mz;
	const struct rte_memzone *mz_scp;
        struct rte_mempool *mp;
	struct onvm_service_chain **scp;
        struct onvm_nf_msg *startup_msg;
        struct onvm_nf_info *nf_info;
        int retval_eal = 0;
        int retval_parse, retval_final;

        /* Only do EAL init from the master core */
        if (first_init_flag && (retval_eal = rte_eal_init(argc, argv)) < 0)
                return -1;

        /* Modify argc and argv to conform to getopt rules for parse_nflib_args */
        argc -= retval_eal; argv += retval_eal;

        if (first_init_flag) {
                /* Keep these for if we need to start another copy */
                first_argc = argc;
                first_argv = argv;
                first_nf_tag = nf_tag;
                first_init_flag = 0;
        }


        /* Reset getopt global variables opterr and optind to their default values */
        opterr = 0; optind = 1;

        initial_instance_id = (uint16_t)NF_NO_ID;
        if ((retval_parse = onvm_nflib_parse_args(argc, argv)) < 0)
                rte_exit(EXIT_FAILURE, "Invalid command-line arguments\n");

        /*
         * Calculate the offset that the nf will use to modify argc and argv for its
         * getopt call. This is the sum of the number of arguments parsed by
         * rte_eal_init and parse_nflib_args. This will be decremented by 1 to assure
         * getopt is looking at the correct index since optind is incremented by 1 each
         * time "--" is parsed.
         * This is the value that will be returned if initialization succeeds.
         */
        retval_final = (retval_eal + retval_parse) - 1;

        /* Reset getopt global variables opterr and optind to their default values */
        opterr = 0; optind = 1;

        /* Lookup mempool for nf_info struct */
        nf_info_mp = rte_mempool_lookup(_NF_MEMPOOL_NAME);
        if (nf_info_mp == NULL)
                rte_exit(EXIT_FAILURE, "No Client Info mempool - bye\n");

        /* Lookup mempool for NF messages */
        nf_msg_pool = rte_mempool_lookup(_NF_MSG_POOL_NAME);
        if (nf_msg_pool == NULL)
                rte_exit(EXIT_FAILURE, "No NF Message mempool - bye\n");

        /* Initialize the info struct */
        nf_info = onvm_nflib_info_init(nf_tag);
        *nf_info_p = nf_info;

        mp = rte_mempool_lookup(PKTMBUF_POOL_NAME);
        if (mp == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get mempool for mbufs\n");

        mz = rte_memzone_lookup(MZ_CLIENT_INFO);
        if (mz == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get tx info structure\n");
        nf_info->tx_stats = mz->addr;

	mz_scp = rte_memzone_lookup(MZ_SCP_INFO);
	if (mz_scp == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get service chain info structre\n");
	scp = mz_scp->addr;
	default_chain = *scp;

	onvm_sc_print(default_chain);

        mgr_msg_queue = rte_ring_lookup(_MGR_MSG_QUEUE_NAME);
        if (mgr_msg_queue == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get nf_info ring");

        /* Put this NF's info struct onto queue for manager to process startup */
        if (rte_mempool_get(nf_msg_pool, (void**)(&startup_msg)) != 0) {
                rte_mempool_put(nf_info_mp, nf_info); // give back mermory
                rte_exit(EXIT_FAILURE, "Cannot create startup msg");
        }

        startup_msg->msg_type = MSG_NF_STARTING;
        startup_msg->msg_data = nf_info;
        if (rte_ring_enqueue(mgr_msg_queue, startup_msg) < 0) {
                rte_mempool_put(nf_info_mp, nf_info); // give back mermory
                rte_mempool_put(nf_msg_pool, startup_msg);
                rte_exit(EXIT_FAILURE, "Cannot send nf_info to manager");
        }

        /* Wait for a client id to be assigned by the manager */
        RTE_LOG(INFO, APP, "Waiting for manager to assign an ID...\n");
        for (; nf_info->status == (uint16_t)NF_WAITING_FOR_ID ;) {
                sleep(1);
        }

        /* This NF is trying to declare an ID already in use. */
        if (nf_info->status == NF_ID_CONFLICT) {
                rte_mempool_put(nf_info_mp, nf_info);
                rte_exit(NF_ID_CONFLICT, "Selected ID already in use. Exiting...\n");
        } else if(nf_info->status == NF_NO_IDS) {
                rte_mempool_put(nf_info_mp, nf_info);
                rte_exit(NF_NO_IDS, "There are no ids available for this NF\n");
        } else if(nf_info->status != NF_STARTING) {
                rte_mempool_put(nf_info_mp, nf_info);
                rte_exit(EXIT_FAILURE, "Error occurred during manager initialization\n");
        }
        RTE_LOG(INFO, APP, "Using Instance ID %d\n", nf_info->instance_id);
        RTE_LOG(INFO, APP, "Using Service ID %d\n", nf_info->service_id);

        /* Now, map rx and tx rings into client space */
        nf_info->rx_ring = rte_ring_lookup(get_rx_queue_name(nf_info->instance_id));
        if (nf_info->rx_ring == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get RX ring - is server process running?\n");

        nf_info->tx_ring = rte_ring_lookup(get_tx_queue_name(nf_info->instance_id));
        if (nf_info->tx_ring == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get TX ring - is server process running?\n");

        nf_info->nf_msg_ring = rte_ring_lookup(get_msg_queue_name(nf_info->instance_id));
        if (nf_info->nf_msg_ring == NULL)
                rte_exit(EXIT_FAILURE, "Cannot get nf msg ring");

        /* Tell the manager we're ready to recieve packets */
        keep_running = 1;

        RTE_LOG(INFO, APP, "Finished Process Init.\n");
        return retval_final;
}


int
onvm_nflib_run(
        struct onvm_nf_info* info,
        pkt_handler handler)
{
        void *pkts[PKT_READ_SIZE];
        int ret;

        /* Don't allow conflicting NF modes */
        if (info->nf_mode == NF_MODE_RING) {
                return -1;
        }
        info->nf_mode = NF_MODE_SINGLE;

        printf("\nClient process %d handling packets\n", info->instance_id);

        /* Listen for ^C and docker stop so we can exit gracefully */
        signal(SIGINT, onvm_nflib_handle_signal);
        signal(SIGTERM, onvm_nflib_handle_signal);

        if (info->nf_pkt_function == NULL) {
                info->nf_pkt_function = handler;
        }

        printf("Sending NF_READY message to manager...\n");
        ret = onvm_nflib_nf_ready(info);
        if (ret != 0) rte_exit(EXIT_FAILURE, "Unable to message manager\n");

        printf("[Press Ctrl-C to quit ...]\n");
        for (; keep_running;) {
                onvm_nflib_dequeue_packets(pkts, info, handler);
                onvm_nflib_dequeue_messages(info);
        }

        // Stop and free
        onvm_nflib_cleanup(info);

        return 0;
}


int
onvm_nflib_return_pkt(struct onvm_nf_info *info, struct rte_mbuf* pkt) {
        /* FIXME: should we get a batch of buffered packets and then enqueue? Can we keep stats? */
        if(unlikely(rte_ring_enqueue(info->tx_ring, pkt) == -ENOBUFS)) {
                rte_pktmbuf_free(pkt);
                info->tx_stats->tx_drop[info->instance_id]++;
                return -ENOBUFS;
        }
        else info->tx_stats->tx_returned[info->instance_id]++;
        return 0;
}

int
onvm_nflib_nf_ready(struct onvm_nf_info *info) {
        struct onvm_nf_msg *startup_msg;
        int ret;

        /* Put this NF's info struct onto queue for manager to process startup */
        ret = rte_mempool_get(nf_msg_pool, (void**)(&startup_msg));
        if (ret != 0) return ret;

        startup_msg->msg_type = MSG_NF_READY;
        startup_msg->msg_data = info;
        ret = rte_ring_enqueue(mgr_msg_queue, startup_msg);
        if (ret < 0) {
                rte_mempool_put(nf_msg_pool, startup_msg);
                return ret;
        }
        return 0;
}

int
onvm_nflib_handle_msg(struct onvm_nf_info *info, struct onvm_nf_msg *msg) {
        switch(msg->msg_type) {
        case MSG_STOP:
                RTE_LOG(INFO, APP, "NF %u Shutting down...\n", info->instance_id);
                keep_running = 0;
                break;
        case MSG_SCALE:
                RTE_LOG(INFO, APP, "NF %u received scale message...\n", info->instance_id);
                onvm_nflib_scale(info);
                break;
        case MSG_NOOP:
        default:
                break;
        }

        return 0;
}

void
onvm_nflib_stop(struct onvm_nf_info *info) {
        onvm_nflib_cleanup(info);
}


struct rte_ring *
onvm_nflib_get_tx_ring(struct onvm_nf_info* info) {
        /* Don't allow conflicting NF modes */
        if (info->nf_mode == NF_MODE_SINGLE) {
                return NULL;
        }

        /* We should return the tx_ring associated with the info struct */
        info->nf_mode = NF_MODE_RING;
        return info->tx_ring;
}


struct rte_ring *
onvm_nflib_get_rx_ring(struct onvm_nf_info* info) {
        /* Don't allow conflicting NF modes */
        if (info->nf_mode == NF_MODE_SINGLE) {
                return NULL;
        }

        /* We should return the rx_ring associated with the info struct */
        info->nf_mode = NF_MODE_RING;
        return info->rx_ring;
}


volatile struct client_tx_stats *
onvm_nflib_get_tx_stats(struct onvm_nf_info* info) {
        /* Don't allow conflicting NF modes */
        if (info->nf_mode == NF_MODE_SINGLE) {
                return NULL;
        }

        /* We should return the tx_stats associated with the info struct */
        info->nf_mode = NF_MODE_RING;
        return info->tx_stats;
}


/******************************Helper functions*******************************/


static inline void
onvm_nflib_dequeue_packets(void **pkts, struct onvm_nf_info *info, pkt_handler handler) {
        struct onvm_pkt_meta* meta;
        uint16_t i, j, nb_pkts;
        void *pktsTX[PKT_READ_SIZE];
        int tx_batch_size = 0;
        int ret_act;

	/* Dequeue all packets in ring up to max possible. */
	nb_pkts = rte_ring_dequeue_burst(info->rx_ring, pkts, PKT_READ_SIZE);

        if(unlikely(nb_pkts == 0)) {
                return;
        }

        /* Give each packet to the user proccessing function */
        for (i = 0; i < nb_pkts; i++) {
                meta = onvm_get_pkt_meta((struct rte_mbuf*)pkts[i]);
                ret_act = (*handler)((struct rte_mbuf*)pkts[i], meta);
                /* NF returns 0 to return packets or 1 to buffer */
                if(likely(ret_act == 0)) {
                        pktsTX[tx_batch_size++] = pkts[i];
                } else {
                        info->tx_stats->tx_buffer[info->instance_id]++;
                }
        }

        if (unlikely(tx_batch_size > 0 && rte_ring_enqueue_bulk(info->tx_ring, pktsTX, tx_batch_size) == -ENOBUFS)) {
                info->tx_stats->tx_drop[info->instance_id] += tx_batch_size;
                for (j = 0; j < tx_batch_size; j++) {
                        rte_pktmbuf_free(pktsTX[j]);
                }
        } else {
                info->tx_stats->tx[info->instance_id] += tx_batch_size;
        }
}

static inline void
onvm_nflib_dequeue_messages(struct onvm_nf_info *info) {
        struct onvm_nf_msg *msg;

        // Check and see if this NF has any messages from the manager
        if (likely(rte_ring_count(info->nf_msg_ring) == 0)) {
                return;
        }
        msg = NULL;
        rte_ring_dequeue(info->nf_msg_ring, (void**)(&msg));
        onvm_nflib_handle_msg(info, msg);
        rte_mempool_put(nf_msg_pool, (void*)msg);
}

static inline void
onvm_nflib_scale(struct onvm_nf_info *info) {
        unsigned current;
        unsigned core;
        enum rte_lcore_state_t state;
        int ret;

        if (info->headroom == 0) {
                RTE_LOG(INFO, APP, "No cores available to scale\n");
                return;
        }

        current = rte_lcore_id();
        if (current != rte_get_master_lcore()) {
                RTE_LOG(INFO, APP, "Can only scale from the master lcore\n");
                return;
        }

        if (info->nf_mode != NF_MODE_SINGLE) {
                RTE_LOG(INFO, APP, "Can only scale NFs running in single mode\n");
                return;
        }

        // Find the next available lcore to use
        RTE_LOG(INFO, APP, "Currently running on core %u\n", current);
        for (core = rte_get_next_lcore(current, 1, 0); core != RTE_MAX_LCORE; core = rte_get_next_lcore(core, 1, 0)) {
                state = rte_eal_get_lcore_state(core);
                if (state != RUNNING) {
                        RTE_LOG(INFO, APP, "Able to scale to core %u\n", core);
                        ret = rte_eal_remote_launch(&onvm_nflib_start_child, info, core);
                        if (ret == -EBUSY) {
                                RTE_LOG(INFO, APP, "Core is %u busy, skipping...\n", core);
                                continue;
                        }
                        return;
                }
        }

        RTE_LOG(INFO, APP, "No cores available to scale\n");
}

// The entry point for a child NF
static int
onvm_nflib_start_child(void *arg) {
        struct onvm_nf_info *parent_info;
        struct onvm_nf_info *child_info;
        int ret;

        parent_info = (struct onvm_nf_info *)arg;
        parent_info->headroom--;
        RTE_LOG(INFO, APP, "Starting another copy of service %u, new headroom: %u\n", parent_info->service_id, parent_info->headroom);
        ret = onvm_nflib_init(first_argc, first_argv, first_nf_tag, &child_info);
        if (ret < 0) {
                RTE_LOG(INFO, APP, "Unable to init new NF, exiting...\n");
                return -1;
        }

        onvm_nflib_run(child_info, parent_info->nf_pkt_function);
        return 0;
}

static struct onvm_nf_info *
onvm_nflib_info_init(const char *tag)
{
        void *mempool_data;
        struct onvm_nf_info *info;

        if (rte_mempool_get(nf_info_mp, &mempool_data) < 0) {
                rte_exit(EXIT_FAILURE, "Failed to get client info memory");
        }

        if (mempool_data == NULL) {
                rte_exit(EXIT_FAILURE, "Client Info struct not allocated");
        }

        info = (struct onvm_nf_info*) mempool_data;
        info->instance_id = initial_instance_id;
        info->service_id = service_id;
        info->status = NF_WAITING_FOR_ID;
        info->tag = tag;

        // Set core headroom. This is the number of excess cores we have
        // or 0, if this is not the master core
        info->headroom = rte_get_master_lcore() == rte_lcore_id()
                ? rte_lcore_count() - 1
                : 0;

        return info;
}


static void
onvm_nflib_usage(const char *progname) {
        printf("Usage: %s [EAL args] -- "
               "[-n <instance_id>]"
               "[-r <service_id>]\n\n", progname);
}


static int
onvm_nflib_parse_args(int argc, char *argv[]) {
        const char *progname = argv[0];
        int c;

        opterr = 0;
        while ((c = getopt (argc, argv, "n:r:")) != -1)
                switch (c) {
                case 'n':
                        initial_instance_id = (uint16_t) strtoul(optarg, NULL, 10);
                        break;
                case 'r':
                        service_id = (uint16_t) strtoul(optarg, NULL, 10);
                        // Service id 0 is reserved
                        if (service_id == 0) service_id = -1;
                        break;
                case '?':
                        onvm_nflib_usage(progname);
                        if (optopt == 'n')
                                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                        else if (isprint(optopt))
                                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
                        else
                                fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
                        return -1;
                default:
                        return -1;
                }

        if (service_id == (uint16_t)-1) {
                /* Service ID is required */
                fprintf(stderr, "You must provide a nonzero service ID with -r\n");
                return -1;
        }
        return optind;
}


static void
onvm_nflib_handle_signal(int sig)
{
        if (sig == SIGINT || sig == SIGTERM)
                keep_running = 0;
}

static void
onvm_nflib_cleanup(struct onvm_nf_info *nf_info)
{
        struct onvm_nf_msg *shutdown_msg;
        nf_info->status = NF_STOPPED;

        /* Put this NF's info struct back into queue for manager to ack shutdown */
        RTE_LOG(INFO, APP, "Shutting down NF %u\n", nf_info->instance_id);
        if (mgr_msg_queue == NULL) {
                rte_mempool_put(nf_info_mp, nf_info); // give back mermory
                rte_exit(EXIT_FAILURE, "Cannot get nf_info ring for shutdown");
        }
        if (rte_mempool_get(nf_msg_pool, (void**)(&shutdown_msg)) != 0) {
                rte_mempool_put(nf_info_mp, nf_info); // give back mermory
                rte_exit(EXIT_FAILURE, "Cannot create shutdown msg");
        }

        shutdown_msg->msg_type = MSG_NF_STOPPING;
        shutdown_msg->msg_data = nf_info;

        if (rte_ring_enqueue(mgr_msg_queue, shutdown_msg) < 0) {
                rte_mempool_put(nf_info_mp, nf_info); // give back mermory
                rte_mempool_put(nf_msg_pool, shutdown_msg);
                rte_exit(EXIT_FAILURE, "Cannot send nf_info to manager for shutdown");
        }

}
