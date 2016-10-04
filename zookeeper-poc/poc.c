#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <zookeeper/zookeeper.h>

#include "watcher.c"

/**
 * A ZooKeeper Proof-of-Concept program to benchmark speeds across a cluster of VMs
 * run with ./poc <read set size> <write set size> <percent of writes>
 * Defaults to reading/writing on localhost:2181
 */

// Max length of a value, path
#define VALUE_LENGTH 20
#define PATH_LENGTH 5
#define OPERATIONS 100

#define NODE_FORMAT "/a%d"
#define TIMER_START() clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start)
#define TIMER_STOP() clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end)

void
rand_str(char *dest, size_t length)
{
    char charset[] = "0123456789"
                     "abcdefghipqrstuvwxyz"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (length-- > 0) {
        size_t index = (double) rand() / RAND_MAX * (sizeof charset - 1);
        *dest++ = charset[index];
    }
    *dest = '\0';
}

int
main(int argc, char **argv)
{
    int read_set_size;
    int write_set_size;
    int write_threshold;
    double write_percent;
    zhandle_t *zh;
    char *path;
    char *val;
    struct Stat stat;
    int status;
    struct timespec start;
    struct timespec end;

    if (argc != 4) {
        fprintf(stderr, "Bad args\n");
        return -1;
    }

    srand(time(NULL));
    is_connected = 0;
    val = malloc(VALUE_LENGTH);
    path = malloc(PATH_LENGTH);
    read_set_size = atoi(argv[1]);
    write_set_size = atoi(argv[2]);
    write_percent = atof(argv[3]);
    write_threshold = write_percent * RAND_MAX;

    // Connect to zookeeper
    zh = zookeeper_init("localhost:2181", watcher, 3000, &myid, NULL, 0);
    if (!zh) {
        perror("Unable to connect to zookeeper\n");
        return errno;
    }

    // Wait for us to be connected
    sleep(1);
    while (zoo_state(zh) != ZOO_CONNECTED_STATE) {
        printf("Zookeeper State is %d: %s\n", zoo_state(zh), state2String(zoo_state(zh)));
        sleep(5);
    }

    // Create our parent node
    //if (zoo_exists(zh, NODE_PARENT, 0, stat) == ZNONODE) {
    //    status = zoo_create(zh, NODE_PARENT, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0, NULL, 0);
    //}

    // Create max(read_set_size, write_set_size) nodes if they don't already exist
    for (int i = 0; i < (read_set_size > write_set_size ? read_set_size : write_set_size); i++) {
        sprintf(path, NODE_FORMAT, i);
        if (zoo_exists(zh, path, 0, &stat) == ZNONODE) {
            rand_str(val, VALUE_LENGTH - 1);
            status = zoo_create(zh, path, val, VALUE_LENGTH, &ZOO_OPEN_ACL_UNSAFE, 0, NULL, 0);
            if (status != ZOK) {
                fprintf(stderr, "Unable to create node %s: %s\n", path, status2String(status));
            } else {
                printf("Created node %s with data %s\n", path, val);
            }
        } else {
            fprintf(stderr, "Node %s already exists\n", path);
        }
    }

    long write_average = 0;
    long read_average = 0;
    int len;
    double write_weight = 1.0 / (OPERATIONS * write_percent);
    double read_weight = 1.0 / (OPERATIONS * (1 - write_percent));
    for (int i = 0; i < OPERATIONS; i++) {
        if (rand() < write_threshold) {
            // Write to a node
            sprintf(path, NODE_FORMAT, rand() % write_set_size);
            rand_str(val, VALUE_LENGTH - 1);
            TIMER_START();
            status = zoo_set(zh, path, val, VALUE_LENGTH, -1);
            TIMER_STOP();
            if (status == ZOK) {
                write_average += write_weight * (end.tv_nsec - start.tv_nsec);
            } else {
                fprintf(stderr, "Setting %s failed: %s\n", path, status2String(status));
            }
        } else {
            // Read from a node
            sprintf(path, NODE_FORMAT, rand() % read_set_size);
            len = VALUE_LENGTH;
            TIMER_START();
            status = zoo_get(zh, path, 0, val, &len, &stat);
            TIMER_STOP();
            if (status == ZOK) {
                read_average += read_weight * (end.tv_nsec - start.tv_nsec);
            } else {
                fprintf(stderr, "Getting %s failed: %s\n", path, status2String(status));
            }
        }
    }

    printf("Average read time: %lu ns\n", read_average);
    printf("Average write time: %lu ns\n", write_average);

    // Delete nodes
    for (int i = 0; i < (read_set_size > write_set_size ? read_set_size : write_set_size); i++) {
        sprintf(path, NODE_FORMAT, i);
        zoo_delete(zh, path, -1);
    }

    zookeeper_close(zh);
}
