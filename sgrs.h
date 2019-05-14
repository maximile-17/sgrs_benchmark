#define _XOPEN_SOURCE 600 //The X/Open Portability Guide

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <mpi.h>

#include <infiniband/verbs.h>

#define IB_DEV   (1) //change 0-1 because there are 2 mlx5 card ,the second is useable
#define IB_PORT  (1)
#define CQ_DEPTH (64)
#define WR_ID    (519)

#define MAX_SIZE (64)

#define BLOCK_SZ (4096)
#define BLOCK_N  (30)              // MLX5 CX-5
#define STRIDE   (45*1024*1024)    // LLC of E5-2699 v3

#define NITER    (10000)
#define WITER    (1000)
#define TICKS_PER_USEC  (2300)     // E5-2699 v3

struct ib_conn{
	uint32_t  lid;
	uint32_t  qpn;
	uint32_t  psn;
	uint8_t gid[16];  //used for ehternet link,RoCE
};

enum sg_test {
	SGRS,     // 0 copy, one send/recv
	SR_COPY,  // n copy, one send/recv
	TEST_END
};
//static int block_size, block_num, stride;
struct params{
	int block_size;
	int block_num;
	int stride;
	int iterW;
	int iterN;
	bool IBlink;
	bool Ethlink;
	int mtu;
};

/*parameters for user to set, printed when -h*/
const char *paraminfo[] = {
  "  -h, --help       	   print help and exit",
  "	 -b, --block_size=INT  number of elements in one block(default='4096')"
  "  -n, --block_num=INT   number of blocks(default=`30')",
  "  -s, --stride=INT  	   space between two blocks(B)(default=`45*1024*1024')",
  "  -W, --iterW=INT  	   Warm up iterations(default=`1000')",
  "  -N, --iterN=INT  	   calculate iterations(default=`10000'),<=10000",
  "  -I, --IBlink=BOOL	   whether IB link_layer(default='1')",
  "  -E, --Ethlink=BOOL    whether Ethernet link_layer(default='0')",
  "  -m, --mtu=INT		   must equal to active_mtu(default=1024)",
    0
};

static union ibv_gid mygid;
////////////////////////////////////
static int buf_size;
static void *buf_sg, *buf_cp;

static struct ibv_device     **dev_list;
static struct ibv_device      *dev;
static struct ibv_context     *dev_ctx;
static struct ibv_device_attr  dev_attr;
static struct ibv_port_attr    port_attr;

static struct ibv_pd          *pd;
static struct ibv_mr          *mr;

static struct ibv_cq          *cq;
static struct ibv_qp          *qp;
static struct ibv_qp_init_attr qp_init_attr;
static struct ibv_qp_attr      qp_attr;

static struct ibv_sge          sg_list[MAX_SIZE];
static struct ibv_send_wr      sr;
static struct ibv_recv_wr      rr;

static struct ib_conn local_conn, remote_conn;

static uint64_t ticks[NITER], min_tick, max_tick, copyticks[NITER], nicticks[NITER];
static uint64_t rdtsc();	//use TSC to count time
static void print_timing(int itern);
int parser(int argc, char *const *argv, params *parameters, int np, int myid);
void print_help (void);

static void print_timing(int itern)
{
	int i, count, total;
	int lat, lats[NITER];
	uint64_t total_tick;

	total_tick = 0;
	for (i = 0; i < itern; i++) {
		total_tick += ticks[i];
		lats[i] = ticks[i] / TICKS_PER_USEC;
	}

	printf("total avg: %.3f us | min: %.3f us | max: %.3f us\n",
	       (float) total_tick / itern / TICKS_PER_USEC,
	       (float) min_tick / TICKS_PER_USEC,
	       (float) max_tick / TICKS_PER_USEC);

	total = 0;
	for (lat = min_tick / TICKS_PER_USEC; lat <= max_tick / TICKS_PER_USEC; lat++) {
		count = 0;
		for (i = 0; i < itern; i++) {
			if (lat == lats[i]) count++;
		}
		total += count;
		if (count) printf("\t%-3d : %-5d (cdf %.4f%%)\n", lat, count, (float) total * 100. / NITER);
	}
}

static uint64_t rdtsc()
{
	uint32_t lo, hi;
	__asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
	return (((uint64_t)hi << 32) | lo);
}


/*print help message to show parameters info*/
void
print_help (void)
{
  int i = 0;
  while (paraminfo[i])
    printf("%s\n", paraminfo[i++]);
}
/*parser the parameters user adding*/
int parser(int argc, char * const *argv, struct params *parameters, int np, int myid)
{
/////////////////////////////////////////////////////////////
int opt;
    while((opt = getopt(argc, argv, "hb:n:s:W:N:IEm:"))!= -1){
        switch(opt){
			case 'h':
				if(myid == 0){
					 print_help();
				}
                break;
            case 'b':
                parameters->block_size = strtol(optarg, NULL, 0);
                break;
            case 'n':
                parameters->block_num = strtol(optarg, NULL, 0);
                break;
            case 's':
                parameters->stride = strtol(optarg, NULL, 0);
                break;
            case 'W':
                parameters->iterW = strtol(optarg, NULL, 0);
                break;
            case 'N':
                parameters->iterN = strtol(optarg, NULL, 0);
                break;
            case 'I':
                parameters->IBlink = 1;
				break;
			case 'E':
                parameters->Ethlink = 1;
				break;
			case 'm':
                parameters->mtu = strtol(optarg, NULL, 0);
                break;	
            default:
                if(myid == 0){
                  printf("invalid argument!\n");
                  print_help();
                }
                return 1;
        }
      }
    if(myid == 0){
      printf("------------------parameters_info-------------------------\n \
	  number of process:%#3d\n \
		-b, --block_size=%#7d\n \
  		-n, --block_num=%#8d\n \
		-s, --stride= %#10d\n \
		-W, --iterW=  %#10d\n \
		-N, --iterN=  %#10d\n \
		-I, --IBlink= %#10d\n \
		-E, --Ethlink=%#10d\n \
		-m, --mtu=    %#10d\n",\
	 np, parameters->block_size, parameters->block_num, parameters->stride, \
	  parameters->iterW, parameters->iterN, parameters->IBlink, parameters->Ethlink, \
	parameters->mtu);
	  
    }
    if(np != 2) { 
      printf("this test requires exactly two ranks!!!\n");
      return 1;
    }
   return 0;
}
////////////////////////////parameters->/////////////////////////////////
