#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>

#include "include/libbpf/src/bpf.h"
#include "include/libbpf/src/libbpf.h"
#include "include/common.h"

// TC CMD sizes.
#define CMD_MAX 2048
#define CMD_MAX_TC 256

// Initialize static variables.
static uint8_t cont = 1;
static int interface_map_fd;
static int mac_map_fd;
static uint8_t gwMAC[ETH_ALEN];
static char tc_cmd[CMD_MAX_TC] = "tc";

// TC program file name.
const char TCFile[] = "/etc/IPIPDirect/IPIPDirect_filter.o";

// Maps.
const char *map_interface = BASEDIR_MAPS "/interface_map";
const char *map_mac = BASEDIR_MAPS "/mac_map";

// Extern error number.
extern int errno;

// Signal function.
void signHdl(int tmp)
{
    // Set cont to 0 which will stop the while loop and the program.
    cont = 0;
}

// Get gateway MAC address.
void GetGatewayMAC()
{
    // Command to run.
    char cmd[] = "ip neigh | grep \"$(ip -4 route list 0/0 | cut -d' ' -f3) \" | cut -d' ' -f5 | tr '[a-f]' '[A-F]'";

    // Execute command.
    FILE *fp =  popen(cmd, "r");

    // Check if command is valid.
    if (fp != NULL)
    {
        // Initialize line char.
        char line[18];

        // Get output from command.
        if (fgets(line, sizeof(line), fp) != NULL)
        {
            // Parse output and put it into gwMAC.
            sscanf(line, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &gwMAC[0], &gwMAC[1], &gwMAC[2], &gwMAC[3], &gwMAC[4], &gwMAC[5]);
        }
        
        // Close command.
        pclose(fp);
    }
}

int open_map(const char *name)
{
    // Initialize FD.
    int fd;

    // Get map objective.
    fd = bpf_obj_get(name);

    // Check map FD.
    if (fd < 0)
    {
        fprintf(stderr, "Error getting map. Map name => %s\n", name);

        return fd;
    }

    // Return FD.
    return fd;
}

int tc_egress_attach_bpf(const char *dev, const char *bpf_obj, const char *sec_name)
{
    // Initialize variables.
	char cmd[CMD_MAX];
	int ret = 0;

	// Delete clsact which also deletes existing filters.

    // Set cmd to all 0's.
	memset(&cmd, 0, CMD_MAX);

    // Format command.
	snprintf(cmd, CMD_MAX, "%s qdisc del dev %s clsact 2> /dev/null", tc_cmd, dev);

    // Call system command.
	ret = system(cmd);

    // Check if command executed.
	if (!WIFEXITED(ret)) 
    {
		fprintf(stderr, "Error attaching TC egress filter. Cannot execute TC command when removing clsact. Command => %s and Return Error Number => %d.\n", cmd, WEXITSTATUS(ret));
	}

	// Create clsact.

    // Set cmd to all 0's.
	memset(&cmd, 0, CMD_MAX);

    // Format command.
	snprintf(cmd, CMD_MAX, "%s qdisc add dev %s clsact", tc_cmd, dev);

    // Call system command.
	ret = system(cmd);

    // Check if command executed.
	if (ret) 
    {
		fprintf(stderr, "Error attaching TC egress filter. TC cannot create a clsact. Command => %s and Return Error Number => %d.\n", cmd, WEXITSTATUS(ret));
		
        exit(1);
	}

	// Attach to egress filter.

    // Set cmd to all 0's.
	memset(&cmd, 0, CMD_MAX);

    // Format command.
	snprintf(cmd, CMD_MAX, "%s filter add dev %s egress prio 1 handle 1 bpf da obj %s sec %s", tc_cmd, dev, bpf_obj, sec_name);

    // Call system command.
	ret = system(cmd);

    // Check if command executed.
	if (ret) 
    {
		fprintf(stderr, "Error attaching TC egress filter. TC cannot attach to filter. Command => %s and Return Error Number => %d.\n", cmd, WEXITSTATUS(ret));

		exit(1);
	}

    // Return error or not.
	return ret;
}

int tc_remove_egress_filter(const char* dev)
{
    // Initialize starting variables.
	char cmd[CMD_MAX];
	int ret = 0;

    // Set cmd to all 0's.
	memset(&cmd, 0, CMD_MAX);

    // Format command.
	snprintf(cmd, CMD_MAX, "%s filter delete dev %s egress", tc_cmd, dev);

    // Call system command.
	ret = system(cmd);

    // Check if command executed.
	if (ret) 
    {
		fprintf(stderr, "Error detaching TC egress filter. Command => %s and Return Error Number => %d.\n", cmd, ret);
		
        exit(1);
	}

    // Return error or not.
	return ret;
}

int main(int argc, char *argv[])
{
    // Check argument count.
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <Interface> [<Interface IP>]\n", argv[0]);

        exit(1);
    }

    // Initialize variables.
    int err, ifindex;

    // Get interface index.
    ifindex = if_nametoindex(argv[1]);

    // Check if interface is valid.
    if (ifindex <= 0)
    {
        fprintf(stderr, "Error loading interface (%s).\n", argv[1]);

        exit(1);
    }

    // Attempt to attach to egress filter.
    err = tc_egress_attach_bpf(argv[1], TCFile, "egress");

    if (err)
    {
        exit(err);
    }

    // Get maps.
    interface_map_fd = open_map(map_interface);

    if (interface_map_fd < 0)
    {
        exit(interface_map_fd);
    }

    mac_map_fd = open_map(map_mac);

    if (mac_map_fd < 0)
    {
        exit(mac_map_fd);
    }

    // Get IP address of interface and initialize starting variables.
    int sockfd;
    char *ip;
    struct ifreq ifr;

    // Make a temporary socket.
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);

    // Check if socket is valid.
    if (sockfd < 0)
    {
        fprintf(stderr, "Error creating socket to get IP address. Error => %s. Error Num => %d. Attempting to use command line.\n", strerror(errno), errno);

        // Check if we have more than one argument.
        if (argc < 3)
        {
            fprintf(stderr, "No IP specified.\n");
            
            exit(1);
        }

        // We do, so just make the interface IP that.
        ip = argv[2];
    }

    // Check socket again.
    if (sockfd)
    {
        // Start filling out ifr variable.
        ifr.ifr_addr.sa_family = AF_INET;

        // Copy interface name.
        strcpy(ifr.ifr_name, argv[1]);

        // To ioctl on socket to get IP address and check.
        if (ioctl(sockfd, SIOCGIFADDR, &ifr) < 0)
        {
            fprintf(stderr, "Error using ioctl(). Error => %s. Error Num => %d. Resorting to command line.\n", strerror(errno), errno);

            // Check if we have more than one argument.
            if (argc < 3)
            {
                fprintf(stderr, "No IP specified.\n");
                
                exit(1);
            }

            // We do, so just make the interface IP that.
            ip = argv[2];
        }
        else
        {
            // Assign IP to interface's IP.
            ip = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
        }
    }

    // Close temporary socket if open.
    close(sockfd);

    // Get gateway MAC address and store it in gwMAC.
    GetGatewayMAC();

    // Add IP to the "interface_map" BPF map.
    uint32_t ipAddr = inet_addr(ip);
    uint32_t key = 0;

    bpf_map_update_elem(interface_map_fd, &key, &ipAddr, BPF_ANY);

    // Add gateway MAC address to the "mac_map" BPF map.
    uint64_t val;
    val = mac2int(gwMAC);

    uint32_t key2 = 0;

    bpf_map_update_elem(mac_map_fd, &key2, &val, BPF_ANY);

    // Signal calls so we can shutdown program.
    signal(SIGINT, signHdl);
    signal(SIGTERM, signHdl);
    signal(SIGKILL, signHdl);

    // Debug.
    fprintf(stdout, "Starting IPIP Direct TC egress program. Interface address => %s.\n", ip);

    // Loop!
    while (cont)
    {
        // We sleep every second.
        sleep(1);
    }

    // Debug.
    fprintf(stdout, "Cleaning up...\n");

    // Remove TC egress filter.
    err = tc_remove_egress_filter(argv[1]);

    // Check for errors.
    if (err)
    {
        exit(err);
    }

    exit(0);
}